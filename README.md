<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->
# Driveclub-PC

A personal hobby fork of the shadPS4 PS4 emulator, tuned
to run Driveclub (CUSA00093, v1.28) on Windows. It is built on
GR2fork by junminlee2004.

This fork was created to help bring this project and build to Linux users.

    Game log files on this build are located at log/shad_log.txt and log/shadps4.log (instead of log/CUSA00093.log).

    Save data is located at ~/.local/share/shadPS4/home/1000/savedata/CUSA00093, containing PROFILE, GHOSTS, and REPLAYS.

    Per-game overrides go to ~/.local/share/shadPS4/custom_configs/CUSA00093.json.
    thanks to @dougbug589

As I do not have a formal programming background, contributions from experienced developers are very welcome. If you would like to fix issues, improve Linux compatibility, or continue maintaining this version, feel free to submit Pull Requests or create your own fork.

**No game data, firmware or Sony code is included.** You need your own dump of Driveclub
(base game plus the 1.28 update) and the PS4 system modules from your own console.

Unofficial, and not affiliated with Sony Interactive Entertainment, Evolution Studios, the
shadPS4 team or the GR2fork author. Names and trademarks belong to their owners.

**Current release: 0.2.5.** See [release notes](driveclub-pc/RELEASE-0.2.5.md) for changes, installation and known limitations.

## Tested version

Only **Driveclub CUSA00093 with update 1.28** has been tested. Other regions are accepted as experimental; local gameplay validation remains CUSA00093 v1.28 on NVIDIA.

## How to use

1. Download the release zip and extract it into its own folder.
2. Get your dumps ready:
   - the base game folder (`CUSA00093`), and next to it the 1.28 update folder named
     `<base folder>-patch` or `<base folder>-UPDATE`;
   - the PS4 firmware modules (`sys_modules`) from your own console.
3. Run `Driveclub-PC.exe`. A settings window opens:

