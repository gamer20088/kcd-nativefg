// Offline test of encore_uir's D3D11 -> D3D12 handoff (no game, no Encore): a D3D11 device and a D3D12 device on
// the same adapter; the D3D12 queue stands in for the present route's guides queue and a fake slSetTagForFrame for
// Encore's pointer. Per frame it checks that the HUD-less texture passed to Encore's tag call and the UI layer
// tagged as kBufferTypeUIColorAndAlpha hold that frame's D3D11 images, that the ring rotates, and that the release
// fence frees slots.
//
// x64 developer prompt, from build-uir\test:
//   cl /nologo /W4 /std:c++20 /EHsc /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I..\..\src /I..\dlss-sdk-v310.9.1\include
//      /I..\..\build\_deps\minhook-src\include ..\..\tests\encore_uir_interop.cpp /Fe:encore_uir_interop.exe
//      /link ..\minhook.lib d3d11.lib d3d12.lib dxgi.lib user32.lib advapi32.lib
#include "../src/encore/encore_uir.cpp"

#include <dxgi1_4.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace kc {
void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("  log: ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}
}  // namespace kc

using namespace kc::encore_uir;

static int g_fail;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) {                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            ++g_fail;                                   \
        }                                               \
    } while (0)

constexpr UINT W = 64, H = 48;
static uint32_t pattern(UINT x, UINT y, uint32_t frame, uint32_t salt) {
    return 0xFF000000u | ((frame * 0x10101u) ^ (x << 8) ^ y ^ salt);
}

// Fake Streamline slSetTagForFrame: records the last tag.
static struct {
    int calls = 0;
    uint32_t type = ~0u, lifecycle = ~0u, w = 0, h = 0, state = 0;
    void* resource = nullptr;
    const void* frame = nullptr;
    void* cmd = nullptr;
} g_tag;
static int __fastcall fake_set_tag(const void* frame, const void*, const void* tags, uint32_t n, void* cmd) {
    auto* t = static_cast<const uint8_t*>(tags);
    auto* res = *reinterpret_cast<uint8_t* const*>(t + 0x20);
    ++g_tag.calls;
    g_tag.frame = frame;
    g_tag.cmd = cmd;
    g_tag.type = *reinterpret_cast<const uint32_t*>(t + 0x28);
    g_tag.lifecycle = *reinterpret_cast<const uint32_t*>(t + 0x2C);
    g_tag.w = *reinterpret_cast<const uint32_t*>(t + 0x38);
    g_tag.h = *reinterpret_cast<const uint32_t*>(t + 0x3C);
    g_tag.resource = *reinterpret_cast<void**>(res + 0x28);
    g_tag.state = *reinterpret_cast<const uint32_t*>(res + 0x40);
    return n == 1 && memcmp(t + 8, kResourceTagGuid, 16) == 0 && memcmp(res + 8, kResourceGuid, 16) == 0 ? 0 : 1;
}

struct Readback {
    ID3D12Resource* buf = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
};

