#include "renderer/hudless.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <d3dcompiler.h>

#include "core/log.h"
#include "core/state.h"
#include "encore/encore_uir.h"
#include "hooks/d3d11_hooks.h"
#include "renderer/state_backup.h"
#include "renderer/taa_dlss.h"

namespace kc::hudless {

static bool g_enabled;
static DWORD g_render_thread;               // thread of RT_EndFrame
static ID3D11DeviceContext* g_ctx;          // immediate context of the latest draw (identity, lives with device)
static ID3D11Resource* g_bound_rt0;         // identity only: resource of RTV 0 while bound
static ID3D11Resource* g_last_draw_res;     // target of the latest draw this frame (identity)
static ID3D11Resource* g_final;             // previous frame's last target (identity; only used while bound or
                                            // as this frame's last draw target)
static bool g_scene_in_final;               // a full-screen pass wrote g_final this frame
static bool g_snapped;                      // interface phase: HUD-less copied, UI layer being drawn
static uint64_t g_frames, g_at_ui, g_at_end, g_replayed, g_replay_skipped;
static UINT g_ui_draws_frame;               // interface draws into the final image this frame (menu signal)

// UI layer: g_ui collects the replayed draws; at the end of the frame the closure pass writes g_ui_out from it, the
// final image (copy g_fin) and the HUD-less image (copy g_hud), so that final = out.rgb + (1 - out.a) * hudless holds
// for every pixel, whatever blend modes the interface used.
static ID3D11Texture2D* g_ui;
static ID3D11RenderTargetView* g_ui_rtv;
static ID3D11ShaderResourceView* g_ui_srv;
static ID3D11Texture2D* g_ui_out;
static ID3D11UnorderedAccessView* g_ui_out_uav;
static ID3D11Texture2D *g_hud, *g_fin;
static ID3D11ShaderResourceView *g_hud_srv, *g_fin_srv;
static ID3D11ComputeShader* g_close_cs;
static ID3D11Texture2D* g_ui_alpha;                // R8_UNORM alpha of g_ui_out (UIAlpha variant)
static ID3D11UnorderedAccessView* g_ui_alpha_uav;
static ID3D11Buffer* g_cov;                        // closure counter: pixels with alpha >= 0.5
static ID3D11UnorderedAccessView* g_cov_uav;
constexpr int kCovRing = 3;
static ID3D11Buffer* g_cov_staging[kCovRing];
static bool g_cov_pending[kCovRing];
static int g_cov_next;
static float g_coverage = -1.0f;                   // latest read-back interface coverage (fraction of screen)
static UINT g_ui_w, g_ui_h;
static DXGI_FORMAT g_ui_src_fmt = DXGI_FORMAT_UNKNOWN;
static bool g_ui_failed;
static uint64_t g_closed;

static const char kCloseCS[] = R"(
Texture2D<float4> Fin : register(t0);
Texture2D<float4> Hud : register(t1);
Texture2D<float4> Ui : register(t2);
RWTexture2D<unorm float4> OutC : register(u0);
RWTexture2D<unorm float> OutA : register(u1);   // UIAlpha variant
RWByteAddressBuffer Count : register(u2);       // [0] = pixels with alpha >= 0.5 (menu coverage)
void put(uint2 id, float4 v) {
    OutC[id] = v;
    OutA[id] = v.a;
    if (v.a >= 0.5)
        Count.InterlockedAdd(0, 1);
}
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint w, h;
    OutC.GetDimensions(w, h);
    if (any(id >= uint2(w, h)))
        return;
    float3 F = Fin.Load(int3(id, 0)).rgb;
    float3 H = Hud.Load(int3(id, 0)).rgb;
    float4 U = Ui.Load(int3(id, 0));
    float3 res = F - (U.rgb + (1.0 - U.a) * H);
    if (max(abs(res.r), max(abs(res.g), abs(res.b))) <= 4.0 / 255.0) {
        put(id, U);  // the replay explains this pixel (the game's own 8-bit blending rounds a little)
        return;
    }
    if (U.a >= 1.0 / 255.0) {
        // Replayed coverage is the interface's real opacity: keep it, so it never depends on the scene behind (that
        // made multi-layer translucent HUD elements flicker), and only correct the color (additive parts etc.).
        put(id, float4(clamp(F - (1.0 - U.a) * H, 0.0, 1.0), U.a));
        return;
    }
    // No replayed coverage (opaque draw with shader alpha 0, ...): least-squares coverage, then the exact color.
    float3 d = F - U.rgb;
    float hh = dot(H, H);
    float k = hh > 1e-4 ? saturate(dot(d, H) / hh) : 0.0;
    float a = max(1.0 - k, 1.0 / 255.0);  // NVIDIA: alpha must be non-zero wherever there is interface
    put(id, float4(clamp(F - (1.0 - a) * H, 0.0, 1.0), a));
}
)";

