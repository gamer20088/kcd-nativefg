#include "encore/encore_uir.h"

#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>
#include <intrin.h>

#include <MinHook.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>

#include <atomic>
#include <iterator>
#include <cstring>
#include <mutex>
#include <string>

#include "core/log.h"

#pragma intrinsic(_ReturnAddress)

namespace kc::encore_uir {

// ---- The one Encore build this was analysed on --------------------------------------------------------------------
constexpr DWORD kTimestamp = 0x6AFD7776;
constexpr DWORD kSizeOfImage = 0x360A000;
constexpr uintptr_t kInputCopyRva = 0x200E0;       // present-route input copy + slSetTagForFrame (depth, mvec, HUD-less)
constexpr uintptr_t kTrialCallRva = 0x1D2ED;       // the present trial's call site: ... xor r9d,r9d ... call 0x200E0
constexpr uintptr_t kTrialReturnRva = 0x1D308;     // return address of that call
constexpr uintptr_t kSetOptionsPtrRva = 0x1201E70;  // slDLSSGSetOptions pointer used by the present-route options
constexpr uintptr_t kSetTagPtrRva = 0x1201E58;     // slSetTagForFrame pointer (0x20840 calls through it)
constexpr uintptr_t kViewportRva = 0x11BEBD0;      // sl::ViewportHandle the present route tags and sets options on
constexpr uintptr_t kOptionsLoadRva = 0x1F8A0;     // mov rax,[rip+...] -> kSetOptionsPtrRva, inside 0x1F6E0
constexpr uintptr_t kSetTagCallRva = 0x209C1;      // call [rip+...] -> kSetTagPtrRva, inside 0x20840
constexpr uintptr_t kOptionsReturnRvas[] = {0x1F90F, 0x1FA78, 0x1FA9D};  // after each `call rax` in 0x1F6E0

static const uint8_t kTrialCallBytes[] = {0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00,  // mov [rsp+20h],0
                                          0x45, 0x33, 0xC9,                                      // xor r9d,r9d
                                          0x4C, 0x8D, 0x45, 0x80,                                // lea r8,[rbp-80h]
                                          0x48, 0x8B, 0xD6, 0x48, 0x8B, 0xCF,                    // mov rdx,rsi / rcx,rdi
                                          0xE8, 0xD8, 0x2D, 0x00, 0x00};                         // call 0x200E0
static const uint8_t kInputCopyPrologue[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                             0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C};
static const uint8_t kOptionsLoadBytes[] = {0x48, 0x8B, 0x05, 0xC9, 0x25, 0x1E, 0x01};
static const uint8_t kSetTagCallBytes[] = {0xFF, 0x15, 0x91, 0x14, 0x1E, 0x01};  // call [rip+0x11E1491]
static const uint8_t kCallRax[] = {0xFF, 0xD0};
// Encore's graphics-API probe (movzx eax, byte [rip+...]; ret). Encore allows multipliers above 2x only when it
// returns 4 (or two other API checks pass); on this DX11 route it does not, so every multiplier cap below is 2:
//   0x115930 request callback (0x11E7C88), 0x0B4785 menu maximum (greys out 3x-6x), 0x13839E / 0x1392B9 runtime
//   maximum, 0x0F5CDD DLSS-G generated-frame count, 0x0BDED4 "capped to native 2x". Dynamic MFG sites are left alone.
constexpr uintptr_t kApiProbeRva = 0x1B3310;
static const uint8_t kApiProbeBytes[] = {0x0F, 0xB6, 0x05, 0x19, 0xF4, 0x76, 0x01, 0xC3};
constexpr uintptr_t kMfgGateReturnRvas[] = {0x115935, 0x0B478A, 0x1383A3, 0x1392BE, 0x0F5CE2, 0x0BDED9};
constexpr uint8_t kMfgApi = 4;

// sl::DLSSGOptions (Streamline 2.x, as Encore's own option dumper at 0xC4470 reads it).
constexpr uint32_t kDlssgOptionsGuid[4] = {0xFAC5F1CB, 0x4F362DFD, 0x9E3AE6A1, 0xC5565286};
constexpr size_t kOptGuid = 0x08, kOptVersion = 0x18, kOptFlags = 0x28, kOptHudlessFormat = 0x54, kOptUiFormat = 0x58,
                 kOptUir = 0x70;
constexpr uint32_t kFlagFullscreenMenuDetection = 1u << 4;  // sl::DLSSGFlags::eEnableFullscreenMenuDetection
constexpr uint32_t kFlagRetainResourcesWhenOff = 1u << 3;   // sl::DLSSGFlags::eRetainResourcesWhenOff
constexpr size_t kOptMode = 0x20;                           // sl::DLSSGMode (0 = eOff), as 0x1F6E0 writes it
constexpr size_t kOptFrames = 0x24;                         // numFramesToGenerate (multiplier - 1), as 0x1F6E0 writes it
constexpr size_t kOptCopy = 0x78;  // what Encore's 0x1F6E0 initializes (through +0x74)
constexpr uint8_t kSlTrue = 1;
// sl::Resource / sl::ResourceTag as Encore builds them in 0x20840 (GUIDs read from its .rdata).
static const uint8_t kResourceGuid[16] = {0xCF, 0x70, 0x9D, 0x3A, 0x18, 0x24, 0x72, 0x4B,
                                          0x83, 0x91, 0x13, 0xF8, 0x72, 0x1C, 0x72, 0x61};
static const uint8_t kResourceTagGuid[16] = {0xAD, 0x5A, 0x6A, 0x4C, 0x45, 0xB4, 0x6C, 0x49,
                                             0x87, 0xFF, 0x1A, 0xF3, 0x84, 0x5B, 0xE6, 0x53};
constexpr uint32_t kBufferTypeUIColorAndAlpha = 23;  // sl_consts.h order, confirmed by sl.common's name table
// sl.dlss_g 2.14.16 fetches its UI inputs in slot order HUD-less (type 2), UIAlpha (0x45), UI (0x17) at 0x44E77-0x44EB7;
// 24 (the position in sl.common's name table) was never forwarded to NGX.
constexpr uint32_t kBufferTypeUIAlpha = 0x45;
constexpr uint32_t kLifecycleOnlyValidNow = 0;       // Streamline copies on the command list passed with the tag
constexpr uint32_t kStateNonPixelShaderResource = 0x40;
constexpr size_t kSlotFrameToken = 0x20;             // 0x20840: rcx = [slot+20h] for slSetTagForFrame

using InputCopyFn = bool(__fastcall*)(void* trial, void* slot, void* guides, ID3D12Resource* hudless, void* convert);
using SetOptionsFn = int(__fastcall*)(const void* viewport, void* options);
using SetTagFn = int(__fastcall*)(const void* frame, const void* viewport, const void* tags, uint32_t n, void* cmd);

static uintptr_t g_base;
static bool g_active;
static InputCopyFn o_input_copy;
using ApiProbeFn = uint8_t(__fastcall*)();
static ApiProbeFn o_api_probe;
static bool g_unlock_mfg = false;  // [encore] arch_spoof
static std::atomic<uint32_t> g_unlock_hits;
static std::atomic<uint32_t> g_site_hits[6];

void set_unlock_mfg(bool on) { g_unlock_mfg = on; }
static bool g_options_detour = true;  // [encore] options_detour
void set_options_detour(bool on) { g_options_detour = on; }

// Only the multiplier-cap call sites see the MFG-capable answer; every other Encore caller gets the real one.
static uint8_t __fastcall hk_api_probe() {
    const uint8_t v = o_api_probe();
    if (!g_unlock_mfg || v == kMfgApi)
        return v;
    const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
    for (size_t i = 0; i < std::size(kMfgGateReturnRvas); ++i) {
        if (ra == kMfgGateReturnRvas[i]) {
            ++g_unlock_hits;
            if (++g_site_hits[i] == 1)  // first hit per site
                logf("encore_uir: multiplier cap at +0x%llx lifted (GPU architecture %u)",
                     (unsigned long long)(kMfgGateReturnRvas[i] - 5), v);
            return kMfgApi;
        }
    }
    return v;
}
static std::atomic<SetOptionsFn> g_real_set_options;
static bool g_menu_detection = true;  // [encore] menu_detection
static bool g_inputs = true;          // [encore] uir: hand HUD-less/UI to DLSS-G and enable UIR (0 = suspension only)
static bool g_alpha_only;             // [encore] ui_input = alpha: kBufferTypeUIAlpha (R8) instead of color+alpha
static std::atomic<bool> g_suspended;  // frame generation suspended (menus): options mode forced to eOff
// [encore] fg_multiplier: -1 = rtx-encore.jsonc frameGeneration.multiplier (fixed mode), 0 = Encore's request as is,
// 2..6 = this multiplier. Encore 2.5.0 caps its DX11 present route at 2x (maximum=2; COMPATIBILITY.md "DirectX 11:
// DLSS MFG ... unavailable"); its DLSS-G backend is D3D12 with sl.dlss_g/nvngx_dlssg patched to 6x.
static int g_fg_setting = -1;
static std::atomic<int> g_fg_multiplier{0};  // effective: 0 = no override
static std::wstring g_jsonc_path;
static FILETIME g_jsonc_time;

void set_fg_multiplier(int m) { g_fg_setting = m; }

// RTX Encore's own menu is drawn after our HUD-less/UI capture, so DLSS-G would recompose our UI over frames that
// lack it and the menu would look half transparent. While it is open we hand DLSS-G no HUD-less/UI image.
static std::atomic<bool> g_encore_menu_open;
static int g_menu_vk;

// rtx-encore.jsonc "menuKey" (virtual-key code Encore's menu writes), 0 if absent.
static int read_encore_menu_key() {
    HANDLE h = CreateFileW(g_jsonc_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    std::string text(64 * 1024, '\0');
    DWORD n = 0;
    ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
    text.resize(n);
    const size_t k = text.find("\"menuKey\"");
    if (k == std::string::npos)
        return 0;
    const int vk = atoi(text.c_str() + text.find(':', k) + 1);
    return vk > 0 && vk < 255 ? vk : 0;
}

bool encore_menu_open() { return g_encore_menu_open.load(); }

// Diagnostics (read-only): the Encore values that decide the multiplier limit, logged when they change.
//   0x1922361 D157 midpoint-fix qualification (pre-Blackwell MFG), 0x11CAEC0 runtime maximum factor,
//   0x11D4F80 menu capability struct (+0 route, +8 maximum factor the menu offers).
static void log_encore_mfg_state() {
    static uint32_t last[5] = {~0u, ~0u, ~0u, ~0u, ~0u};
    const uint32_t now[5] = {*reinterpret_cast<const uint8_t*>(g_base + 0x1922361),
                             *reinterpret_cast<const uint32_t*>(g_base + 0x11CAEC0),
                             *reinterpret_cast<const uint32_t*>(g_base + 0x11D4F80),
                             *reinterpret_cast<const uint32_t*>(g_base + 0x11D4F88),
                             g_unlock_hits.load() ? 1u : 0u};
    if (memcmp(now, last, sizeof(now)) == 0)
        return;
    memcpy(last, now, sizeof(now));
    logf("encore_uir: Encore MFG state: midpoint-fix %u, runtime maximum %u, menu route %u, menu maximum %u, "
         "gate sites hit %u/%u/%u/%u/%u/%u", now[0], now[1], now[2], now[3], g_site_hits[0].load(),
         g_site_hits[1].load(), g_site_hits[2].load(), g_site_hits[3].load(), g_site_hits[4].load(),
         g_site_hits[5].load());
}

// Encore's menu visibility: the byte SetMenuVisible (0x3FE6A0) writes with xchg and every UI backend reads before
// drawing the menu window, e.g. 0x3FB7E6: movzx eax, byte [rip+0x1542463] -> 0x193DC50. Encore's D3D11 UI backend
// (0x421020) composites the menu into the image DLSS-G receives; our HUD-less/UI alpha lack it, so with them DLSS-G
// recomposes generated frames without the menu (it looks half transparent). While it is visible we hand over no
// HUD-less/UI image and DLSS-G interpolates the full frame, menu included.
constexpr uintptr_t kMenuVisibleRva = 0x193DC50;
constexpr uintptr_t kMenuReaderRva = 0x3FB7E6;
static const uint8_t kMenuReaderBytes[] = {0x0F, 0xB6, 0x05, 0x63, 0x24, 0x54, 0x01};
static const volatile uint8_t* g_menu_visible;  // null: version guard failed, key-tracking fallback

// Render thread, once per frame.
static void update_encore_menu() {
    static bool init, toggled, was_down;
    if (!init) {
        init = true;
        if (memcmp(reinterpret_cast<const void*>(g_base + kMenuReaderRva), kMenuReaderBytes,
                   sizeof(kMenuReaderBytes)) == 0)
            g_menu_visible = reinterpret_cast<const volatile uint8_t*>(g_base + kMenuVisibleRva);
        g_menu_vk = read_encore_menu_key();
        if (!g_menu_vk)
            g_menu_vk = VK_INSERT;
        logf("encore_uir: RTX Encore menu visibility from %s",
             g_menu_visible ? "Encore's menu state (0x193DC50)" : "its key (fallback: version guard mismatch)");
    }
    bool open;
    if (g_menu_visible) {
        open = *g_menu_visible != 0;
    } else {  // fallback: menu key toggles, Escape closes
        const bool down = (GetAsyncKeyState(g_menu_vk) & 0x8000) != 0;
        if (down && !was_down)
            toggled = !toggled;
        was_down = down;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            toggled = false;
        open = toggled;
    }
    if (g_encore_menu_open.exchange(open) != open)
        logf("encore_uir: RTX Encore menu %s: HUD-less/UI inputs %s%s", open ? "visible" : "hidden",
             open ? "paused (DLSS-G interpolates the full frame)" : "resumed",
             g_suspended.load() ? "; frame generation paused for a game menu" : "");
}

// Reads frameGeneration.mode / multiplier from Encore's own config. 0 unless mode is "fixed".
static int read_encore_multiplier() {
    HANDLE h = CreateFileW(g_jsonc_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    std::string text(64 * 1024, '\0');
    DWORD n = 0;
    ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
    text.resize(n);
    const size_t fg = text.find("\"frameGeneration\"");
    if (fg == std::string::npos)
        return 0;
    auto value_after = [&](const char* key) -> std::string {
        size_t k = text.find(key, fg);
        if (k == std::string::npos)
            return {};
        size_t c = text.find(':', k);
        size_t s = text.find_first_not_of(" \t\"", c + 1);
        size_t e = text.find_first_of(",\"\r\n /", s);
        return s == std::string::npos ? std::string() : text.substr(s, e - s);
    };
    if (value_after("\"mode\"") != "fixed")
        return 0;
    const int m = atoi(value_after("\"multiplier\"").c_str());
    return m >= 2 && m <= 6 ? m : 0;
}

// Render thread, once per frame: picks up multiplier changes made in Encore's menu (it saves rtx-encore.jsonc).
static bool refresh_multiplier() {
    int m = g_fg_setting >= 2 ? g_fg_setting : 0;
    if (g_fg_setting == -1) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(g_jsonc_path.c_str(), GetFileExInfoStandard, &a))
            return false;
        if (CompareFileTime(&a.ftLastWriteTime, &g_jsonc_time) == 0)
            return false;
        g_jsonc_time = a.ftLastWriteTime;
        m = read_encore_multiplier();
    }
    if (g_fg_multiplier.exchange(m) == m)
        return false;
    logf("encore_uir: frame generation multiplier %dx%s", m ? m : 0,
         m ? " requested from DLSS-G" : " (Encore's own request)");
    return true;
}

static void apply_multiplier(uint8_t* p) {
    const int m = g_fg_multiplier.load();
    uint32_t mode;
    memcpy(&mode, p + kOptMode, sizeof(mode));
    if (m < 2 || mode != 1)  // only for DLSSGMode::eOn
        return;
    const uint32_t frames = static_cast<uint32_t>(m - 1);
    memcpy(p + kOptFrames, &frames, sizeof(frames));
}

void set_ui_alpha_only(bool on) { g_alpha_only = on; }
void set_inputs(bool on) { g_inputs = on; }
bool ui_alpha_only() { return g_alpha_only; }
static DXGI_FORMAT ui_fmt() { return g_alpha_only ? DXGI_FORMAT_R8_UNORM : kUiFormat; }

// Forces mode eOff (keeping resources) while suspended; `p` keeps Encore's mode otherwise.
static void apply_suspension(uint8_t* p) {
    if (!g_suspended.load())
        return;
    const uint32_t off = 0;
    memcpy(p + kOptMode, &off, sizeof(off));
    uint32_t flags;
    memcpy(&flags, p + kOptFlags, sizeof(flags));
    flags |= kFlagRetainResourcesWhenOff;
    memcpy(p + kOptFlags, &flags, sizeof(flags));
}

void set_menu_detection(bool on) { g_menu_detection = on; }

// NVIDIA (ProgrammingGuideDLSS_G.md): with kBufferTypeUIColorAndAlpha tagged, eEnableFullscreenMenuDetection lets
// DLSS-G stop generating frames on full-screen menus (map, inventory).
static void patch_flags(uint8_t* p) {
    if (!g_menu_detection)
        return;
    uint32_t flags;
    memcpy(&flags, p + kOptFlags, sizeof(flags));
    flags |= kFlagFullscreenMenuDetection;
    memcpy(p + kOptFlags, &flags, sizeof(flags));
}

// ---- Shared ring of HUD-less + UI textures ---------------------------------------------------------------------------
// Created on Encore's D3D12 device (the guides' device) by the present thread, opened on D3D11 by the render thread.
// ready fence:   D3D11 signals after both copies of a frame; the guides' queue waits for it before the tag copies.
// release fence: the guides' queue signals after Streamline's copies; D3D11 waits for it before reusing a slot.
constexpr int kRing = 3;
struct Shared {
    std::mutex m;
    UINT want_w = 0, want_h = 0;
    DXGI_FORMAT want_fmt = DXGI_FORMAT_UNKNOWN;
    bool broken = false;
    uint32_t gen = 0;
    // D3D12 side (present thread).
    ID3D12Device* dev12 = nullptr;
    ID3D12Resource* hud12[kRing] = {};
    ID3D12Resource* ui12[kRing] = {};
    ID3D12CommandAllocator* alloc[kRing] = {};
    UINT64 alloc_done[kRing] = {};  // release value after which alloc[i] may be reset
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* ready12 = nullptr;
    ID3D12Fence* release12 = nullptr;
    UINT64 release_value = 0;
    UINT have_w = 0, have_h = 0;
    DXGI_FORMAT have_fmt = DXGI_FORMAT_UNKNOWN;
    HANDLE hud_h[kRing] = {}, ui_h[kRing] = {}, ready_h = nullptr, release_h = nullptr;
    UINT64 consumed = 0;
    // D3D11 side (render thread).
    ID3D11Texture2D* hud11[kRing] = {};
    ID3D11Texture2D* ui11[kRing] = {};
    ID3D11Fence* ready11 = nullptr;
    ID3D11Fence* release11 = nullptr;
    uint32_t gen11 = 0;
    UINT64 value11 = 0;
    int next_slot = 0;
    int cur_slot = -1;           // slot holding this frame's HUD-less copy
    UINT64 slot_free[kRing] = {};  // release value that frees each slot (written by the present thread)
    // Published by the render thread for the present thread.
    uint32_t ready_gen = 0;
    UINT64 ready_value = 0;
    int ready_slot = -1;
    // Options re-send (present thread).
    uint8_t last_opts[0x100] = {};
    bool have_opts = false;
    uint32_t sent_hud_fmt = 0, sent_ui_fmt = 0;
    // Stats.
    uint64_t submitted = 0, tagged = 0, ui_tagged = 0, missing = 0, ui_skipped = 0, reused_wait = 0;
};
static Shared g_s;
static std::atomic<uint64_t> g_options_patched;

bool active() { return g_active; }

static DXGI_FORMAT typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return f;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

template <class T> static void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
static void close(HANDLE& h) {
    if (h) {
        CloseHandle(h);
        h = nullptr;
    }
}

static void release12() {
    for (int i = 0; i < kRing; ++i) {
        release(g_s.hud12[i]);
        release(g_s.ui12[i]);
        release(g_s.alloc[i]);
        close(g_s.hud_h[i]);
        close(g_s.ui_h[i]);
        g_s.alloc_done[i] = 0;
    }
    release(g_s.list);
    release(g_s.ready12);
    release(g_s.release12);
    close(g_s.ready_h);
    close(g_s.release_h);
}

static HRESULT shared_texture(DXGI_FORMAT fmt, ID3D12Resource** tex, HANDLE* h) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = g_s.want_w;
    rd.Height = g_s.want_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Simultaneous access: written by D3D11, read by Streamline's copy on D3D12; the layout does not depend on the
    // state the tags declare (NON_PIXEL_SHADER_RESOURCE).
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    HRESULT hr = g_s.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                    nullptr, IID_PPV_ARGS(tex));
    if (SUCCEEDED(hr))
        hr = g_s.dev12->CreateSharedHandle(*tex, nullptr, GENERIC_ALL, nullptr, h);
    return hr;
}