int main() {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        printf("SKIP: no DXGI factory\n");
        return 0;
    }
    IDXGIAdapter1* adapter = nullptr;
    factory->EnumAdapters1(0, &adapter);
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    printf("adapter: %ls\n", ad.Description);
    ID3D12Device* dev12 = nullptr;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12)))) {
        printf("SKIP: no D3D12 device on adapter 0\n");
        return 0;
    }
    ID3D11Device* dev11 = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &dev11, nullptr, &ctx);
    CHECK(SUCCEEDED(hr), "D3D11CreateDevice 0x%08lx", hr);
    if (FAILED(hr))
        return 1;

    // Fake Encore image: only the slSetTagForFrame pointer is read through g_base.
    static void* fake_ptr = reinterpret_cast<void*>(&fake_set_tag);
    g_base = reinterpret_cast<uintptr_t>(&fake_ptr) - kSetTagPtrRva;
    uint8_t fake_slot[0x40] = {};
    static uint64_t frame_token = 0x1234;
    *reinterpret_cast<void**>(fake_slot + kSlotFrameToken) = &frame_token;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ID3D12CommandAllocator* alloc = nullptr;
    dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* list = nullptr;
    dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ID3D12Fence* done = nullptr;
    dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
    UINT64 done_value = 0;

    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = W;
    dd.Height = H;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R32_FLOAT;
    dd.SampleDesc.Count = 1;
    ID3D12Resource* depth = nullptr;
    dev12->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                   IID_PPV_ARGS(&depth));
    void* guides[13] = {queue, depth};

    auto make_readback = [&](Readback& rb) {
        D3D12_RESOURCE_DESC td = dd;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        UINT64 total = 0;
        dev12->GetCopyableFootprints(&td, 0, 1, 0, &rb.fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_PPV_ARGS(&rb.buf));
    };
    Readback rb_hud, rb_ui;
    make_readback(rb_hud);
    make_readback(rb_ui);

    // What Streamline's tag copies do (barrier from the declared state, copy), on the guides' queue.
    auto read_back = [&](ID3D12Resource* src, Readback& rb, uint32_t frame, uint32_t salt) {
        alloc->Reset();
        list->Reset(alloc, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{rb.buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = rb.fp;
        D3D12_TEXTURE_COPY_LOCATION from{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(done, ++done_value);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        done->SetEventOnCompletion(done_value, ev);
        const bool finished = WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0;
        CloseHandle(ev);
        if (!finished)
            return -1;
        uint8_t* p = nullptr;
        rb.buf->Map(0, nullptr, reinterpret_cast<void**>(&p));
        int bad = 0;
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x)
                bad += reinterpret_cast<uint32_t*>(p + y * rb.fp.Footprint.RowPitch)[x] != pattern(x, y, frame, salt);
        D3D12_RANGE none{0, 0};
        rb.buf->Unmap(0, &none);
        return bad;
    };

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = W;
    sd.Height = H;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;  // as swap-chain buffers often are
    sd.SampleDesc.Count = 1;
    sd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* src = nullptr;
    dev11->CreateTexture2D(&sd, nullptr, &src);
    sd.Format = kUiFormat;
    ID3D11Texture2D* ui = nullptr;
    dev11->CreateTexture2D(&sd, nullptr, &ui);

    g_active = true;
    std::vector<uint32_t> px(W * H);
    auto fill = [&](ID3D11Texture2D* t, uint32_t frame, uint32_t salt) {
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x)
                px[y * W + x] = pattern(x, y, frame, salt);
        ctx->UpdateSubresource(t, 0, nullptr, px.data(), W * 4, 0);
    };
    int tagged = 0;
    ID3D12Resource* seen[kRing + 1] = {};
    for (uint32_t frame = 1; frame <= 8; ++frame) {
        fill(src, frame, 0);
        copy_hudless(ctx, src);  // render thread, before the interface
        fill(ui, frame, 0x5A5A5A);
        finish_frame(ctx, ui);   // render thread, end of frame
        int slot = -1;
        ID3D12Resource* h = consume(guides, &slot);  // present thread, inside the present trial
        if (frame == 1) {
            CHECK(!h && slot < 0, "frame 1: ring is created on the present thread first, none expected yet");
            continue;
        }
        CHECK(h && slot >= 0, "frame %u: no HUD-less texture for the tag call", frame);
        if (!h)
            continue;
        if (frame == 4) {
            int s2 = -1;
            CHECK(!consume(guides, &s2), "second consume in one frame returned a stale image");
        }
        if (frame <= 5)
            seen[frame - 2] = h;
        const int calls = g_tag.calls;
        tag_ui_and_release(fake_slot, guides, slot);  // after Encore's own input copy
        CHECK(g_tag.calls == calls + 1, "frame %u: UI layer not tagged", frame);
        CHECK(g_tag.type == 23 && g_tag.lifecycle == 0 && g_tag.w == W && g_tag.h == H && g_tag.state == 0x40 &&
                  g_tag.frame == &frame_token && g_tag.cmd == g_s.list && g_tag.resource == g_s.ui12[slot],
              "frame %u: UI tag type %u life %u %ux%u state 0x%x frame %p cmd %p res %p", frame, g_tag.type,
              g_tag.lifecycle, g_tag.w, g_tag.h, g_tag.state, g_tag.frame, g_tag.cmd, g_tag.resource);
        int bad = read_back(h, rb_hud, frame, 0);
        CHECK(bad == 0, "frame %u: HUD-less: %d pixels differ from the D3D11 image of the same frame", frame, bad);
        bad = read_back(g_s.ui12[slot], rb_ui, frame, 0x5A5A5A);
        CHECK(bad == 0, "frame %u: UI layer: %d pixels differ from the D3D11 layer of the same frame", frame, bad);
        ++tagged;
    }
    CHECK(tagged == 7, "tagged %d frames, expected 7", tagged);
    CHECK(seen[0] != seen[1] && seen[1] != seen[2] && seen[0] != seen[2] && seen[3] == seen[0],
          "ring does not rotate over %d slots", kRing);
    CHECK(g_s.release12->GetCompletedValue() >= g_s.release_value && g_s.release_value == 7,
          "release fence %llu / %llu", g_s.release12->GetCompletedValue(), g_s.release_value);
    CHECK(g_s.reused_wait >= 4, "slot reuse waited %llu times", g_s.reused_wait);

    // Resize: new ring, then the next frame opens it on D3D11 again.
    sd.Width = W * 2;
    sd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    ID3D11Texture2D* big = nullptr;
    dev11->CreateTexture2D(&sd, nullptr, &big);
    sd.Format = kUiFormat;
    ID3D11Texture2D* big_ui = nullptr;
    dev11->CreateTexture2D(&sd, nullptr, &big_ui);
    const uint32_t gen_before = g_s.gen;
    int slot = -1;
    copy_hudless(ctx, big);
    finish_frame(ctx, big_ui);
    CHECK(!consume(guides, &slot), "resize frame: old-size texture must not be handed out");
    copy_hudless(ctx, big);
    finish_frame(ctx, big_ui);
    ID3D12Resource* h2 = consume(guides, &slot);
    CHECK(h2 && h2->GetDesc().Width == W * 2 && g_s.gen == gen_before + 1, "after resize: %p gen %u", h2, g_s.gen);

    printf(g_fail ? "encore_uir_interop: %d FAILED\n" : "encore_uir_interop: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