static DXGI_FORMAT unorm_view(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return f;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

template <class T> static void rel(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static void release_ui() {
    rel(g_ui_rtv);
    rel(g_ui_srv);
    rel(g_ui);
    rel(g_ui_out_uav);
    rel(g_ui_out);
    rel(g_hud_srv);
    rel(g_hud);
    rel(g_fin_srv);
    rel(g_fin);
    rel(g_ui_alpha_uav);
    rel(g_ui_alpha);
    rel(g_cov_uav);
    rel(g_cov);
    for (int i = 0; i < kCovRing; ++i) {
        rel(g_cov_staging[i]);
        g_cov_pending[i] = false;
    }
    g_coverage = -1.0f;
}
static std::vector<std::pair<D3D11_BLEND_DESC, ID3D11BlendState*>> g_blend_cache;
static std::vector<std::pair<D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState*>> g_dss_cache;

void set_enabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }

static bool ensure_ui(ID3D11DeviceContext* ctx, ID3D11Resource* final_res) {
    if (g_ui_failed)
        return false;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(final_res->QueryInterface(IID_PPV_ARGS(&t))))
        return false;
    D3D11_TEXTURE2D_DESC fd;
    t->GetDesc(&fd);
    t->Release();
    if (g_ui && g_ui_w == fd.Width && g_ui_h == fd.Height && g_ui_src_fmt == fd.Format)
        return true;
    release_ui();
    d3d11::ScopedInside inside;  // our own resources: no hook side effects
    const DXGI_FORMAT view = unorm_view(fd.Format);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = fd.Width;
    d.Height = fd.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // replay accumulation: no 8-bit rounding per interface layer
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = view != DXGI_FORMAT_UNKNOWN && fd.SampleDesc.Count == 1 ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
        hr = dev->CreateTexture2D(&d, nullptr, &g_ui);
    if (SUCCEEDED(hr))
        hr = dev->CreateRenderTargetView(g_ui, nullptr, &g_ui_rtv);
    if (SUCCEEDED(hr))
        hr = dev->CreateShaderResourceView(g_ui, nullptr, &g_ui_srv);
    d.Format = encore_uir::kUiFormat;  // what DLSS-G gets (NVIDIA: not R10G10B10A2)
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (SUCCEEDED(hr))
        hr = dev->CreateTexture2D(&d, nullptr, &g_ui_out);
    if (SUCCEEDED(hr))
        hr = dev->CreateUnorderedAccessView(g_ui_out, nullptr, &g_ui_out_uav);
    d.Format = DXGI_FORMAT_R8_UNORM;
    if (SUCCEEDED(hr))
        hr = dev->CreateTexture2D(&d, nullptr, &g_ui_alpha);
    if (SUCCEEDED(hr))
        hr = dev->CreateUnorderedAccessView(g_ui_alpha, nullptr, &g_ui_alpha_uav);
    d.Format = encore_uir::kUiFormat;
    {
        D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                             D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (SUCCEEDED(hr))
            hr = dev->CreateBuffer(&bd, nullptr, &g_cov);
        if (SUCCEEDED(hr))
            hr = dev->CreateUnorderedAccessView(g_cov, &ud, &g_cov_uav);
        D3D11_BUFFER_DESC sb{16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        for (int i = 0; i < kCovRing && SUCCEEDED(hr); ++i)
            hr = dev->CreateBuffer(&sb, nullptr, &g_cov_staging[i]);
    }
    // Copies of the HUD-less and final images (same format family as the final image, read as UNORM).
    D3D11_TEXTURE2D_DESC c = d;
    c.Format = fd.Format;
    c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = view;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (SUCCEEDED(hr))
        hr = dev->CreateTexture2D(&c, nullptr, &g_hud);
    if (SUCCEEDED(hr))
        hr = dev->CreateShaderResourceView(g_hud, &sd, &g_hud_srv);
    if (SUCCEEDED(hr))
        hr = dev->CreateTexture2D(&c, nullptr, &g_fin);
    if (SUCCEEDED(hr))
        hr = dev->CreateShaderResourceView(g_fin, &sd, &g_fin_srv);
    if (SUCCEEDED(hr) && !g_close_cs) {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        hr = D3DCompile(kCloseCS, sizeof(kCloseCS) - 1, "kcd_ui_close_cs", nullptr, nullptr, "main", "cs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (FAILED(hr) && err)
            logf("hudless: closure shader: %s", static_cast<const char*>(err->GetBufferPointer()));
        if (SUCCEEDED(hr))
            hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_close_cs);
        rel(code);
        rel(err);
    }
    dev->Release();
    if (FAILED(hr))
        release_ui();
    if (FAILED(hr)) {
        g_ui_failed = true;
        logf("hudless: UI layer %ux%u failed 0x%08lx; UI layer off", fd.Width, fd.Height, hr);
        return false;
    }
    g_ui_w = fd.Width;
    g_ui_h = fd.Height;
    g_ui_src_fmt = fd.Format;
    logf("hudless: UI layer %ux%u format %d ready (final image format %d)", fd.Width, fd.Height, (int)d.Format,
         (int)fd.Format);
    return true;
}

static void begin_interface(ID3D11DeviceContext* ctx) {
    encore_uir::copy_hudless(ctx, g_final);
    if (ensure_ui(ctx, g_final)) {
        ctx->CopyResource(g_hud, g_final);
        const float zero[4] = {};
        ctx->ClearRenderTargetView(g_ui_rtv, zero);
    }
    g_snapped = true;
}

// End of frame: closure pass into g_ui_out (see kCloseCS). Returns the layer to hand over, or null.
static ID3D11Texture2D* close_ui_layer(ID3D11DeviceContext* ctx) {
    if (!g_ui || !g_close_cs || !g_final)
        return nullptr;
    ctx->CopyResource(g_fin, g_final);
    {
        StateBackup backup(ctx);
        ID3D11ShaderResourceView* srvs[3] = {g_fin_srv, g_hud_srv, g_ui_srv};
        ctx->CSSetShader(g_close_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(g_cov_uav, zero);
        ID3D11UnorderedAccessView* uavs[3] = {g_ui_out_uav, g_ui_alpha_uav, g_cov_uav};
        ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        ctx->Dispatch((g_ui_w + 7) / 8, (g_ui_h + 7) / 8, 1);
        ID3D11ShaderResourceView* null_srvs[3] = {};
        ID3D11UnorderedAccessView* null_uavs[3] = {};
        ctx->CSSetShaderResources(0, 3, null_srvs);
        ctx->CSSetUnorderedAccessViews(0, 3, null_uavs, nullptr);
    }
    // Interface coverage, read back a few frames late without waiting (menu suspension).
    ctx->CopyResource(g_cov_staging[g_cov_next], g_cov);
    g_cov_pending[g_cov_next] = true;
    g_cov_next = (g_cov_next + 1) % kCovRing;
    if (g_cov_pending[g_cov_next]) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_cov_staging[g_cov_next], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            g_coverage = *static_cast<const UINT*>(m.pData) / (static_cast<float>(g_ui_w) * g_ui_h);
            ctx->Unmap(g_cov_staging[g_cov_next], 0);
            g_cov_pending[g_cov_next] = false;
        }
    }
    ++g_closed;
    return encore_uir::ui_alpha_only() ? g_ui_alpha : g_ui_out;
}