// Present thread, g_s.m held.
static bool create12() {
    release12();
    HRESULT hr = S_OK;
    for (int i = 0; i < kRing && SUCCEEDED(hr); ++i) {
        hr = shared_texture(g_s.want_fmt, &g_s.hud12[i], &g_s.hud_h[i]);
        if (SUCCEEDED(hr))
            hr = shared_texture(ui_fmt(), &g_s.ui12[i], &g_s.ui_h[i]);
        if (SUCCEEDED(hr))
            hr = g_s.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_s.alloc[i]));
    }
    if (SUCCEEDED(hr))
        hr = g_s.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_s.alloc[0], nullptr,
                                          IID_PPV_ARGS(&g_s.list));
    if (SUCCEEDED(hr))
        hr = g_s.list->Close();
    if (SUCCEEDED(hr))
        hr = g_s.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_s.ready12));
    if (SUCCEEDED(hr))
        hr = g_s.dev12->CreateSharedHandle(g_s.ready12, nullptr, GENERIC_ALL, nullptr, &g_s.ready_h);
    if (SUCCEEDED(hr))
        hr = g_s.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_s.release12));
    if (SUCCEEDED(hr))
        hr = g_s.dev12->CreateSharedHandle(g_s.release12, nullptr, GENERIC_ALL, nullptr, &g_s.release_h);
    ++g_s.gen;
    g_s.consumed = 0;
    g_s.release_value = 0;
    if (FAILED(hr)) {
        release12();
        g_s.broken = true;
        logf("encore_uir: D3D12 HUD-less/UI ring %ux%u format %d failed 0x%08lx; UIR off for this session",
             g_s.want_w, g_s.want_h, (int)g_s.want_fmt, hr);
        return false;
    }
    g_s.have_w = g_s.want_w;
    g_s.have_h = g_s.want_h;
    g_s.have_fmt = g_s.want_fmt;
    logf("encore_uir: D3D12 HUD-less (format %d) + UI (format %d) ring x%d, %ux%u, device %p (generation %u)",
         (int)g_s.have_fmt, (int)ui_fmt(), kRing, g_s.have_w, g_s.have_h, g_s.dev12, g_s.gen);
    return true;
}

