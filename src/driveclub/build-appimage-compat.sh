#!/usr/bin/env bash
# build-appimage-compat.sh
#
# Builds Driveclub-PC for Linux inside an OLD Ubuntu (glibc 2.35) and packs an AppImage that also
# runs on SteamOS (glibc 2.37) and most other distributions. A program built on a new distribution
# (for example Fedora 44, glibc 2.43) refuses to start on an older one, so the build must happen on
# an old base.
#
# Run it on the host (NOT inside a container). Needs: distrobox, git, curl.
#
#   bash build-appimage-compat.sh deps       install the toolchain in an Ubuntu 22.04 container
#   bash build-appimage-compat.sh build      clone, configure and compile
#   bash build-appimage-compat.sh appimage   check glibc, pack the AppImage
#   bash build-appimage-compat.sh test       start it in a clean Debian 12 container (smoke test)
#   bash build-appimage-compat.sh all        everything above, in order (default)
#   DRY=1 bash build-appimage-compat.sh all  only print what would be done
#
# Settings can be overridden with environment variables (see the block below).
# Patches inside the AppImage: put patch folders (each with a files.json and its XML files) in
# bundled-patches/ of the source tree, or point PATCHES_DIR at another folder. They are copied to
# the user's patches folder the first time the launcher runs.
set -euo pipefail

BOX=${BOX:-dcpc-compat}
IMAGE=${IMAGE:-ubuntu:22.04}
TEST_BOX=${TEST_BOX:-dcpc-compat-test}
TEST_IMAGE=${TEST_IMAGE:-debian:12}
REPO=${REPO:-https://github.com/DionSantosGG/Driveclub-pc-wrapper-linux.git}
BRANCH=${BRANCH:-linux-support}
WORK=${WORK:-$HOME/dcpc-compat}
NAME=${NAME:-Driveclub-PC}
LLVM=${LLVM:-20}
GCCV=${GCCV:-14}
MAX_GLIBC=${MAX_GLIBC:-2.37}        # the build fails if the program needs a newer glibc than this
DRY=${DRY:-0}
STAGE=${1:-all}

SRC="$WORK/src"
PATCHES_DIR=${PATCHES_DIR:-$SRC/bundled-patches}
TOOLS="$WORK/tools"
PACK="$WORK/appimage"
LOG="$WORK/build.log"
BUILD_DIR="$SRC/Build/x64-Clang-Release"
OUT="$NAME-linux-x86_64.AppImage"
GCCDIR="/usr/lib/gcc/x86_64-linux-gnu/$GCCV"

LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage

say() { printf '\n==> %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

if [ "$DRY" != 1 ]; then
  command -v distrobox >/dev/null || die "distrobox not found. Run this on the Bazzite host."
  [ ! -e /run/.containerenv ] || die "Run this on the host, not inside a container."
  command -v curl >/dev/null || die "curl not found."
fi
mkdir -p "$WORK" "$TOOLS" "$PACK"

# ---- helpers -------------------------------------------------------------------------------------
box_exists() {
  distrobox list --no-color 2>/dev/null | awk -F'|' 'NR>1 {gsub(/ /,"",$2); print $2}' | grep -qx "$1"
}

ensure_box() {   # name image
  if [ "$DRY" = 1 ]; then echo "[dry-run] ensure container $1 ($2)"; return; fi
  if ! box_exists "$1"; then
    say "Creating container $1 from $2 (the first run downloads the image)"
    distrobox create --yes --name "$1" --image "$2"
  fi
}

inbox() {        # run a bash snippet inside the build container
  if [ "$DRY" = 1 ]; then echo "[dry-run] in $BOX: $1" | head -c 600; echo; return; fi
  distrobox enter "$BOX" -- bash -lc "$1"
}

# Prints the highest GLIBC_x.y symbol needed by the given files/folders and fails when it is newer
# than the limit. Written to a file so it can run inside the container without quoting problems.
write_glibc_checker() {
  cat > "$WORK/check_glibc.sh" <<'EOF'
#!/usr/bin/env bash
# usage: check_glibc.sh LIMIT PATH...
limit=$1; shift
max=0.0; maxfile=none
newer() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ] && [ "$1" != "$2" ]; }
while IFS= read -r f; do
  v=$(objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -Vu | tail -1)
  [ -n "$v" ] || continue
  if newer "$v" "$max"; then max=$v; maxfile=$f; fi
done < <(find "$@" -type f \( -perm -u+x -o -name '*.so*' \) 2>/dev/null)
echo "Highest glibc symbol required: $max  (in $maxfile)"
echo "Libraries that use libstdc++ dynamically: $(for f in $(find "$@" -type f -perm -u+x 2>/dev/null); do objdump -T "$f" 2>/dev/null | grep -l GLIBCXX >/dev/null && echo "$f"; done | head -3 | tr '\n' ' ')"
if newer "$max" "$limit"; then
  echo "TOO NEW: needs glibc $max, but the limit is $limit (SteamOS has 2.37)."
  exit 3
fi
echo "OK: glibc $max is not newer than $limit."
EOF
  chmod +x "$WORK/check_glibc.sh"
}

