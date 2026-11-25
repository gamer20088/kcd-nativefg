# Changelog

## v1.2.0 (2026-11-25)

- **Higher multipliers in RTX Encore's own menu.** v1.0.0 and v1.1.0 redirected RTX Encore's `slDLSSGSetOptions`
  pointer into `dinput8.dll`. RTX Encore then treated this add-on as the frame generation provider and limited the
  multiplier to 2x, greying out everything above. The options hook is now an in-place detour inside `sl.dlss_g.dll`,
  so RTX Encore recognises the real Streamline provider. `[encore] options_detour` (default 1; 0 restores the old
  behaviour). Verified in gameplay at 3x (RTX Encore reports `actualPresented=3`); 4x-6x unverified.
- **Menu detection:** with a 3D scene, a pause now needs both many interface draws and high coverage. This ends the
  random frame generation pauses in busy scenes; map, inventory and loading protection are unchanged.
- **RTX Encore overlay:** its real menu-visible state is read, guarded by a code-byte check of RTX Encore 2.5.0, with
  its menu key as the fallback. While the overlay is open, the HUD-less and UI inputs are withheld so it stays solid.
- `[encore] fg_multiplier` default -1 follows the multiplier chosen in RTX Encore.
- `[encore] arch_spoof`: an unsuccessful GPU-architecture experiment, off by default.
- Documentation: why RTX Encore lists DLSS SR while the game's own anti-aliasing is on screen.
- Unchanged: game motion vectors and depth, UIAlpha recomposition, HUD, compass, status icon and interaction text
  protection, native game AA with optional DLAA, Windows HDR + RTX HDR compatibility.

## v1.1.0-experimental (2026-11-25)

- `fg_multiplier`: passes the RTX Encore multiplier to DLSS-G. Did not lift RTX Encore's 2x menu limit.

## v1.0.0 (2026-11-25)

- Game motion vectors and depth for RTX Encore, DLSS-G UI recomposition (HUD-less + UI alpha), menu pausing.
  Verified at 2x.