// Present thread: once the formats are known, re-send Encore's last present-route options with them, so DLSS-G
// (re)creates its UIR resources for the real HUD-less and UI formats.
static void resend_options_if_needed(const void* viewport) {
    const uint32_t hud = static_cast<uint32_t>(g_s.have_fmt), ui = static_cast<uint32_t>(ui_fmt());
    if (!g_s.have_opts || !hud || (g_s.sent_hud_fmt == hud && g_s.sent_ui_fmt == ui))
        return;
    SetOptionsFn real = g_real_set_options.load();
    if (!real)
        return;
    alignas(16) uint8_t opts[sizeof(g_s.last_opts)];
    memcpy(opts, g_s.last_opts, sizeof(opts));
    opts[kOptUir] = kSlTrue;
    patch_flags(opts);
    apply_multiplier(opts);
    apply_suspension(opts);
    memcpy(opts + kOptHudlessFormat, &hud, sizeof(hud));
    memcpy(opts + kOptUiFormat, &ui, sizeof(ui));
    int r = real(viewport, opts);
    g_s.sent_hud_fmt = hud;
    g_s.sent_ui_fmt = ui;
    logf("encore_uir: present-route DLSS-G options re-sent with hudLessBufferFormat %u, uiBufferFormat %u: %d", hud,
         ui, r);
}

