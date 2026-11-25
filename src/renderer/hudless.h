#pragma once
#include <d3d11.h>

// Experimental (encore_uir): per frame, the final image just before the interface (HUD-less) and the interface on
// its own (UI color + alpha), for DLSS-G UI recomposition.
//
// "Final image" = the render target of the last draw of the previous frame. The HUD-less snapshot is taken at the
// first draw into it that is not a 3-vertex full-screen pass, once a full-screen pass has written the scene into it.
// From then on, every draw into the final image is replayed once more into a private UI layer (cleared to 0):
//   same shaders, inputs, viewport and depth/stencil test; color blend as the game's; alpha blend ONE/INV_SRC_ALPHA
//   (opaque draws: alpha 1); depth/stencil writes off.
// For "over"-blended interface that gives premultiplied color and coverage, so final = ui + (1 - ui.a) * hudless,
// the relation DLSS-G expects. The game's own draws and image are untouched.
// Frames without interface hand over the final image and an empty layer at the end of the frame.
//
// Frame boundary and render thread come from the engine's CD3D9Renderer::RT_EndFrame, not from the DXGI Present
// hook: Encore's present route can run before the native Present, and Streamline's generated frames call Present
// from another thread.
namespace kc::hudless {

void set_enabled(bool on);
bool enabled();

// Render thread, from the D3D11 hooks (not for our own passes).
void on_set_rt0(ID3D11RenderTargetView* rtv);
// kind: D Draw, I DrawIndexed, J DrawIndexedInstanced, N DrawInstanced; count = vertices or indices (diagnostics).
void before_draw(ID3D11DeviceContext* ctx, bool fullscreen_tri, UINT count, char kind);
// [encore] menu_suspend / menu_coverage: suspend frame generation while no 3D scene is drawn (map, loading) or the
// interface covers at least `coverage` of the screen (read back 2-3 frames late), or, if `draws` > 0, this frame drew at
// least `draws` interface draws (same frame).
void set_menu_suspend(bool on, float coverage, UINT draws);
// After the game's draw: true if it went into the final image during the interface phase. Then, with the hooks'
// re-entrancy guard held: replay_begin; the same draw call again; replay_end.
bool should_replay();
bool replay_begin(ID3D11DeviceContext* ctx);
void replay_end(ID3D11DeviceContext* ctx);
// Render thread, at the start of RT_EndFrame (interface drawn, before Present).
void on_end_frame();

}  // namespace kc::hudless
