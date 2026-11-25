# KCD NativeFG | Native Frame Generation and HUD Fix for RTX Encore

An add-on for **Kingdom Come: Deliverance** (the first game, DirectX 11) that gives
[RTX Encore](https://github.com/SilyNoMeta/rtx-encore)'s DLSS Frame Generation the game's real motion vectors and
depth, and separates the HUD from the 3D scene for DLSS-G UI recomposition. In our testing (one system, RTX 3080 Ti,
2x and 3x frame generation) the HUD, interaction text and menu transitions were clearly cleaner than with RTX Encore
alone; results on other systems may differ.

KCD NativeFG is unofficial and not affiliated with Warhorse Studios, NVIDIA or the RTX Encore project. Based on
[KCD1 Clarity](https://github.com/eonflux7/kcd1-clarity).

## What it does

- **Game motion vectors and depth.** At the game's SMAA 2TX resolve, it builds full-screen motion vectors and a depth
  copy and makes a DLAA call that RTX Encore reads them from. By default the DLAA image is discarded, so **the game's
  own anti-aliasing stays on screen** (`[general] output=0`). Set `output=1` to show DLAA instead.
- **HUD separation (UI recomposition).** It captures the image just before the interface is drawn (HUD-less) and the
  interface's alpha, and hands both to DLSS-G through RTX Encore. DLSS-G then recomposes the HUD instead of
  interpolating it. In our testing this removed the visible warping of the compass, status icons and interaction text;
NPC subtitles have not been checked separately.
- **Menu handling.** Frame generation pauses while the map, inventory or loading screens are shown, and resumes
  afterwards. In our testing this removed the smearing on map and inventory transitions. Since v1.2.0 a pause needs
  both many interface draws and most of the screen covered, which stopped the random 5-15 s pauses in busy scenes.
- **Higher multipliers through RTX Encore's own menu** (v1.2.0). Earlier versions accidentally made RTX Encore treat
  this add-on as the frame generation provider, and RTX Encore then greyed out everything above 2x. v1.2.0 hooks
  Streamline in place, so RTX Encore recognises the real provider (`sl.dlss_g.dll`) and offers its higher
  multipliers again. You choose the multiplier in RTX Encore; the add-on does not force one.
- **RTX Encore's overlay** (v1.2.0). While RTX Encore's own menu is open (read from its real menu state, not a
  hotkey), the HUD-less and UI inputs are withheld so the menu stays solid during camera movement.

### Why RTX Encore shows "DLSS SR"

The motion vectors and depth reach RTX Encore through an internal DLAA evaluation, so a DLSS feature exists and RTX
Encore's overlay lists DLSS Super Resolution. With `output=0` (default) that result is discarded: nothing is
upscaled and the image on screen is the game's own SMAA 2TX at native resolution.

## Releases

| Release | Status |
| --- | --- |
| **v1.2.0** (recommended) | **Verified in gameplay** (RTX 3080 Ti, driver 617.42, 2560x1440, Windows HDR + RTX HDR) at **2x** and **3x**. At 3x, RTX Encore's present receipts report 3 presented frames per rendered frame (`requested=x3 actualPresented=3`). **4x, 5x and 6x are selectable but unverified** (RTX Encore itself marks 5x and 6x experimental). |
| v1.1.0-experimental (prerelease) | Superseded by v1.2.0, kept for rollback. Its multiplier option could not lift RTX Encore's 2x menu limit. |
| v1.0.0 | 2x build, verified in gameplay, kept for rollback. |

## Requirements

- Kingdom Come: Deliverance (Steam) **1.9.x**, DirectX 11, **native resolution**. Anti-aliasing **SMAA 2TX** in the game's video options.
- An NVIDIA RTX graphics card, a recent driver, and Hardware-accelerated GPU scheduling on.
- **RTX Encore 2.5.0**, exactly. The add-on checks the build and switches its RTX Encore features off for any other version.
- NVIDIA Streamline SDK **2.14.16** runtime files: `sl.*.dll`, `nvngx_dlss.dll`, `nvngx_dlssg.dll`.
- Optional: Windows HDR plus NVIDIA RTX HDR (NVIDIA App). Tested working together.

RTX Encore and the NVIDIA files are **not** included here. The installer downloads them from their official GitHub
releases and checks every file against a pinned SHA-256.

## Install

Close the game. Download the repository (or just `tools/Install-KCDEncore.ps1` from a release), then in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File tools\Install-KCDEncore.ps1
```

- **What it does:** finds the game through Steam and downloads:
  - RTX Encore 2.5.0 (`rtx-encore.dll`, installed as `version.dll`)
  - NVIDIA Streamline SDK 2.14.16
  - this add-on's `dinput8.dll` and `kcd_clarity.ini`

  It backs up everything it replaces into `Bin\Win64\.kcd-encore\`, then installs.
- **Other actions:** `-Action Status`, `-Action Update` (keeps your `kcd_clarity.ini` and `rtx-encore.jsonc`;
  new keys get their defaults), `-Action Rollback` (undoes the last
  install or update), `-Action Uninstall`.
- **Options:**
  - `-GameDir <...\KingdomComeDeliverance>` if Steam detection fails.
  - `-Release v1.0.0` (or another tag) installs an older release, for example to roll back.
- **Other mods:** the installer only removes files it installed. It refuses to replace another mod's `dinput8.dll`
  or `version.dll` unless you pass `-Force`, and backs that file up first. Another `dinput8.dll` mod (Ultimate ASI
  Loader, full KCD1 Clarity) cannot be used together with this one.

Manual install:
1. Install RTX Encore 2.5.0 as its documentation describes, with `version.dll` and the Streamline 2.14.16 files.
2. Copy `dinput8.dll` and `kcd_clarity.ini` from the release into `Bin\Win64`.

## RTX Encore settings

Open RTX Encore's menu in game (its menu key, **Insert** by default), or edit `rtx-encore.jsonc`:

- Frame generation for games without frame generation: **on**. Engine: **DLSS-FG**.
- Frame generation mode: **Fixed**. Multiplier: **2** is the conservative recommendation. **3** is verified in our
  testing; 4 to 6 are available but unverified. Choose it in RTX Encore's menu.
- Motion: **Game motion vectors**.
- RTX Encore's own upscaling: **off**.
- FG preset: game, A or B. Not C (Lite), which turns UI recomposition off.

## Add-on settings (`Bin\Win64\kcd_clarity.ini`)

| Key | Shipped value | Meaning |
| --- | --- | --- |
| `[general] mode` | 1 | 0 = add-on off (stock game) |
| `[general] output` | 0 | 0 = the game's SMAA 2TX on screen, 1 = DLAA on screen |
| `[dlss] map_fix` | 1 | Motion vectors to RTX Encore on map and menu frames |
| `[encore] uir` | 1 | Hand the HUD-less image and UI to DLSS-G (UI recomposition). 0 = RTX Encore estimates the UI itself |
| `[encore] ui_input` | 1 | 1 = UI alpha (recommended), 0 = UI colour + alpha |
| `[encore] menu_suspend` | 1 | Pause frame generation in menus and loading |
| `[encore] menu_draws` | 180 | Interface draws per frame that count as a menu (inventory) |
| `[encore] menu_coverage` | 70 | Interface coverage (%) that counts as a menu |
| `[encore] fg_multiplier` | -1 | -1 = the multiplier chosen in RTX Encore (recommended), 0 = RTX Encore's own request, 2-6 = force this multiplier |
| `[encore] options_detour` | 1 | 1 = hook Streamline in place, so RTX Encore offers its higher multipliers. 0 = v1.1.0 behaviour (2x limit) |
| `[encore] arch_spoof` | 0 | Unsuccessful experiment; leave at 0 |

## Known limitations and observations

- RTX Encore's own overlay can show negligible residual artifacts around its borders while the camera moves.
- One very minor artifact near the screen centre was seen once, in one indoor dialogue while switching speakers.
  It was not seen in other conversations; its cause is unknown and it is not a confirmed general issue.
- Works only with RTX Encore 2.5.0 (exact build) and Streamline 2.14.16; other versions switch the RTX Encore
  features off.
- Multipliers above 3x have not been verified.

Version history: [CHANGELOG.md](CHANGELOG.md).

Diagnostics: `kcd_clarity.log` next to the game. F10 saves the current HUD-less and UI layers as BMP files.

## Building

Needs Visual Studio 2022 Build Tools with C++ x64. Run `build.cmd`; the output is `build\dinput8.dll`. CMake
downloads MinHook and the needed DLSS SDK files and checks their hashes. Regression tests are in `tests/`; their
build commands are at the top of each file. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) describes how it works.

## Credits and licenses

- **[KCD1 Clarity](https://github.com/eonflux7/kcd1-clarity)** and its contributors: the base of this add-on (TAA hook,
  motion vector and depth reconstruction, engine research). MIT license, see [LICENSE](LICENSE).
- **[RTX Encore](https://github.com/SilyNoMeta/rtx-encore)** by SilyNoMeta: the frame generation this add-on feeds.
  Not included; download it from its releases.
- **NVIDIA**: DLSS, DLSS Frame Generation, Streamline and the DLSS SDK, under NVIDIA's licenses. Not included except
  what is linked from the DLSS SDK at build time; see [THIRD_PARTY.md](THIRD_PARTY.md).
- [MinHook](https://github.com/TsudaKageyu/minhook): BSD 2-Clause.

Kingdom Come: Deliverance © Warhorse Studios / Deep Silver. This repository contains none of the game's files.