// Render thread (the thread Encore sets options on in KCD): suspend or resume frame generation by re-sending
// Encore's last present-route options with mode eOff (resources retained) or with Encore's own mode.
void set_fg_suspended(bool on) {
    if (!g_active || g_suspended.exchange(on) == on)
        return;
    std::lock_guard lock(g_s.m);
    SetOptionsFn real = g_real_set_options.load();
    if (!g_s.have_opts || !real) {
        logf("encore_uir: frame generation %s requested before Encore set options; applied on its next call",
             on ? "suspension" : "resume");
        return;
    }
    alignas(16) uint8_t opts[sizeof(g_s.last_opts)];
    memcpy(opts, g_s.last_opts, sizeof(opts));
    apply_multiplier(opts);
    apply_suspension(opts);
    uint32_t mode;
    memcpy(&mode, opts + kOptMode, sizeof(mode));
    const int r = real(reinterpret_cast<const void*>(g_base + kViewportRva), opts);
    logf("encore_uir: frame generation %s: DLSS-G options mode %u -> %d", on ? "suspended" : "resumed", mode, r);
}

// Render thread, once per frame (hudless::on_end_frame): apply a changed multiplier by re-sending Encore's last options.
void on_frame() {
    if (g_active) {
        update_encore_menu();
        log_encore_mfg_state();
    }
    if (!g_active || !refresh_multiplier() || g_suspended.load())
        return;
    std::lock_guard lock(g_s.m);
    SetOptionsFn real = g_real_set_options.load();
    if (!g_s.have_opts || !real)
        return;
    alignas(16) uint8_t opts[sizeof(g_s.last_opts)];
    memcpy(opts, g_s.last_opts, sizeof(opts));
    apply_multiplier(opts);
    uint32_t frames;
    memcpy(&frames, opts + kOptFrames, sizeof(frames));
    const int r = real(reinterpret_cast<const void*>(g_base + kViewportRva), opts);
    logf("encore_uir: DLSS-G options re-sent with numFramesToGenerate %u: %d", frames, r);
}

