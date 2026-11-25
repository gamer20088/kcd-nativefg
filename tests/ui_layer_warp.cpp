// Offline test (software D3D11) of hudless.cpp's UI-layer replay: interface draws replayed into the UI layer must give
// final = ui.rgb + (1 - ui.a) * hudless (the relation DLSS-G expects), leave the game's image as the game drew it, and
// restore the game's output-merger state.
//
// x64 developer prompt, from build-uir\test:
//   cl /nologo /W4 /std:c++20 /EHsc /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I..\..\src ..\..\tests\ui_layer_warp.cpp
//      /Fe:ui_layer_warp.exe /link d3d11.lib d3dcompiler.lib
#include "../src/renderer/hudless.cpp"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace kc {
void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("  log: ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}
static State g_test_state;
State& state() { return g_test_state; }
namespace encore_uir {
static bool t_alpha_only;
void copy_hudless(ID3D11DeviceContext*, ID3D11Resource*) {}
void finish_frame(ID3D11DeviceContext*, ID3D11Resource*) {}
bool ui_alpha_only() { return t_alpha_only; }
void set_fg_suspended(bool) {}
void on_frame() {}
}  // namespace encore_uir
namespace taa_dlss {
bool scene_last_frame() { return true; }
}  // namespace taa_dlss
namespace d3d11 {
ScopedInside::ScopedInside() {}
ScopedInside::~ScopedInside() {}
}  // namespace d3d11
}  // namespace kc

using namespace kc::hudless;

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

static const char kShaders[] = R"(
cbuffer C : register(b0) { float4 Color; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps() : SV_Target { return Color; }
)";

constexpr UINT W = 8, H = 8;