check_glibc() { inbox "bash '$WORK/check_glibc.sh' '$MAX_GLIBC' $*"; }

# ---- stages --------------------------------------------------------------------------------------
stage_deps() {
  ensure_box "$BOX" "$IMAGE"
  say "Installing base tools"
  inbox "set -e
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends software-properties-common ca-certificates curl wget gnupg lsb-release git pkg-config python3-pip python3-venv file patchelf desktop-file-utils xz-utils binutils make"

  say "Installing GCC $GCCV (only for its libstdc++, linked statically) and LLVM $LLVM"
  inbox "set -e
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y gcc-$GCCV g++-$GCCV libstdc++-$GCCV-dev
curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh $LLVM
test -x /usr/bin/clang++-$LLVM"

  say "Installing development libraries (a missing optional one only prints a warning)"
  local pkgs="libasound2-dev libpulse-dev libpipewire-0.3-dev libjack-jackd2-dev libsamplerate0-dev \
libdbus-1-dev libibus-1.0-dev libudev-dev libevdev-dev libusb-1.0-0-dev uuid-dev libssl-dev \
libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev libxss-dev libxtst-dev \
libxinerama-dev libxxf86vm-dev libxrender-dev libxcomposite-dev libxdamage-dev libxkbcommon-dev \
libwayland-dev wayland-protocols libwayland-bin libdecor-0-dev libgl-dev libegl-dev libgbm-dev \
libdrm-dev libglvnd-dev libvulkan-dev"
  inbox "for p in $pkgs; do sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \$p || echo \"WARN: could not install \$p\"; done"

  say "Installing CMake and Ninja in a private Python environment ($WORK/venv)"
  inbox "set -e
python3 -m venv '$WORK/venv'
'$WORK/venv/bin/pip' install --upgrade pip 'cmake>=3.30,<4' ninja
'$WORK/venv/bin/cmake' --version | head -1"
}

stage_build() {
  ensure_box "$BOX" "$IMAGE"
  if [ "$DRY" = 1 ] || [ ! -d "$SRC/.git" ]; then
    say "Cloning $REPO (branch $BRANCH) with submodules"
    inbox "git clone --recursive --branch '$BRANCH' '$REPO' '$SRC'"
  else
    say "Source already in $SRC; updating branch $BRANCH and the submodules"
    inbox "git -C '$SRC' fetch origin '$BRANCH' && git -C '$SRC' checkout '$BRANCH' \
      && git -C '$SRC' pull --ff-only origin '$BRANCH' || echo 'WARN: could not update the branch; building what is already there'
git -C '$SRC' submodule update --init --recursive"
  fi

  say "Configuring (clang $LLVM, libstdc++ $GCCV linked statically)"
  inbox "set -e
export PATH='$WORK/venv/bin':\$PATH
cd '$SRC'
cmake --preset x64-Clang-Release --fresh \
  -DCMAKE_C_COMPILER=clang-$LLVM -DCMAKE_CXX_COMPILER=clang++-$LLVM \
  -DCMAKE_C_FLAGS=--gcc-install-dir=$GCCDIR -DCMAKE_CXX_FLAGS=--gcc-install-dir=$GCCDIR \
  -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc --gcc-install-dir=$GCCDIR' \
  -DCMAKE_SHARED_LINKER_FLAGS='-static-libstdc++ -static-libgcc --gcc-install-dir=$GCCDIR'" 2>&1 | tee -a "$LOG"

  say "Compiling (this takes a while; the log is $LOG)"
  inbox "set -e
export PATH='$WORK/venv/bin':\$PATH
cmake --build '$BUILD_DIR' --parallel" 2>&1 | tee -a "$LOG"

  write_glibc_checker
  say "Checking which glibc the program needs"
  check_glibc "'$BUILD_DIR/$NAME'"
}