// Present thread, inside the present trial, before Encore's tag copies. Returns this frame's HUD-less texture (and
// sets *ui_slot) after making the guides' queue wait for the D3D11 copies, or null (stock behaviour).
static ID3D12Resource* consume(void* guides, int* ui_slot) {
    *ui_slot = -1;
    auto* g = static_cast<IUnknown**>(guides);
    IUnknown* queue_unk = g[0];  // ID3D12CommandQueue: executes the tag copy list
    IUnknown* depth_unk = g[1];  // ID3D12Resource: depth guide
    if (!queue_unk || !depth_unk)
        return nullptr;
    std::lock_guard lock(g_s.m);
    if (g_s.broken)
        return nullptr;
    if (!g_s.dev12) {
        ID3D12Resource* depth = nullptr;
        if (FAILED(depth_unk->QueryInterface(IID_PPV_ARGS(&depth))))
            return nullptr;
        HRESULT hr = depth->GetDevice(IID_PPV_ARGS(&g_s.dev12));
        depth->Release();
        if (FAILED(hr)) {
            g_s.broken = true;
            logf("encore_uir: guides' D3D12 device unavailable 0x%08lx; UIR off", hr);
            return nullptr;
        }
        logf("encore_uir: Encore present-route D3D12 device %p", g_s.dev12);
    }
    if (g_s.want_fmt != DXGI_FORMAT_UNKNOWN &&
        (!g_s.hud12[0] || g_s.want_w != g_s.have_w || g_s.want_h != g_s.have_h || g_s.want_fmt != g_s.have_fmt)) {
        if (create12())
            resend_options_if_needed(reinterpret_cast<const void*>(g_base + kViewportRva));
        return nullptr;  // the render thread opens the ring next frame
    }
    if (!g_s.hud12[0] || g_s.ready_gen != g_s.gen || g_s.ready_value <= g_s.consumed || g_s.ready_slot < 0) {
        ++g_s.missing;
        return nullptr;
    }
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(queue_unk->QueryInterface(IID_PPV_ARGS(&queue))))
        return nullptr;
    HRESULT hr = queue->Wait(g_s.ready12, g_s.ready_value);
    queue->Release();
    if (FAILED(hr)) {
        ++g_s.missing;
        return nullptr;
    }
    g_s.consumed = g_s.ready_value;
    ++g_s.tagged;
    *ui_slot = g_s.ready_slot;
    return g_s.hud12[g_s.ready_slot];
}

// Present thread, after Encore's input copy succeeded: tag the UI layer on our own list (Streamline copies it there),
// execute it on the guides' queue, then signal the release fence for the slot.
static void tag_ui_and_release(void* slot_ctx, void* guides, int slot) {
    auto* queue_unk = static_cast<IUnknown**>(guides)[0];
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(queue_unk->QueryInterface(IID_PPV_ARGS(&queue))))
        return;
    std::lock_guard lock(g_s.m);
    auto set_tag = *reinterpret_cast<SetTagFn*>(g_base + kSetTagPtrRva);
    const void* frame = *reinterpret_cast<void**>(static_cast<uint8_t*>(slot_ctx) + kSlotFrameToken);
    bool ui_ok = false;
    if (set_tag && frame && g_s.release12->GetCompletedValue() >= g_s.alloc_done[slot] &&
        SUCCEEDED(g_s.alloc[slot]->Reset()) && SUCCEEDED(g_s.list->Reset(g_s.alloc[slot], nullptr))) {
        alignas(16) uint8_t res[0x80] = {};
        memcpy(res + 0x08, kResourceGuid, 16);
        *reinterpret_cast<uint64_t*>(res + 0x18) = 1;      // version
        res[0x20] = 0;                                     // ResourceType::eTex2d
        *reinterpret_cast<void**>(res + 0x28) = g_s.ui12[slot];
        *reinterpret_cast<uint32_t*>(res + 0x40) = kStateNonPixelShaderResource;
        alignas(16) uint8_t tag[0x40] = {};
        memcpy(tag + 0x08, kResourceTagGuid, 16);
        *reinterpret_cast<uint64_t*>(tag + 0x18) = 1;      // version
        *reinterpret_cast<void**>(tag + 0x20) = res;
        *reinterpret_cast<uint32_t*>(tag + 0x28) = g_alpha_only ? kBufferTypeUIAlpha : kBufferTypeUIColorAndAlpha;
        *reinterpret_cast<uint32_t*>(tag + 0x2C) = kLifecycleOnlyValidNow;
        *reinterpret_cast<uint32_t*>(tag + 0x38) = g_s.have_w;  // extent {top, left, width, height}
        *reinterpret_cast<uint32_t*>(tag + 0x3C) = g_s.have_h;
        int r = set_tag(frame, reinterpret_cast<const void*>(g_base + kViewportRva), tag, 1, g_s.list);
        if (SUCCEEDED(g_s.list->Close()) && r == 0) {
            ID3D12CommandList* lists[] = {g_s.list};
            queue->ExecuteCommandLists(1, lists);
            ui_ok = true;
            ++g_s.ui_tagged;
        } else if (g_s.ui_skipped < 4) {
            logf("encore_uir: UI color/alpha tag refused: %d", r);
        }
    }
    if (!ui_ok)
        ++g_s.ui_skipped;
    queue->Signal(g_s.release12, ++g_s.release_value);
    queue->Release();
    g_s.alloc_done[slot] = g_s.release_value;
    g_s.slot_free[slot] = g_s.release_value;
    if (g_s.tagged == 1 || g_s.tagged % 3600 == 0)
        logf("encore_uir: tagged HUD-less %llu / UI %llu (UI skipped %llu) present-trial frames, %llu without a fresh "
             "image, copies %llu, slot waits %llu, options patched %llu", g_s.tagged, g_s.ui_tagged, g_s.ui_skipped,
             g_s.missing, g_s.submitted, g_s.reused_wait, g_options_patched.load());
}