void on_set_rt0(ID3D11RenderTargetView* rtv) {
    if (!g_enabled || GetCurrentThreadId() != g_render_thread)
        return;
    g_bound_rt0 = nullptr;
    if (rtv) {
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        g_bound_rt0 = res;  // identity only; the RTV keeps it alive while bound
        if (res)
            res->Release();
    }
}

// Diagnostics: one frame of draws into the final image, with blend state (automatic once, and on F10).
static int g_trace_frames;
static int g_trace_lines;
static void trace_draw(ID3D11DeviceContext* ctx, bool fullscreen_tri, UINT count, char kind) {
    if (++g_trace_lines > 120)
        return;
    ID3D11BlendState* bs = nullptr;
    float f[4];
    UINT mask;
    ctx->OMGetBlendState(&bs, f, &mask);
    D3D11_RENDER_TARGET_BLEND_DESC rt{FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                                      D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
    if (bs) {
        D3D11_BLEND_DESC d;
        bs->GetDesc(&d);
        rt = d.RenderTarget[0];
        bs->Release();
    }
    logf("hudless trace: %c%u%s %s blend %d src %d dst %d srcA %d dstA %d mask 0x%x", kind, count,
         fullscreen_tri ? " fullscreen" : "", g_snapped ? "UI" : (g_scene_in_final ? "pre-UI" : "scene"),
         rt.BlendEnable, rt.SrcBlend, rt.DestBlend, rt.SrcBlendAlpha, rt.DestBlendAlpha, rt.RenderTargetWriteMask);
}

void before_draw(ID3D11DeviceContext* ctx, bool fullscreen_tri, UINT count, char kind) {
    if (!g_enabled || !g_bound_rt0 || GetCurrentThreadId() != g_render_thread)
        return;
    g_ctx = ctx;
    g_last_draw_res = g_bound_rt0;
    if (g_trace_frames && g_final && g_bound_rt0 == g_final)
        trace_draw(ctx, fullscreen_tri, count, kind);
    if (g_snapped && g_bound_rt0 == g_final)
        ++g_ui_draws_frame;  // same-frame menu signal (CPU, no read-back delay)
    if (g_snapped || !g_final || g_bound_rt0 != g_final)
        return;
    if (fullscreen_tri) {
        g_scene_in_final = true;
        return;
    }
    if (!g_scene_in_final)
        return;
    begin_interface(ctx);  // the final image just before the first interface draw
    ++g_at_ui;
}

bool should_replay() {
    return g_enabled && g_snapped && g_ui_rtv && g_bound_rt0 && g_bound_rt0 == g_final &&
           GetCurrentThreadId() == g_render_thread;
}

// Saved output-merger state around one replayed draw.
static struct {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView* dsv;
    ID3D11BlendState* bs;
    float factor[4];
    UINT mask;
    ID3D11DepthStencilState* dss;
    UINT ref;
} g_saved;

static void release_saved() {
    for (auto*& r : g_saved.rtv)
        if (r) {
            r->Release();
            r = nullptr;
        }
    for (IUnknown* u : {static_cast<IUnknown*>(g_saved.dsv), static_cast<IUnknown*>(g_saved.bs),
                        static_cast<IUnknown*>(g_saved.dss)})
        if (u)
            u->Release();
    g_saved.dsv = nullptr;
    g_saved.bs = nullptr;
    g_saved.dss = nullptr;
}

template <class Desc, class State, class Create>
static State* cached(std::vector<std::pair<Desc, State*>>& cache, const Desc& d, Create create) {
    for (auto& [k, v] : cache)
        if (memcmp(&k, &d, sizeof(d)) == 0)
            return v;
    State* s = nullptr;
    if (FAILED(create(&d, &s)))
        return nullptr;
    cache.emplace_back(d, s);
    return s;
}

bool replay_begin(ID3D11DeviceContext* ctx) {
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_saved.rtv, &g_saved.dsv);
    ctx->OMGetBlendState(&g_saved.bs, g_saved.factor, &g_saved.mask);
    ctx->OMGetDepthStencilState(&g_saved.dss, &g_saved.ref);

    D3D11_BLEND_DESC bd{};
    D3D11_RENDER_TARGET_BLEND_DESC rt{FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
                                      D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
                                      D3D11_COLOR_WRITE_ENABLE_ALL};
    if (g_saved.bs) {
        D3D11_BLEND_DESC gd;
        g_saved.bs->GetDesc(&gd);
        rt = gd.RenderTarget[0];
    }
    if ((rt.RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_ALL) == 0) {
        release_saved();  // no color written: nothing for the UI layer
        ++g_replay_skipped;
        return false;
    }
    float factor[4] = {g_saved.factor[0], g_saved.factor[1], g_saved.factor[2], g_saved.factor[3]};
    if (rt.BlendEnable) {
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;  // coverage: a_ui = a + a_ui * (1 - a)
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    } else {
        rt.BlendEnable = TRUE;  // opaque draw: color as written, alpha 1
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ZERO;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        factor[0] = factor[1] = factor[2] = factor[3] = 1.0f;
    }
    rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    bd.RenderTarget[0] = rt;

    D3D11_DEPTH_STENCIL_DESC dd{TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE,
                                D3D11_DEFAULT_STENCIL_READ_MASK, D3D11_DEFAULT_STENCIL_WRITE_MASK,
                                {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                 D3D11_COMPARISON_ALWAYS},
                                {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                 D3D11_COMPARISON_ALWAYS}};
    if (g_saved.dss)
        g_saved.dss->GetDesc(&dd);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;  // tests as the game's draw, writes nothing
    dd.StencilWriteMask = 0;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11BlendState* bs = cached(g_blend_cache, bd, [dev](const D3D11_BLEND_DESC* d, ID3D11BlendState** s) {
        return dev->CreateBlendState(d, s);
    });
    ID3D11DepthStencilState* dss =
        cached(g_dss_cache, dd, [dev](const D3D11_DEPTH_STENCIL_DESC* d, ID3D11DepthStencilState** s) {
            return dev->CreateDepthStencilState(d, s);
        });
    dev->Release();
    if (!bs || !dss) {
        release_saved();
        ++g_replay_skipped;
        return false;
    }
    ctx->OMSetRenderTargets(1, &g_ui_rtv, g_saved.dsv);
    ctx->OMSetBlendState(bs, factor, g_saved.mask);
    ctx->OMSetDepthStencilState(dss, g_saved.ref);
    ++g_replayed;
    return true;
}

