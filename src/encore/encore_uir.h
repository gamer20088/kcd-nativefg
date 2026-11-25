#pragma once
#include <d3d11.h>

#include <cstdint>

// Experimental: HUD-less image and UI color/alpha layer for RTX Encore 2.5.0's DX11 present route, so DLSS-G runs
// UI recomposition (UIR) instead of interpolating the interface with the scene.
//
// Evidence (static analysis of the installed VERSION.dll, build "quality-strict-shadow-stock", PE timestamp
// 0x6AFD7776; RVAs below; see docs/ARCHITECTURE.md):
//   - 0x200E0 copies the present-route guides and tags them with slSetTagForFrame: arg 3 = depth/motion guides,
//     arg 4 = an ID3D12Resource tagged as kBufferTypeHUDLessColor (2) when non-null. The D3D11 present trial
//     (0x1CF20) passes `xor r9d,r9d` at 0x1D2F6.
//   - 0x1F6E0 builds the present-route sl::DLSSGOptions (v5) with enableUserInterfaceRecomposition (+0x70) = eFalse
//     and calls slDLSSGSetOptions through the pointer at RVA 0x1201E70.
//   - NGX (nvsdk_ngx_defs_dlssg.h): "For UIR frame generation, both HUDless and one of UI or UIAlpha must be
//     provided." 0x200E0 tags no UI layer, so we tag kBufferTypeUIColorAndAlpha (23) ourselves through Encore's
//     slSetTagForFrame pointer (RVA 0x1201E58), with Encore's frame token and viewport.
// Only applied when every byte relied on matches that build. VERSION.dll on disk is never modified.
namespace kc::encore_uir {

// Init thread, after MinHook is initialized. Returns false (and does nothing) if Encore is missing or differs.
bool install();
bool active();
// [encore] menu_detection: add DLSSGFlags::eEnableFullscreenMenuDetection to the present-route options.
void set_menu_detection(bool on);
// [encore] ui_input: alpha = tag kBufferTypeUIAlpha (R8, NVIDIA's preferred input) instead of UIColorAndAlpha.
void set_ui_alpha_only(bool on);
bool ui_alpha_only();
// Render thread: suspend (options mode eOff, resources retained) or resume frame generation, e.g. for menus.
void set_fg_suspended(bool on);
// [encore] uir: false = keep only the options wrapper (menu suspension); no HUD-less/UI inputs, UIR left as Encore sets it.
void set_inputs(bool on);

constexpr DXGI_FORMAT kUiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;  // premultiplied UI color + coverage alpha

// Render thread (D3D11 immediate context), once per frame in this order:
//   copy_hudless: `src` is the final image without the interface (just before the first interface draw).
//   finish_frame: `ui` is the UI layer (kUiFormat, or R8_UNORM alpha with ui_alpha_only, same size); signals the
//                 shared fence for the present route.
void copy_hudless(ID3D11DeviceContext* ctx, ID3D11Resource* src);
void finish_frame(ID3D11DeviceContext* ctx, ID3D11Resource* ui);

}  // namespace kc::encore_uir