// Render thread, g_s.m held: opens the current generation on D3D11. False if not ready.
static bool open11(ID3D11DeviceContext* ctx) {
    if (g_s.gen11 == g_s.gen && g_s.hud11[0])
        return true;
    if (!g_s.hud12[0] || !g_s.ready_h)
        return false;
    for (int i = 0; i < kRing; ++i) {
        release(g_s.hud11[i]);
        release(g_s.ui11[i]);
    }
    release(g_s.ready11);
    release(g_s.release11);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Device1* dev1 = nullptr;
    ID3D11Device5* dev5 = nullptr;
    HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&dev1));
    if (SUCCEEDED(hr))
        hr = dev->QueryInterface(IID_PPV_ARGS(&dev5));
    for (int i = 0; i < kRing && SUCCEEDED(hr); ++i) {
        hr = dev1->OpenSharedResource1(g_s.hud_h[i], IID_PPV_ARGS(&g_s.hud11[i]));
        if (SUCCEEDED(hr))
            hr = dev1->OpenSharedResource1(g_s.ui_h[i], IID_PPV_ARGS(&g_s.ui11[i]));
    }
    if (SUCCEEDED(hr))
        hr = dev5->OpenSharedFence(g_s.ready_h, IID_PPV_ARGS(&g_s.ready11));
    if (SUCCEEDED(hr))
        hr = dev5->OpenSharedFence(g_s.release_h, IID_PPV_ARGS(&g_s.release11));
    release(dev5);
    release(dev1);
    dev->Release();
    if (FAILED(hr)) {
        g_s.broken = true;
        logf("encore_uir: opening the shared ring on D3D11 failed 0x%08lx; UIR off", hr);
        return false;
    }
    for (int i = 0; i < kRing; ++i) {
        close(g_s.hud_h[i]);
        close(g_s.ui_h[i]);
        g_s.slot_free[i] = 0;
    }
    close(g_s.ready_h);
    close(g_s.release_h);
    g_s.gen11 = g_s.gen;
    g_s.value11 = 0;
    g_s.next_slot = 0;
    g_s.cur_slot = -1;
    logf("encore_uir: D3D11 side of the ring open (generation %u)", g_s.gen11);
    return true;
}

void copy_hudless(ID3D11DeviceContext* ctx, ID3D11Resource* src) {
    if (!g_active || !g_inputs || g_encore_menu_open.load() || !ctx || !src)
        return;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(src->QueryInterface(IID_PPV_ARGS(&tex))))
        return;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    tex->Release();
    DXGI_FORMAT f = typed(d.Format);
    if (f == DXGI_FORMAT_UNKNOWN || d.SampleDesc.Count != 1 || d.ArraySize != 1 || d.MipLevels != 1)
        return;
    std::lock_guard lock(g_s.m);
    if (g_s.broken)
        return;
    g_s.want_w = d.Width;
    g_s.want_h = d.Height;
    g_s.want_fmt = f;
    g_s.cur_slot = -1;
    if (!g_s.hud12[0] || g_s.have_w != d.Width || g_s.have_h != d.Height || g_s.have_fmt != f || !open11(ctx))
        return;  // the present thread (re)creates the ring
    ID3D11DeviceContext4* ctx4 = nullptr;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4))))
        return;
    const int slot = g_s.next_slot;
    if (g_s.slot_free[slot]) {
        ctx4->Wait(g_s.release11, g_s.slot_free[slot]);  // Streamline's copy of this slot's last use is done
        ++g_s.reused_wait;
    }
    ctx4->Release();
    ctx->CopyResource(g_s.hud11[slot], src);
    g_s.cur_slot = slot;
}

void finish_frame(ID3D11DeviceContext* ctx, ID3D11Resource* ui) {
    if (!g_active || !g_inputs || g_encore_menu_open.load() || !ctx || !ui)
        return;
    std::lock_guard lock(g_s.m);
    if (g_s.broken || g_s.cur_slot < 0 || g_s.gen11 != g_s.gen)
        return;
    const int slot = g_s.cur_slot;
    g_s.cur_slot = -1;
    ID3D11DeviceContext4* ctx4 = nullptr;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4))))
        return;
    ctx->CopyResource(g_s.ui11[slot], ui);
    ++g_s.value11;
    ctx4->Signal(g_s.ready11, g_s.value11);
    ctx4->Release();
    ctx->Flush();  // the D3D12 queue waits for this value; make sure it reaches the GPU
    g_s.ready_gen = g_s.gen11;
    g_s.ready_value = g_s.value11;
    g_s.ready_slot = slot;
    g_s.next_slot = (slot + 1) % kRing;
    ++g_s.submitted;
}

