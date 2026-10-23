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

- Your own copy of Driveclub (`CUSA00093`, base game) with the 1.28 update next to it as
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

The settings window lets you pick the game folder, import the firmware modules and choose the
same options as the Windows window. Press Play to start. The choices are saved in
`driveclub_pc.ini`; with "Show this window at start" unticked, the window is skipped on later runs
unless `DCPC_LAUNCHER=1` is set. You can also start the game directly with
`./Driveclub-PC -g /path/to/CUSA00093`.

The data folder (settings, firmware modules, logs) is shadPS4's default,
`~/.local/share/shadPS4`, unless a `./user` folder exists in the working directory, in which case
that one is used instead. In our test everything lived in `~/.local/share/shadPS4`: the settings
file `driveclub_pc.ini` and the logs in `log/` (the game log is `CUSA00093.log`).

## NVIDIA GPUs (untested)

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
laptops with two GPUs, check the `GPU_Model` line in `~/.local/share/shadPS4/log/CUSA00093.log` to
see which one was picked; the emulator logs its `Vulkan gpuId` setting (`-1` means automatic).

This path has **not been tested** by the author of this guide, who only has an AMD GPU. If you
try it on NVIDIA, please report the result (driver version, GPU model and what worked or did not)
in an issue so this section can be updated.

## Known limitations

- The Browse and Import buttons use the system folder dialog (desktop portal or zenity). If no
  dialog appears, type the path into the text field instead.
- The settings window is not HiDPI-aware and can look blurry on scaled displays.
- Motion blur did not appear in our test. The log shows a very frequent
  `ResolveDepthOverlap: Unimplemented depth overlap copy` warning, which may or may not be
  related; we have not compared against the Windows build.
- Only the Windows build is tested by the original author. The Linux build was not tested beyond
  the hardware listed above.

## Credits

Driveclub-PC and its README are the work of the original author (ondrejeder) and contributors;
it builds on shadPS4, GR2fork and the Driveclub_Widescreen project. This Linux guide and the
small Linux changes are shared under the same license as the project (GPL-2.0-or-later).
