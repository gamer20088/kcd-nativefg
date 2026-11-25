#pragma once
#include <windows.h>

#include <string>

namespace kc {

// kcd_clarity.ini next to the DLL. Missing keys get defaults; the file is written back on load
// so every setting is visible to the user.
struct Config {
    int mode = 1;              // [general] mode: 0 = off (stock game), 1 = feed motion vectors + depth through DLSS
    int output = 0;            // [general] output: 0 = keep the game's own AA image (SMAA 2TX) on screen,
                               //                   1 = show the DLAA image instead
    int dlss_preset = 0;       // [dlss] preset: NGX render preset, 0 = DLSS default, 10-13 = J-M
    bool velocity16 = true;    // [dlss] velocity16: object motion vectors at 16 bits (restart to change)
    bool velocity_cleanup = true;  // [dlss] velocity_cleanup: camera motion on occluders (velocity_mask)
    bool force_halton = true;  // [dlss] force_halton: r_AntialiasingTAAPattern 8, only while output = 1
    bool cut_guard = true;     // [dlss] cut_guard: zero the motion vectors on camera cuts
    bool map_fix = true;       // [dlss] map_fix: zero motion vectors to Encore on map/menu frames
    bool encore_uir = true;    // [encore] uir: experimental HUD-less image + UI recomposition on Encore 2.5.0's
                               //               DX11 present route (encore/encore_uir.h); 0 = no inputs to DLSS-G
                               //               (with menu_suspend = 0 as well: previous behaviour)
    bool encore_menu_detection = true;  // [encore] menu_detection: DLSS-G full-screen menu detection (needs uir)
    bool encore_ui_alpha = false;  // [encore] ui_input: 0 = UI color+alpha, 1 = UI alpha only (kBufferTypeUIAlpha)
    bool encore_menu_suspend = true;  // [encore] menu_suspend: no frame generation without 3D scene / in menus
    int encore_menu_coverage = 50;    // [encore] menu_coverage: interface coverage (%) that counts as a menu
    int encore_fg_multiplier = -1;   // [encore] fg_multiplier: -1 follow rtx-encore.jsonc, 0 Encore's request, 2-6
    int encore_menu_draws = 0;        // [encore] menu_draws: interface draws per frame that count as a menu (0 = off)
};

Config& config();
void config_load(const std::wstring& path);
void config_save();

}  // namespace kc