// ---- Hooks ----------------------------------------------------------------------------------------------------------
static bool __fastcall hk_input_copy(void* trial, void* slot, void* guides, ID3D12Resource* hudless, void* convert) {
    int ui_slot = -1;
    if (!hudless && guides && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_base + kTrialReturnRva) {
        if (ID3D12Resource* h = consume(guides, &ui_slot))
            hudless = h;
    }
    const bool ok = o_input_copy(trial, slot, guides, hudless, convert);
    if (ui_slot >= 0) {
        if (ok) {
            tag_ui_and_release(slot, guides, ui_slot);
        } else {
            // Encore refused this frame's inputs: free the slot without a UI tag.
            IUnknown* q = static_cast<IUnknown**>(guides)[0];
            ID3D12CommandQueue* queue = nullptr;
            if (q && SUCCEEDED(q->QueryInterface(IID_PPV_ARGS(&queue)))) {
                std::lock_guard lock(g_s.m);
                queue->Signal(g_s.release12, ++g_s.release_value);
                g_s.slot_free[ui_slot] = g_s.alloc_done[ui_slot] = g_s.release_value;
                queue->Release();
            }
        }
    }
    return ok;
}

static int __fastcall hk_set_options(const void* viewport, void* options) {
    const auto ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    bool ours = false;
    for (uintptr_t r : kOptionsReturnRvas)
        ours = ours || ra == g_base + r;
    if (ours && options) {
        auto* p = static_cast<uint8_t*>(options);
        uint64_t version;
        memcpy(&version, p + kOptVersion, sizeof(version));
        if (memcmp(p + kOptGuid, kDlssgOptionsGuid, sizeof(kDlssgOptionsGuid)) == 0 && version >= 4) {
            const uint8_t before = p[kOptUir];
            if (g_inputs) {
                p[kOptUir] = kSlTrue;
                patch_flags(p);
            }
            uint32_t hud, ui = static_cast<uint32_t>(ui_fmt());
            {
                std::lock_guard lock(g_s.m);
                hud = static_cast<uint32_t>(g_s.have_fmt);
                if (hud && g_inputs) {
                    memcpy(p + kOptHudlessFormat, &hud, sizeof(hud));
                    memcpy(p + kOptUiFormat, &ui, sizeof(ui));
                }
                memcpy(g_s.last_opts, p, kOptCopy);  // Encore's own mode, before any suspension
                memset(g_s.last_opts + kOptCopy, 0, sizeof(g_s.last_opts) - kOptCopy);
                g_s.have_opts = true;
                g_s.sent_hud_fmt = hud;
                g_s.sent_ui_fmt = hud ? ui : 0;
            }
            apply_multiplier(p);
            apply_suspension(p);
            uint64_t n = ++g_options_patched;
            if (n <= 8)
                logf("encore_uir: present-route DLSS-G options (v%llu, call +0x%llx): enableUserInterfaceRecomposition "
                     "%u -> 1, hudLessBufferFormat %u, uiBufferFormat %u, flags 0x%x", version,
                     (unsigned long long)(ra - g_base), before, hud, hud ? ui : 0,
                     *reinterpret_cast<uint32_t*>(p + kOptFlags));
        }
    }
    SetOptionsFn real = g_real_set_options.load();
    int r = real ? real(viewport, options) : -1;
    if (ours && g_options_patched.load() <= 8)
        logf("encore_uir: slDLSSGSetOptions -> %d", r);
    return r;
}

// ---- NGX probe: what DLSS-G actually receives ----------------------------------------------------------------------
using NgxCreateFn = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                               NVSDK_NGX_Handle**);
using NgxEvalFn = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                             const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
static NgxCreateFn o_ngx_create;
static NgxEvalFn o_ngx_eval;

static int get_uir(const NVSDK_NGX_Parameter* p) {
    int v = -1;
    if (NVSDK_NGX_FAILED(p->Get(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, &v)))
        return -1;
    return v;
}

static NVSDK_NGX_Result __cdecl hk_ngx_create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature id,
                                              NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out) {
    NVSDK_NGX_Result r = o_ngx_create(list, id, p, out);
    if (id == NVSDK_NGX_Feature_FrameGeneration && p)
        logf("ngx probe: DLSS-G CreateFeature -> 0x%08x, UserInterfaceRecompositionEnabled %d", (unsigned)r,
             get_uir(p));
    return r;
}

static NVSDK_NGX_Result __cdecl hk_ngx_eval(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* h,
                                            const NVSDK_NGX_Parameter* p, PFN_NVSDK_NGX_ProgressCallback cb) {
    if (p) {
        ID3D12Resource* bb = nullptr;
        if (NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_DLSSG_Parameter_Backbuffer, &bb)) && bb) {
            ID3D12Resource* hud = nullptr;
            ID3D12Resource* ui = nullptr;
            ID3D12Resource* ui_alpha = nullptr;
            p->Get(NVSDK_NGX_DLSSG_Parameter_HUDLess, &hud);
            p->Get(NVSDK_NGX_DLSSG_Parameter_UI, &ui);
            p->Get(NVSDK_NGX_DLSSG_Parameter_UIAlpha, &ui_alpha);
            unsigned frames = 0, frames_max = 0;
            p->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, &frames);
            p->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &frames_max);
            const int uir = get_uir(p);
            static int last = -100;
            static uint64_t n;
            const int key = (hud ? 1 : 0) | (ui ? 2 : 0) | (ui_alpha ? 1024 : 0) | ((uir & 0xFF) << 2) |
                            static_cast<int>((frames & 0xF) << 12) | static_cast<int>((frames_max & 0xF) << 16);
            ++n;
            if (key != last || n % 3600 == 0) {
                logf("ngx probe: DLSS-G evaluate #%llu: HUDLess %s, UI %s, UIAlpha %s, UserInterfaceRecompositionEnabled %d, "
                     "MultiFrameCount %u (max %u)", n, hud ? "set" : "none", ui ? "set" : "none",
                     ui_alpha ? "set" : "none", uir, frames, frames_max);
                last = key;
            }
        }
    }
    return o_ngx_eval(list, h, p, cb);
}

static bool hook_ngx() {
    HMODULE ngx = GetModuleHandleW(L"_nvngx.dll");
    if (!ngx)
        return false;
    void* c = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
    void* e = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
    MH_STATUS sc = c ? MH_CreateHook(c, &hk_ngx_create, reinterpret_cast<void**>(&o_ngx_create)) : MH_ERROR_NOT_EXECUTABLE;
    MH_STATUS se = e ? MH_CreateHook(e, &hk_ngx_eval, reinterpret_cast<void**>(&o_ngx_eval)) : MH_ERROR_NOT_EXECUTABLE;
    if (sc == MH_OK)
        MH_EnableHook(c);
    if (se == MH_OK)
        MH_EnableHook(e);
    logf("ngx probe: _nvngx.dll %p D3D12 CreateFeature %s, EvaluateFeature %s", ngx, MH_StatusToString(sc),
         MH_StatusToString(se));
    return true;
}