void replay_end(ID3D11DeviceContext* ctx) {
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_saved.rtv, g_saved.dsv);
    ctx->OMSetBlendState(g_saved.bs, g_saved.factor, g_saved.mask);
    ctx->OMSetDepthStencilState(g_saved.dss, g_saved.ref);
    release_saved();
}

// ---- Diagnostics: F10 dumps final, HUD-less and the UI layer (color, alpha) as BMPs next to the DLL ----------------
static bool g_dump_pending;
static int g_auto_dump = -1;  // -1 = not yet; 2, 1 = dumping consecutive frames; 0 = done

static void write_bmp(const std::wstring& path, const uint8_t* rgba, UINT pitch, UINT w, UINT h, int channel,
                      bool bgra) {
    const size_t row = (static_cast<size_t>(w) * 3 + 3) & ~static_cast<size_t>(3);
    std::vector<uint8_t> px(row * h);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            const uint8_t* s = rgba + static_cast<size_t>(y) * pitch + (channel == 9 ? x : x * 4);
            uint8_t* d = &px[static_cast<size_t>(h - 1 - y) * row + x * 3];
            if (channel >= 0) {  // single channel as grey
                d[0] = d[1] = d[2] = s[channel == 9 ? 0 : channel];
            } else {
                d[0] = s[bgra ? 0 : 2];
                d[1] = s[1];
                d[2] = s[bgra ? 2 : 0];
            }
        }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size());
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD n;
    WriteFile(f, &fh, sizeof(fh), &n, nullptr);
    WriteFile(f, &ih, sizeof(ih), &n, nullptr);
    WriteFile(f, px.data(), static_cast<DWORD>(px.size()), &n, nullptr);
    CloseHandle(f);
}

