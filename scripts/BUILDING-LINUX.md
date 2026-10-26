# Building and running Driveclub-PC on Linux (experimental)

This is a community guide for building Driveclub-PC natively on Linux. It was written after a
successful build and run on **Bazzite (immutable Fedora)** using a **Fedora distrobox**, with an
**AMD RX 6750 XT** (Mesa RADV 26.0, Vulkan 1.4) under **Wayland**. Other distributions, GPUs and
X11 are untested, so expect to adapt some steps.

Driveclub-PC is a fork of shadPS4, which already supports Linux. The Linux-specific changes in
this branch are small:

- `src/driveclub/guest_stats.cpp`, `src/driveclub/vk_call_stats.cpp`: use `<x86intrin.h>` when not
  built with MSVC (they only need `__rdtsc`).
- `src/driveclub/launcher.cpp`: a settings window for non-Windows builds (SDL3 + Dear ImGui) that
  mirrors the Windows window, and the real display size read through SDL instead of a fixed
  1920x1080.

## You need

- Your own copy of Driveclub (`CUSA00093` is the tested release; the launcher also recognises other regions and shows a note about each; base game) with the 1.28 update next to it as
  `CUSA00093-patch` or `CUSA00093-UPDATE`, as described in the main README.
- The PS4 firmware modules (`*.sprx`, including `libSceLibcInternal.sprx`) from your own console.

**Do not share game files or firmware modules.** They are not part of this project and must not be
posted in issues, releases or forks.

## Build (Fedora container example)

```bash
distrobox create -n build -i fedora:latest
distrobox enter build

sudo dnf install -y clang cmake ninja-build git gcc-c++ libatomic pkgconf-pkg-config \
  vulkan-devel alsa-lib-devel pulseaudio-libs-devel openal-soft-devel \
  openssl-devel libevdev-devel systemd-devel libusb1-devel libuuid-devel \
  libX11-devel libXext-devel libXrandr-devel libXi-devel libXcursor-devel libXfixes-devel \
  libXScrnSaver-devel libXtst-devel libXinerama-devel libXxf86vm-devel libXrender-devel \
  libXcomposite-devel libXdamage-devel libxkbcommon-devel wayland-devel \
  wayland-protocols-devel libdecor-devel mesa-libGL-devel mesa-libEGL-devel mesa-libgbm-devel \
  libdrm-devel libsamplerate-devel pipewire-devel jack-audio-connection-kit-devel \
  dbus-devel ibus-devel libglvnd-devel

git clone --recursive https://github.com/ondrejeder/Driveclub-pc-wrapper.git
cd Driveclub-pc-wrapper
cmake --preset x64-Clang-Release
cmake --build Build/x64-Clang-Release --parallel
```

This is the set of packages that worked in our test. Some of them may not be strictly required.
The `--recursive` clone matters: a source zip does not contain the `externals/` submodules.
Inside the container, check that Vulkan sees your GPU with `vulkaninfo --summary`
(package `vulkan-tools`); NVIDIA users may need `distrobox create --nvidia`.

## Run

Start the program from inside the build folder, so relative paths behave the same way:

```bash
cd Build/x64-Clang-Release
DCPC_LAUNCHER=1 ./Driveclub-PC
```

The settings window lets you pick the game folder and an optional save folder, import the firmware
modules and choose the same options as the Windows window (including the four frame rate modes,
with and without frame generation). Press Play to start. The choices are saved in
`driveclub_pc.ini`; with "Show this window at start" unticked, the window is skipped on later runs
unless `DCPC_LAUNCHER=1` is set. You can also start the game directly with
`./Driveclub-PC -g /path/to/CUSA00093`.

The data folder (settings, firmware modules, logs, saves) is shadPS4's default,
`~/.local/share/shadPS4`, unless a `./user` folder exists in the working directory, in which case
that one is used instead. On Bazzite, `~` is `/var/home/<user>`, which is the same place.

| What | Where (inside the data folder) |
| --- | --- |
| Settings of this launcher | `driveclub_pc.ini` |
| Per-game emulator settings (written by the launcher) | `custom_configs/CUSA00093.json` |
| Logs | `log/shad_log.txt` and `log/shadps4.log`; a per-game `log/CUSA00093.log` appears when separate logs are enabled in the emulator settings |
| Save data (`PROFILE`, `GHOSTS`, `REPLAYS`) | `home/1000/savedata/CUSA00093/` |
| Firmware modules | `sys_modules/` |

To bring saves from another shadPS4 install, copy its `CUSA00093` save folder to the path above.