// Wraps slDLSSGSetOptions and installs the NGX probe once _nvngx.dll is loaded, before DLSS-G is created.
// RTX Encore treats the module that owns the resolved function as the frame generation provider and takes its
// multiplier limit from it, so options_detour = 1 hooks the function in place inside sl.dlss_g and leaves Encore's
// pointer alone. options_detour = 0 (or a failed detour) swaps the pointer, as v1.1.0 did (Encore then limits to 2x).
static DWORD WINAPI options_watch(LPVOID) {
    auto* slot = reinterpret_cast<void* volatile*>(g_base + kSetOptionsPtrRva);
    void* const mine = reinterpret_cast<void*>(&hk_set_options);
    void* hooked[4] = {};  // detoured targets (a reloaded sl.dlss_g gets a new one; old ones are never touched)
    bool wrapped = false, probed = false, detour = g_options_detour;
    for (;;) {
        void* cur = *slot;
        if (cur && cur != mine && detour) {
            bool known = false;
            for (void* h : hooked)
                known = known || h == cur;
            if (!known) {
                SetOptionsFn tramp = nullptr;
                MH_STATUS c = MH_CreateHook(cur, mine, reinterpret_cast<void**>(&tramp));
                MH_STATUS e = c == MH_OK ? MH_EnableHook(cur) : c;
                HMODULE owner = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(cur), &owner);
                wchar_t name[MAX_PATH] = L"?";
                if (owner)
                    GetModuleFileNameW(owner, name, MAX_PATH);
                const wchar_t* base = wcsrchr(name, L'\\');
                logf("encore_uir: slDLSSGSetOptions %p (%ls) detoured in place %s/%s; Encore's pointer left unchanged",
                     cur, base ? base + 1 : name, MH_StatusToString(c), MH_StatusToString(e));
                for (void*& h : hooked)
                    if (!h) {
                        h = cur;
                        break;
                    }
                if (e == MH_OK) {
                    g_real_set_options.store(tramp);
                    wrapped = true;
                } else {
                    if (c == MH_OK)
                        MH_RemoveHook(cur);
                    detour = false;  // keep UIR working through the pointer swap (multiplier ceiling stays 2x)
                    logf("encore_uir: detour failed; falling back to the pointer swap");
                    continue;
                }
            }
        } else if (cur && cur != mine &&
                   InterlockedCompareExchangePointer(const_cast<void**>(slot), mine, cur) == cur) {
            g_real_set_options.store(reinterpret_cast<SetOptionsFn>(cur));
            logf("encore_uir: slDLSSGSetOptions pointer %p wrapped (options_detour 0: Encore admits this DLL as wrapper)",
                 cur);
            wrapped = true;
        }
        if (!probed)
            probed = hook_ngx();
        Sleep(wrapped && probed ? 50 : 1);
    }
}

static bool matches(uintptr_t rva, const uint8_t* bytes, size_t n) {
    return memcmp(reinterpret_cast<const void*>(g_base + rva), bytes, n) == 0;
}

bool install() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"version.dll";
    HMODULE mod = GetModuleHandleW(path.c_str());
    if (!mod || !GetProcAddress(mod, "RTXEncore_ClaimsProcess")) {
        logf("encore_uir: RTX Encore (version.dll next to the game) not loaded; off");
        return false;
    }
    g_base = reinterpret_cast<uintptr_t>(mod);
    g_jsonc_path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"rtx-encore.jsonc";
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    const bool build = nt->FileHeader.TimeDateStamp == kTimestamp && nt->OptionalHeader.SizeOfImage == kSizeOfImage;
    bool code = build && matches(kTrialCallRva, kTrialCallBytes, sizeof(kTrialCallBytes)) &&
                matches(kInputCopyRva, kInputCopyPrologue, sizeof(kInputCopyPrologue)) &&
                matches(kOptionsLoadRva, kOptionsLoadBytes, sizeof(kOptionsLoadBytes)) &&
                matches(kSetTagCallRva, kSetTagCallBytes, sizeof(kSetTagCallBytes));
    for (uintptr_t r : kOptionsReturnRvas)
        code = code && matches(r - 2, kCallRax, sizeof(kCallRax));
    if (!code) {
        logf("encore_uir: version.dll %p timestamp 0x%08lx size 0x%lx is not the analysed RTX Encore 2.5.0 build "
             "(build %d); off", mod, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, build);
        return false;
    }
    void* target = reinterpret_cast<void*>(g_base + kInputCopyRva);
    MH_STATUS c = MH_CreateHook(target, &hk_input_copy, reinterpret_cast<void**>(&o_input_copy));
    MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
    logf("encore_uir: RTX Encore 2.5.0 at %p matches; present-route input copy hook %s/%s", mod,
         MH_StatusToString(c), MH_StatusToString(e));
    if (e != MH_OK)
        return false;
    g_active = true;
    if (g_unlock_mfg) {
        bool sites = matches(kApiProbeRva, kApiProbeBytes, sizeof(kApiProbeBytes));
        for (uintptr_t r : kMfgGateReturnRvas) {  // each site must be "call 0x1B3310"
            const auto* p = reinterpret_cast<const uint8_t*>(g_base + r - 5);
            int32_t rel;
            memcpy(&rel, p + 1, sizeof(rel));
            sites = sites && p[0] == 0xE8 && r + rel == kApiProbeRva;
        }
        void* probe = reinterpret_cast<void*>(g_base + kApiProbeRva);
        MH_STATUS pc = sites ? MH_CreateHook(probe, &hk_api_probe, reinterpret_cast<void**>(&o_api_probe)) : MH_UNKNOWN;
        MH_STATUS pe = pc == MH_OK ? MH_EnableHook(probe) : pc;
        logf("encore_uir: multiplier unlock (DX11 2x cap): sites %s, hook %s", sites ? "match" : "DIFFER", MH_StatusToString(pe));
    }
    HANDLE t = CreateThread(nullptr, 0, options_watch, nullptr, 0, nullptr);
    if (t)
        CloseHandle(t);
    return true;
}

}  // namespace kc::encore_uir