static void dump_tex(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, const std::wstring& name, int channel) {
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    const DXGI_FORMAT v = unorm_view(d.Format);
    const bool bgra = v == DXGI_FORMAT_B8G8R8A8_UNORM || v == DXGI_FORMAT_B8G8R8X8_UNORM;
    if (v != DXGI_FORMAT_R8G8B8A8_UNORM && !bgra && d.Format != DXGI_FORMAT_R8_UNORM) {
        logf("hudless: dump %ls skipped (format %d)", name.c_str(), (int)d.Format);
        return;
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Texture2D* s = nullptr;
    HRESULT hr = dev->CreateTexture2D(&d, nullptr, &s);
    dev->Release();
    if (FAILED(hr))
        return;
    ctx->CopyResource(s, t);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(s, 0, D3D11_MAP_READ, 0, &m))) {
        write_bmp(state().mod_dir + L"kcd_uir_" + name + L".bmp", static_cast<const uint8_t*>(m.pData), m.RowPitch,
                  d.Width, d.Height, d.Format == DXGI_FORMAT_R8_UNORM ? 9 : channel, bgra);
        ctx->Unmap(s, 0);
    }
    s->Release();
}

static void dump_layers(ID3D11DeviceContext* ctx, const wchar_t* tag) {
    const std::wstring t = tag;
    dump_tex(ctx, g_fin, L"final_" + t, -1);
    dump_tex(ctx, g_hud, L"hudless_" + t, -1);
    dump_tex(ctx, g_ui_out, L"ui_color_" + t, -1);
    dump_tex(ctx, g_ui_out, L"ui_alpha_" + t, 3);
    logf("hudless: dumped kcd_uir_{final,hudless,ui_color,ui_alpha}_%ls.bmp (frame %llu, coverage %.1f%%, interface "
         "draws %u)", tag, g_frames, 100.0f * g_coverage, g_ui_draws_frame);
}