int main() {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        printf("SKIP: no WARP device\n");
        return 0;
    }
    ID3DBlob *vb = nullptr, *pb = nullptr;
    D3DCompile(kShaders, sizeof(kShaders) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vb, nullptr);
    D3DCompile(kShaders, sizeof(kShaders) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pb, nullptr);
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);
    D3D11_BUFFER_DESC cbd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ID3D11Buffer* cb = nullptr;
    dev->CreateBuffer(&cbd, nullptr, &cb);

    D3D11_TEXTURE2D_DESC td{W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D* fin = nullptr;
    dev->CreateTexture2D(&td, nullptr, &fin);
    ID3D11RenderTargetView* fin_rtv = nullptr;
    dev->CreateRenderTargetView(fin, nullptr, &fin_rtv);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&td, nullptr, &staging);

    D3D11_BLEND_DESC over{};
    over.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                            D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
    ID3D11BlendState* bs_over = nullptr;
    dev->CreateBlendState(&over, &bs_over);
    D3D11_DEPTH_STENCIL_DESC nodepth{};
    nodepth.DepthEnable = FALSE;
    nodepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    nodepth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState* dss = nullptr;
    dev->CreateDepthStencilState(&nodepth, &dss);
    D3D11_VIEWPORT vp{0, 0, W, H, 0, 1};

    // Scene (HUD-less): blue.
    const float scene[4] = {0, 0, 1, 1};
    ctx->ClearRenderTargetView(fin_rtv, scene);
    ctx->OMSetRenderTargets(1, &fin_rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->OMSetDepthStencilState(dss, 0);

    // hudless state as at the first interface draw.
    g_enabled = true;
    g_render_thread = GetCurrentThreadId();
    g_final = fin;
    g_bound_rt0 = fin;
    begin_interface(ctx);
    CHECK(g_snapped && g_ui_rtv, "interface phase not started");

    auto ui_draw = [&](ID3D11BlendState* bs, const float* color) {
        ctx->UpdateSubresource(cb, 0, nullptr, color, 0, 0);
        const float f[4] = {0.3f, 0.3f, 0.3f, 0.3f};
        ctx->OMSetBlendState(bs, f, 0xFFFFFFFF);
        ctx->Draw(3, 0);  // the game's draw
        CHECK(should_replay(), "replay not requested during the interface phase");
        if (replay_begin(ctx)) {
            ctx->Draw(3, 0);
            replay_end(ctx);
        }
        ID3D11BlendState* now = nullptr;
        float nf[4];
        UINT mask;
        ctx->OMGetBlendState(&now, nf, &mask);
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        CHECK(now == bs && nf[0] == 0.3f && mask == 0xFFFFFFFF && rtv == fin_rtv, "game state not restored");
        if (now)
            now->Release();
        if (rtv)
            rtv->Release();
    };
    auto read = [&](ID3D11Texture2D* t, float out[4]) {
        ctx->CopyResource(staging, t);
        D3D11_MAPPED_SUBRESOURCE m;
        ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
        const uint8_t* p = static_cast<const uint8_t*>(m.pData) + (H / 2) * m.RowPitch + (W / 2) * 4;
        for (int i = 0; i < 4; ++i)
            out[i] = p[i] / 255.0f;
        ctx->Unmap(staging, 0);
    };
    auto check_relation = [&](const char* what) {
        float f[4], u[4];
        read(fin, f);
        ID3D11Texture2D* layer = close_ui_layer(ctx);  // what is handed to DLSS-G
        CHECK(layer == g_ui_out, "%s: no closed layer", what);
        read(layer, u);
        for (int i = 0; i < 3; ++i) {
            const float expect = u[i] + (1.0f - u[3]) * scene[i];
            CHECK(std::fabs(f[i] - expect) < 3.0f / 255, "%s: channel %d final %.3f vs ui %.3f + (1-%.3f)*%.1f",
                  what, i, f[i], u[i], u[3], scene[i]);
        }
        printf("  %s: final (%.3f %.3f %.3f) ui (%.3f %.3f %.3f a %.3f)\n", what, f[0], f[1], f[2], u[0], u[1], u[2],
               u[3]);
    };

    const float red[4] = {1, 0, 0, 0.5f}, green[4] = {0, 1, 0, 0.5f}, grey[4] = {0.2f, 0.2f, 0.2f, 0.0f};
    ui_draw(bs_over, red);
    check_relation("one translucent draw");
    ui_draw(bs_over, green);
    check_relation("two translucent draws");
    float f[4];
    read(fin, f);
    CHECK(std::fabs(f[0] - 0.25f) < 3.0f / 255 && std::fabs(f[1] - 0.5f) < 3.0f / 255 &&
              std::fabs(f[2] - 0.25f) < 3.0f / 255,
          "game image changed by the replay: %.3f %.3f %.3f", f[0], f[1], f[2]);
    ui_draw(nullptr, grey);  // opaque (default blend state), shader alpha 0
    check_relation("opaque draw");
    float u[4];
    read(g_ui_out, u);
    CHECK(u[3] > 0.99f, "opaque draw: UI alpha %.3f, expected 1", u[3]);
    // Translucent draws keep the replayed layer as is (the closure leaves exact pixels alone): checked above, where
    // ui equalled the premultiplied colour and coverage.

    // Additive glow: game blends ONE/ONE; the closure must still close the relation.
    D3D11_BLEND_DESC add = over;
    add.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    add.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    ID3D11BlendState* bs_add = nullptr;
    dev->CreateBlendState(&add, &bs_add);
    ctx->ClearRenderTargetView(fin_rtv, scene);  // restart the frame
    begin_interface(ctx);
    const float glow[4] = {0.3f, 0.1f, 0.0f, 0.4f};
    ui_draw(bs_add, glow);
    check_relation("additive draw");
    read(g_ui_out, u);
    CHECK(std::fabs(u[3] - 0.4f) < 2.0f / 255, "additive draw: alpha %.3f must stay the replayed 0.4", u[3]);

    // Opacity must not depend on the scene behind (HUD flicker): the same stacked translucent interface over two
    // different scenes gives the same alpha.
    float alpha_over[2];
    const float scenes[2][4] = {{0, 0, 1, 1}, {0.7f, 0.6f, 0.2f, 1}};
    for (int s = 0; s < 2; ++s) {
        ctx->ClearRenderTargetView(fin_rtv, scenes[s]);
        begin_interface(ctx);
        for (int layer = 0; layer < 4; ++layer) {
            const float c[4] = {0.9f, 0.8f, 0.7f, 0.3f};
            ui_draw(bs_over, c);
        }
        close_ui_layer(ctx);
        read(g_ui_out, u);
        alpha_over[s] = u[3];
    }
    // UIAlpha variant: the R8 output equals the color+alpha output's alpha.
    kc::encore_uir::t_alpha_only = true;
    ID3D11Texture2D* alpha_layer = close_ui_layer(ctx);
    kc::encore_uir::t_alpha_only = false;
    CHECK(alpha_layer == g_ui_alpha, "alpha-only mode did not hand over the R8 alpha layer");
    {
        D3D11_TEXTURE2D_DESC ad;
        g_ui_alpha->GetDesc(&ad);
        ad.Usage = D3D11_USAGE_STAGING;
        ad.BindFlags = 0;
        ad.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* as = nullptr;
        dev->CreateTexture2D(&ad, nullptr, &as);
        ctx->CopyResource(as, g_ui_alpha);
        D3D11_MAPPED_SUBRESOURCE m;
        ctx->Map(as, 0, D3D11_MAP_READ, 0, &m);
        const float a8 = static_cast<const uint8_t*>(m.pData)[(H / 2) * m.RowPitch + W / 2] / 255.0f;
        ctx->Unmap(as, 0);
        as->Release();
        CHECK(std::fabs(a8 - alpha_over[1]) < 1.5f / 255, "R8 alpha %.3f vs color+alpha %.3f", a8, alpha_over[1]);
    }
    // Coverage read back a few closures later: the whole 8x8 target is covered at alpha 0.76.
    for (int i = 0; i < 4; ++i)
        close_ui_layer(ctx);
    CHECK(g_coverage > 0.99f, "interface coverage %.3f, expected 1", g_coverage);
    printf("  stacked translucent: alpha %.4f over blue, %.4f over olive\n", alpha_over[0], alpha_over[1]);
    CHECK(alpha_over[0] == alpha_over[1], "UI alpha depends on the scene: %.4f vs %.4f", alpha_over[0],
          alpha_over[1]);

    // Diagnostic dump: valid 24-bit BMPs (8x8: 54-byte header + 8 rows of 24 bytes).
    dump_layers(ctx, L"test");
    for (const wchar_t* n : {L"kcd_uir_final_test.bmp", L"kcd_uir_ui_alpha_test.bmp"}) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const bool ok = GetFileAttributesExW(n, GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 54 + 8 * 24;
        CHECK(ok, "dump %ls: size %lu", n, fa.nFileSizeLow);
        DeleteFileW(n);
    }
    for (const wchar_t* n : {L"kcd_uir_hudless_test.bmp", L"kcd_uir_ui_color_test.bmp"})
        DeleteFileW(n);

    printf(g_fail ? "ui_layer_warp: %d FAILED\n" : "ui_layer_warp: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
