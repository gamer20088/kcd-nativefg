# KCD NativeFG: architecture

KCD NativeFG is a `dinput8.dll` proxy, loaded by the game's `WHGame.dll`. It hooks the game's Direct3D 11 renderer
and works with RTX Encore 2.5.0, which runs DLSS Frame Generation (DLSS-G) on its own Direct3D 12 Streamline backend.

```
KCD1 (D3D11) ──► dinput8.dll (KCD NativeFG) ──► DLAA call with motion vectors + depth ──► RTX Encore (version.dll)
                      │                                                                     │
                      └─► HUD-less image + UI alpha (shared D3D11/D3D12 textures) ─────────► DLSS-G (Streamline, D3D12)
```

## Components (`src/`)

| Module | Role |
| --- | --- |
| `hooks/d3d11_hooks`, `hooks/present` | D3D11 device and context hooks (MinHook). The engine's `RT_EndFrame` marks the frame boundary. |
| `renderer/taa_dlss` | At the game's SMAA 2TX resolve, builds full-screen motion vectors from the engine's reprojection matrix and object velocity, plus a depth copy, then makes a DLAA call. RTX Encore reads its frame-generation guides from that call. `output=0` discards the DLAA image (the game's AA stays on screen); `output=1` shows it. |
| `renderer/velocity_format`, `renderer/velocity_mask` | Higher-precision object velocity and camera motion on occluders. |
| `renderer/hudless` | Copies the final image just before the first interface draw (HUD-less). It then replays each interface draw into a separate UI layer, and a closing pass makes `final = ui + (1 - ui.a) * hudless` hold. It also measures interface coverage and draw counts for menu detection. |
| `encore/encore_uir` | The RTX Encore 2.5.0 integration. It is active only when RTX Encore's build matches exactly (PE timestamp, image size and code bytes). It hands the HUD-less image and the UI alpha to Encore's DX11 present route, enables UI recomposition in the DLSS-G options, pauses frame generation in menus, and passes the selected multiplier. It wraps Streamline's `slDLSSGSetOptions` with an in-place detour inside `sl.dlss_g.dll`, so RTX Encore still sees `sl.dlss_g.dll` as the frame generation provider and keeps offering its higher multipliers (v1.1.0 and earlier redirected RTX Encore's function pointer into this DLL, which RTX Encore then limited to 2x). It also reads RTX Encore's own menu-visible state, behind a code-byte check of that exact build. Textures and fences are shared between D3D11 and D3D12, through a three-slot ring with ready/release fences. |
| `upscaler/dlss` | NGX (DLSS SDK) for the DLAA call. |

## Frame flow

1. **Scene:** the game renders the scene. At the SMAA 2TX draw: motion vectors, depth, and the DLAA call that RTX Encore captures.
2. **Interface starts:** at the first interface draw, the HUD-less copy is taken. Interface draws run normally and are replayed into the UI layer.
3. **`RT_EndFrame`:** the UI layer is closed and both images are copied into the shared ring; a fence is signalled.
4. **Present:** RTX Encore's present route tags depth, motion, HUD-less and UI alpha for DLSS-G, after waiting for that fence.
5. **Menus:** while no 3D scene is drawn, or the interface both covers most of the screen and uses many draws, frame generation is paused (DLSS-G mode off, resources kept). It resumes after a few normal frames. Requiring both signals (v1.2.0) stops the pauses that busy gameplay scenes used to trigger.
6. **RTX Encore's own menu:** while it is open, the HUD-less image and UI alpha are withheld, so DLSS-G interpolates the whole image, with the menu, instead of recomposing a HUD that does not contain it.

The DLAA call exists only to deliver motion vectors and depth. Because a DLSS feature is created and evaluated, RTX Encore's overlay reports DLSS Super Resolution. With `output=0` its result is discarded and nothing is upscaled.

## Safety

- Nothing is written to the game's or RTX Encore's files on disk.
- With any RTX Encore build other than 2.5.0, the RTX Encore features turn themselves off. Motion vectors through the DLAA call keep working.
- `[encore] uir=0` and `menu_suspend=0` give the plain motion-vector behaviour.
- `[encore] options_detour=0` restores the v1.1.0 pointer swap (RTX Encore then limits the multiplier to 2x). If the detour cannot be installed, the add-on falls back to the pointer swap on its own.
- `[encore] arch_spoof` (an unsuccessful experiment) is off by default and is not needed.

## Diagnostics

`kcd_clarity.log` records:
- hook installation
- what DLSS-G receives (`ngx probe:` lines: HUD-less, UI alpha, UI recomposition, multi-frame count)
- frame generation pauses and resumes
- RTX Encore's menu opening and closing, and its multiplier limit (`Encore MFG state:` lines)

F10 saves the HUD-less image and the UI layers of the current frame as BMP files.