| Setting | What to choose |
|---|---|
| Game folder | the base game folder (`CUSA00093`) |
| Firmware | your `sys_modules` folder. **Import...** copies it into `user\sys_modules`, so the original can move later |
| Frame rate | 30 / **60 (default)** / **60 + FG** / 120 (experimental). FG generates intermediate frames for approximately 120 presentations/s; the FPS counter shows real game FPS |
| Display / Window size | borderless fullscreen or window; the window size only applies in windowed mode (fullscreen always covers the display). Includes the ultrawide sizes 2560x1080, 3440x1440 and 5120x1440 |
| Render resolution | **1080p (native)** / 1440p / 2160p: the number of lines the game renders (the engine's own scaling; extra video memory is set up automatically). The width follows the aspect ratio of the display (fullscreen) or window, see *Ultrawide* below. 1440p costs about 10–15 % FPS; 2160p is very demanding |
| Upscaling / Sharpening | AMD FSR 1 or bilinear; RCAS sharpening off / normal / strong |
| Anisotropic filter | 16x by default; Off keeps the game's own setting |
| Anti-aliasing | Off / **FXAA (default)** / SMAA 1x High |
| Prefer performance cores | on by default; keeps the busiest threads on P-cores of hybrid Intel CPUs |
| Compile shaders in the background | on by default: new shaders and pipelines are built off the main thread, so first-time content does not freeze the game (an object may appear a few frames late). Turn off if you see graphics errors |
| GPU readbacks | **Precise (default)**. Relaxed is faster but causes shadow and vegetation glitches; Off makes the 3D world nearly black |
| Save-data root | choose a custom save location, open it, or restore the default; existing saves are never moved automatically |
| Classic PS4 frame pacing | fallback: only turn it on if game speed feels wrong |
| Show this window at start | if turned off, the game starts directly; hold **Shift** while launching to open the window again |

4. Press **Play**. Before the game boots, the launcher checks the folder, the title ID, the update
   version and the firmware, and explains any problem it finds.

The 60 FPS mode supports the original v1.28 executable. The 120 FPS mode remains experimental, including game timing and achievable frame rate.

**Ultrawide:** on a display or window wider than 16:9 the game renders a native wide image: the
scene itself is rendered at the wide size (e.g. 3440x1440 at 1440p, nothing is stretched) with a
wider horizontal field of view (Hor+). The HUD stays in the centred 16:9 area and videos are
pillarboxed. 2160p on an ultrawide display is reduced to 1440p (the game's memory budget).
Displays narrower than 16:9 (e.g. 16:10) keep a 16:9 picture with black bars.
Tested in scripted races and screenshots, not yet on a real ultrawide monitor.

**60 FPS with an unmodified 1.28 executable:** the game no longer runs too fast; the fix is applied
in memory and leaves already-modified executables alone.

**Data:** settings, shader cache, logs and default saves are kept in the `user` folder next to the
executable. A custom save-data root can be selected in the launcher. Keep that folder when you update to a new release. The log of the last run is
`user\log\shad_log.txt`.

**Controls:** gamepad through SDL, the same as shadPS4/GR2fork.

## What it changes

Compared with GR2fork, this fork adds a Driveclub launcher with settings, 30/60/60 + FG/120 FPS modes,
FXAA/SMAA and anisotropic filtering, stability fixes, and CPU-side speedups of the emulated GPU
command processor. Full list: [driveclub-pc/CHANGES.md](driveclub-pc/CHANGES.md).

## Performance

Test machine: ASUS ROG Zephyrus G16 (GU605CR), Intel Core Ultra 9 285H, NVIDIA GeForce RTX 5070 Ti
Laptop GPU, 32 GB RAM, Windows 11. Same race, same game settings.

| | GR2fork (shadPS4) | Driveclub-PC |
|---|---|---|
| 60 FPS target, average | ~51 FPS | ~59.6 FPS |
| 60 FPS target, frame pacing | 12–19 % of frames drop to 33 ms (30 FPS) | stable 60 |
| 120 FPS target (played by hand) | dips to ~35 FPS | 75–90 FPS |
| 120 FPS target, 2560x1440 render (v0.2, played by hand) | – | 65–80 FPS |
| 120 FPS target, ultrawide (v0.2.1, scripted races) | – | 2560x1080: 84, 3440x1440: 70, 5120x1440: 67 FPS |

The 60 FPS rows come from scripted test races; the 120 FPS row comes from races played by hand.
Results vary by up to ~15 % with laptop temperature. At 120 FPS the limit is the emulator's
CPU-side GPU command processing, not the graphics card (27–35 % busy).

## Building (Windows)

Requires Visual Studio 2022 Build Tools with **clang-cl**, CMake, Ninja and Git. Run from an
"x64 Native Tools" developer prompt:

```powershell
git clone --recursive https://github.com/ondrejeder/Driveclub-pc-wrapper.git
cd Driveclub-pc-wrapper
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON
cmake --build build --parallel
```

The 0.2.5 release uses Release + ThinLTO with a fresh PGO profile. To reproduce the packaged build, use the corresponding source ZIP attached to the release; it includes the profile, dependency inventory and build script. The commands above build from source without the release PGO profile.

## Credits

- [shadPS4](https://github.com/shadps4-emu/shadPS4): the shadPS4 Emulator Project and contributors.
  Almost all of the code here is theirs.
- [GR2fork](https://github.com/junminlee2004/GR2fork) by junminlee2004: the fork this project starts
  from (see [README-GR2fork.md](README-GR2fork.md)).
- [Driveclub_Widescreen](https://github.com/rafidwayne/Driveclub_Widescreen) by rafidwayne
  (GPL-2.0-or-later): the aspect ratio, video-out width and FOV values the ultrawide support starts from.
- [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al., MIT licence
  ([driveclub-pc/SMAA-LICENSE.txt](driveclub-pc/SMAA-LICENSE.txt)).
- AMD FidelityFX FSR 1, already part of shadPS4, and optional native Vulkan Optical Flow / Frame Interpolation adapted from the local Killzone port; provenance and license notices are in [externals/fsr-vulkan](externals/fsr-vulkan).
- The libraries in `externals/` (git submodules), each under its own licence.

## Licence

**GPL-2.0-or-later** ([LICENSE](LICENSE)), the same as shadPS4 and GR2fork. Some files carry
other compatible licences in their SPDX headers; the texts are in [LICENSES/](LICENSES/).