The log is full of lines such as `sceKernelOpen: error = 2` for files under `/app0`. They are the
game probing for optional files and are harmless.

## Arch-based systems (no container needed)

A tester built it directly on CachyOS (Arch) with the packages `cmake`, `clang` and `ninja` plus the
usual compiler tools; everything else comes from the bundled `externals/` folder:

```bash
sudo pacman -S --needed base-devel cmake clang ninja git
git clone --recursive https://github.com/ondrejeder/Driveclub-pc-wrapper.git
cd Driveclub-pc-wrapper
cmake --preset x64-Clang-Release
cmake --build Build/x64-Clang-Release --parallel
```

The build has about 2,600 targets and the program is roughly 94 MB. The development libraries for
audio and windowing (ALSA, PulseAudio/PipeWire, X11/Wayland) must be installed, as in the Fedora
list above. This was reported by a tester and has not been reproduced by the author of this guide.

## Older distributions and SteamOS (glibc)

A program built on a recent distribution only runs on systems whose glibc is at least as new as
the one it was built with. A build made on Fedora 44 (glibc 2.43) fails on SteamOS 3.5 (glibc 2.37)
with a "version `GLIBC_x.y' not found" error. The C++23 mode used by this project makes this
worse: with a recent glibc, calls such as `strtol` are linked to versions added in glibc 2.38.

To produce a build that runs on SteamOS and most other distributions, compile on an **old** base.
`scripts/build-appimage-compat.sh` does this with a Ubuntu 22.04 (glibc 2.35) distrobox container:

```bash
bash scripts/build-appimage-compat.sh all
```

What it does: installs clang 20, the libstdc++ of GCC 14 (needed for `std::ranges::to`, linked
statically so the target system does not need it) and a recent CMake in a private Python
environment; builds the project; verifies with `objdump` that no file in the result needs a glibc
newer than 2.37; packs the AppImage; and starts it in a clean Debian 12 container as a smoke test.
You can run the stages separately: `deps`, `build`, `appimage`, `test`.

Reports so far: the AppImage built this way runs on SteamOS, in desktop and game mode (a desktop
PC with a Ryzen 5 5600X and an RX 6750 XT running SteamOS, and a Steam Deck). On the Deck, one tester
reports about 20 FPS regardless of the graphics settings, which points to a CPU-bound case; the
same tester saw about the same speed with the Windows build under Proton.

The result still needs a CPU with AVX2 (the project builds for x86-64-v3) and a Vulkan driver
installed on the system. It has not been tested on a real Steam Deck by the author of this guide.

## NVIDIA GPUs

The renderer talks to the GPU through Vulkan and is not tied to one vendor. The code inherited from
shadPS4 already contains workarounds that switch on when the Vulkan driver reports the NVIDIA
proprietary driver (for example, emulating clip-distance interpolation and extra LDS barriers), and
that detection is the same on Windows and Linux. Nothing NVIDIA-specific is needed at build time;
the build is identical.

What differs on Linux is making the driver visible to the build container:

```bash
distrobox create --nvidia -n build -i fedora:latest
distrobox enter build
vulkaninfo --summary        # your NVIDIA GPU must be listed here
```

The proprietary driver has to be installed on the host (on Bazzite, use an image variant that
ships it). If `vulkaninfo` does not list your GPU, the game will not use it, so fix that first. On
laptops with two GPUs, check the `GPU_Model` line in `~/.local/share/shadPS4/log/shad_log.txt` to
see which one was picked; the emulator logs its `Vulkan gpuId` setting (`-1` means automatic).

The author of this guide only has an AMD GPU and has not tested NVIDIA. One tester reports it
working on a laptop GTX 1660 Ti (proprietary driver 580.178.04, CachyOS, KDE, Wayland) at about 37
FPS on average with the 60 FPS target, limited by the CPU. If you try it on NVIDIA, please report
the result (driver version, GPU model and what worked or did not) in an issue so this section can
be updated.

## Other releases of the game

Since version 0.2.5 the launcher itself recognises regional releases of Driveclub. When you choose a
game folder it shows a compatibility note for the release it finds, and the **Details** button next
to the firmware shows what is missing or invalid for that region. Settings are kept per title id
(`custom_configs/<title id>.json`). The game version is only a warning: anything other than 1.28 is
accepted, with a line in the log.

The 60 FPS unlock, the widescreen handling and the render resolution change the game's executable
at fixed addresses, and each one first checks that the bytes there are the expected ones. On a
release whose executable differs they are skipped, not forced, and the game then runs like plain
shadPS4. To see which ones were applied, look in the log:

```bash
grep "driveclub-pc:" ~/.local/share/shadPS4/log/shad_log.txt
```

Lines saying "skipped" or "do not match" mean that the feature is not active for that executable.

## Game patches (memory patches)

The emulator part of this build loads shadPS4 memory patches for Driveclub. Loading is on by
default and the launcher has a **Game patches** panel on the right of the settings, modelled on the
Patches tab of shadPS4:

- *Patch file*: the patch files found for the game, shown as `file | folder`.
- The list below it has one checkbox per patch (hover for the author's note). **Save** writes the
  choices into the XML file (the `isEnabled` attribute); unsaved choices are saved when you press
  Play. **Revert** drops them, **Delete file** removes the file (after a second click) and its entry
  in `files.json`, **Rescan** reads the folder again.
- Patches marked `Always="true"` in the file are shown greyed out. This emulator only applies
  patches with `isEnabled="true"`, so those are not applied (the only such patch in the set we
  looked at, the red zone fix, is already built into this program).

Where the files live: `~/.local/share/shadPS4/patches/<any-folder-name>/`, each folder with a
`files.json` that says which game each XML file is for, for example
`{ "Driveclub.xml": ["CUSA00093"] }`. The XML must match the game (`AppVer="01.28"` and
`AppElf="eboot.bin"`). One file can also be loaded explicitly with
`./Driveclub-PC -g /path/to/CUSA00093 -p /path/to/patch.xml`.

**Patches this build already does natively.** The launcher has its own 60 FPS unlock, render
resolution and widescreen handling. Community patches for the same things patch the same places,
and the 60 FPS ones may make the game run in slow motion because the game logic tick rate stays
fixed. Use them only if you know why. The panel says so. Patches for other things, such as
disabling motion blur or the 720p internal resolution, are what this feature is for. The 720p
internal resolution patch has been tested in this build and works; the game just renders at a
lower resolution, which is then scaled to the window.

### Putting patches inside the AppImage

Put the patch folders in `bundled-patches/` in the source tree (or set `PATCHES_DIR` to another
folder when running `build-appimage-compat.sh`). The script copies them into the AppImage, and the
first time the launcher runs it copies them to the user's patches folder. Existing files are never
overwritten (the user may have switched patches on or off), and a file that the user deleted does
not come back.

`prepare-bundled-patches.py` makes such a folder from patch XML files:

```bash
python3 prepare-bundled-patches.py bundled-patches shadPS4 ~/.local/share/shadPS4/patches/shadPS4/Driveclub.xml
python3 prepare-bundled-patches.py bundled-patches driveclub-720p Driveclub-720p-internal.xml
```

It switches every patch off (so nothing changes until the user turns it on), writes `files.json`
from the `<TitleID>` block of each file, warns about patches that overlap with built-in features
and about line types the emulator does not apply, and writes `CREDITS.txt` from the `Author`
fields. **The patches are other people's work**: check that you may redistribute them and keep
`CREDITS.txt` with them. The set published with the driveclub-recomp project, for example, names
Kravickas, TonySSS, ZipperedRain, kalaposfos, squidbus, illusion, Ianespaa and u/Charlie_Muggins.

## Known limitations

- The Browse and Import buttons use the system folder dialog (desktop portal or zenity). If no
  dialog appears, type the path into the text field instead.
- The settings window is not HiDPI-aware and can look blurry on scaled displays.
- The window is wide rather than tall (settings on the left, patches on the right, up to 1240 pixels
  wide) and is sized to fit the screen, so it fits a Steam Deck (1280x800). The Play and Quit buttons
  stay at the bottom while each side scrolls. A controller can navigate the
  window; this has not been tested on real hardware.
- The launcher's own render resolution option does not go below 1080p: an experimental 720p
  option was tried and the game crashed with it, so it was removed. The community 720p internal
  resolution patch works instead (see Game patches); it changes two data values rather than the
  game's code.
- Motion blur does not appear. The log shows a very frequent
  `ResolveDepthOverlap: Unimplemented depth overlap copy` warning (the emulator does not implement
  that depth copy), which is the likely cause; a tester reports the same on another machine. The
  community patches for plain shadPS4 also disable motion blur for this kind of reason.
- Only the Windows build is tested by the original author. The Linux build was not tested beyond
  the hardware listed above.

## Credits

Driveclub-PC and its README are the work of the original author (ondrejeder) and contributors;
it builds on shadPS4, GR2fork and the Driveclub_Widescreen project. This Linux guide and the
small Linux changes are shared under the same license as the project (GPL-2.0-or-later).