// ---- Menu suspension (as Skyrim Community Shaders does: no frame generation while a menu is open) -----------------
static bool g_menu_suspend = true;
static float g_menu_coverage = 0.5f;
static UINT g_menu_draws;          // 0 = off: interface draws per frame that count as a menu (same frame)
static UINT g_ui_draws_max_calm;   // largest count on frames that were not menus (calibration, per 3600 frames)
static bool g_suspended;
static int g_calm_frames;

void set_menu_suspend(bool on, float coverage, UINT draws) {
    g_menu_suspend = on;
    g_menu_coverage = coverage;
    g_menu_draws = draws;
}

static void update_menu_suspend() {
    if (!g_menu_suspend)
        return;
    const bool scene = taa_dlss::scene_last_frame();
    const bool by_draws = g_menu_draws && g_ui_draws_frame >= g_menu_draws;
    const bool menu = !scene || (g_coverage >= g_menu_coverage) || by_draws;
    if (!menu && g_ui_draws_frame > g_ui_draws_max_calm)
        g_ui_draws_max_calm = g_ui_draws_frame;
    bool want = g_suspended;
    if (menu) {
        want = true;
        g_calm_frames = 0;
    } else if (g_suspended && ++g_calm_frames >= 3) {
        want = false;  // 3 frames of 3D scene with little interface before frame generation resumes
    }
    if (want != g_suspended) {
        g_suspended = want;
        logf("hudless: frame generation %s (3D scene %d, interface coverage %.1f%%, interface draws %u%s)",
             want ? "suspended" : "resumed", scene, 100.0f * g_coverage, g_ui_draws_frame,
             by_draws ? ", by draw count" : "");
        encore_uir::set_fg_suspended(want);
    }
}