stage_appimage() {
  ensure_box "$BOX" "$IMAGE"
  write_glibc_checker
  [ "$DRY" = 1 ] || [ -f "$BUILD_DIR/$NAME" ] || die "Program not found: $BUILD_DIR/$NAME. Run the build stage first."

  say "Checking the program once more"
  check_glibc "'$BUILD_DIR/$NAME'"

  if [ "$DRY" != 1 ]; then
    say "Downloading linuxdeploy and appimagetool (kept in $TOOLS)"
    [ -f "$TOOLS/linuxdeploy-x86_64.AppImage" ] || curl -fL -o "$TOOLS/linuxdeploy-x86_64.AppImage" "$LINUXDEPLOY_URL"
    [ -f "$TOOLS/appimagetool-x86_64.AppImage" ] || curl -fL -o "$TOOLS/appimagetool-x86_64.AppImage" "$APPIMAGETOOL_URL"
    chmod +x "$TOOLS"/*.AppImage
    cp "$SRC/src/resources/shadps4.png" "$PACK/driveclub-pc.png"
    cat > "$PACK/$NAME.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$NAME
Comment=Driveclub on PC (community Linux build)
Exec=$NAME
Icon=driveclub-pc
Terminal=false
Categories=Game;
EOF
  fi

  say "Building the AppDir (libvulkan stays out: the driver of the system is used)"
  inbox "set -e
export APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64
cd '$PACK'
rm -rf AppDir
'$TOOLS/linuxdeploy-x86_64.AppImage' --appdir AppDir --executable '$BUILD_DIR/$NAME' \
  --desktop-file '$NAME.desktop' --icon-file driveclub-pc.png --exclude-library 'libvulkan.so*'
mkdir -p AppDir/usr/share/doc/driveclub-pc
cp -r '$SRC/LICENSES' AppDir/usr/share/doc/driveclub-pc/ || true
if [ -d '$PATCHES_DIR' ]; then
  mkdir -p AppDir/usr/share/driveclub-pc/patches
  cp -r '$PATCHES_DIR'/. AppDir/usr/share/driveclub-pc/patches/
  echo 'Bundled patches from $PATCHES_DIR:'
  ls -R AppDir/usr/share/driveclub-pc/patches | head -40
else
  echo 'No patches folder at $PATCHES_DIR: the AppImage will not bundle patches.'
fi"

  say "Checking every file that goes into the AppImage"
  check_glibc "'$PACK/AppDir'"

  say "Packing"
  inbox "set -e
export APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64
cd '$PACK'
rm -f '$OUT'
'$TOOLS/appimagetool-x86_64.AppImage' AppDir '$OUT'
ls -lh '$OUT'
sha256sum '$OUT'"
  say "Done: $PACK/$OUT"
}

stage_test() {
  ensure_box "$TEST_BOX" "$TEST_IMAGE"
  say "Smoke test in a clean $TEST_IMAGE container (no development libraries installed)"
  if [ "$DRY" = 1 ]; then echo "[dry-run] run $PACK/$OUT --appimage-extract-and-run --help in $TEST_BOX"; return; fi
  [ -f "$PACK/$OUT" ] || die "AppImage not found: $PACK/$OUT. Run the appimage stage first."
  distrobox enter "$TEST_BOX" -- bash -lc "cd '$PACK' && ldd --version | head -1 && ./'$OUT' --appimage-extract-and-run --help | head -8" \
    && echo "OK: the program starts with the older glibc." \
    || die "The program did not start in the clean container (see the messages above)."
}

case "$STAGE" in
  deps)     stage_deps ;;
  build)    stage_build ;;
  appimage) stage_appimage ;;
  test)     stage_test ;;
  all)      stage_deps; stage_build; stage_appimage; stage_test ;;
  *)        die "Unknown stage '$STAGE'. Use: deps | build | appimage | test | all" ;;
esac