void on_end_frame() {
    if (!g_enabled)
        return;
    if (g_render_thread != GetCurrentThreadId()) {
        logf("hudless: render thread %lu (was %lu)", GetCurrentThreadId(), g_render_thread);
        g_render_thread = GetCurrentThreadId();
        g_last_draw_res = g_bound_rt0 = nullptr;
        return;
    }
    if (!g_snapped && g_final && g_ctx && g_last_draw_res == g_final) {
        begin_interface(g_ctx);  // no interface this frame: HUD-less = final image, empty UI layer
        ++g_at_end;
    }
    if (g_snapped && g_ctx) {
        d3d11::ScopedInside inside;
        if (ID3D11Texture2D* layer = close_ui_layer(g_ctx)) {
            encore_uir::finish_frame(g_ctx, layer);
            if (g_dump_pending) {
                g_dump_pending = false;
                dump_layers(g_ctx, L"f10");
            } else if (g_auto_dump > 0) {
                // Two consecutive gameplay frames: does the layer itself change between frames (HUD flicker)?
                dump_layers(g_ctx, g_auto_dump == 2 ? L"a" : L"b");
                --g_auto_dump;
            }
        }
    }
    if (g_trace_frames && --g_trace_frames == 0)
        logf("hudless trace: end (%d draws into the final image)", g_trace_lines);
    // F10: trace the next frame's draws into the final image and dump the layers (diagnostics).
    const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    static bool f10_was;
    if ((f10 && !f10_was) || (g_at_ui == 600 && !g_trace_frames && g_snapped)) {
        g_trace_frames = 1;
        g_trace_lines = 0;
        g_dump_pending = f10;
        logf("hudless trace: next frame%s", f10 ? " (F10, layers dumped)" : "");
    }
    f10_was = f10;
    encore_uir::on_frame();
    // Automatic dump once per session: 1200 interface frames in, frame generation on, little interface on screen.
    if (g_auto_dump < 0 && g_at_ui >= 1200 && !g_suspended && g_coverage >= 0.0f && g_coverage < 0.1f && g_snapped)
        g_auto_dump = 2;
    update_menu_suspend();
    if (g_last_draw_res != g_final) {
        g_final = g_last_draw_res;  // identity only: no reference, so ResizeBuffers is never blocked
        static int changes;
        if (++changes <= 16)
            logf("hudless: final image is now %p", g_final);
    }
    ++g_frames;
    if (g_frames % 3600 == 0)
        logf("hudless: %llu frames: interface found %llu, none %llu; UI draws replayed %llu, skipped %llu; layers "
             "closed %llu (state cache %zu blend, %zu depth); interface coverage %.1f%%, frame generation %s", g_frames,
             g_at_ui, g_at_end, g_replayed, g_replay_skipped, g_closed, g_blend_cache.size(), g_dss_cache.size(),
             100.0f * g_coverage, g_suspended ? "suspended" : "on");
    if (g_frames % 3600 == 0) {
        logf("hudless: most interface draws on a non-menu frame in the last 3600: %u", g_ui_draws_max_calm);
        g_ui_draws_max_calm = 0;
    }
    g_snapped = false;
    g_scene_in_final = false;
    g_last_draw_res = nullptr;
    g_ui_draws_frame = 0;
}

}  // namespace kc::hudless
