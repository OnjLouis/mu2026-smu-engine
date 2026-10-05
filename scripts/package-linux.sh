#!/usr/bin/env bash
# Stage a Linux release tree + AppDir from a built BUILD dir.
# Usage: package-linux.sh <build-dir> <dist-dir> [appdir]
# - <dist> holds bin/ + plugins/ for the tarball (see doc/release-builds.md).
# - [appdir] (default <dist>.AppDir) holds the gui AppImage staging.
# Building the .AppImage itself needs appimagetool (downloaded by CI);
# this script only stages the AppDir and copies shared-library deps via ldd,
# so it is testable without root or FUSE.
set -euo pipefail

BUILD="${1:?usage: $0 <build-dir> <dist-dir> [appdir]}"
DIST="${2:?usage: $0 <build-dir> <dist-dir> [appdir]}"
APPDIR="${3:-$DIST.AppDir}"

BINARIES="verify boot render panel statetest blocktime live gui vst3probe clapprobe"

mkdir -p "$DIST/bin" "$DIST/plugins"

for b in $BINARIES; do
  if [ -f "$BUILD/$b" ]; then
    cp -f "$BUILD/$b" "$DIST/bin/"
  else
    echo "warning: missing $BUILD/$b" >&2
  fi
done

for p in "$BUILD/S-MU2000.vst3" "$BUILD/S-MU2000.clap"; do
  if [ -e "$p" ]; then
    cp -rf "$p" "$DIST/plugins/"
  else
    echo "warning: missing $p" >&2
  fi
done

# The photo-style panel art: the gui finds art/real one level above bin/
# (layout.cpp, find_default). Without it the standalone gui falls back to
# the plain built-in panel.
mkdir -p "$DIST/art"
cp -rf art/real "$DIST/art/"

cp -f LICENSE "$DIST/LICENSE.txt"
cp -f NOTICE.txt "$DIST/NOTICE.txt"
[ -f doc/vst3-readme.txt ] && cp -f doc/vst3-readme.txt "$DIST/plugins/vst3-readme.txt" || true

printf '# Put the path to your ROM folder on the first line, e.g.\n# /home/you/roms\n' > "$DIST/roms.txt.example"

# ---- AppDir (gui entry) ----
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

mkdir -p "$APPDIR/usr/art" && cp -rf art/real "$APPDIR/usr/art/"   # usr/bin/../art/real
cp -f "$DIST/bin/gui" "$APPDIR/usr/bin/S-MU2000-gui" 2>/dev/null || echo "warning: no gui for AppDir" >&2
# CLI companions ride along inside the AppImage (run via `S-MU2000*.AppImage --appimage-mount` or the tarball).
for b in render live panel verify; do
  [ -f "$DIST/bin/$b" ] && cp -f "$DIST/bin/$b" "$APPDIR/usr/bin/" || true
done

# Icon: prefer panel art, fall back to screenshot.
if [ -f art/real/panel.png ]; then
  cp -f art/real/panel.png "$APPDIR/S-MU2000.png"
  cp -f art/real/panel.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/S-MU2000.png"
elif [ -f doc/mu_screenshot.png ]; then
  cp -f doc/mu_screenshot.png "$APPDIR/S-MU2000.png"
  cp -f doc/mu_screenshot.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/S-MU2000.png"
fi
ln -sf usr/share/icons/hicolor/256x256/apps/S-MU2000.png "$APPDIR/.DirIcon" 2>/dev/null || true

cat > "$APPDIR/S-MU2000.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=S-MU2000
Comment=Yamaha MU2000 software tone generator (needs own ROMs)
Exec=S-MU2000-gui
Icon=S-MU2000
Categories=AudioVideo;Audio;
Terminal=false
EOF
cp -f "$APPDIR/S-MU2000.desktop" "$APPDIR/usr/share/applications/"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:${LD_LIBRARY_PATH:-}"
exec "$HERE/usr/bin/S-MU2000-gui" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# Bundle shared deps of the staged binaries (SDL3, cairo, ALSA, fontconfig chain).
# Excludes loader/libc/system libs that must come from the host.
# Patterns match the full path tail (distros use /lib, /lib64 or /usr/lib).
if [ -f "$APPDIR/usr/bin/S-MU2000-gui" ]; then
  ldd "$APPDIR/usr/bin/S-MU2000-gui" 2>/dev/null | awk '{if ($3 ~ /^\//) print $3}' | sort -u | while read -r lib; do
    case "${lib##*/}" in
      ld-linux*|libc.so*|libm.so*|libpthread*|libdl.so*|librt.so*|\
      libresolv*|libnss*|libthread_db*|\
      libX*|libxcb*|libXau*|libXdmcp*|\
      libGL*|libEGL*|libGLX*|libGLdispatch*|libvulkan*|libdrm*|libgbm*|\
      libwayland*|libxkbcommon*)
        ;;
      *) cp -fL "$lib" "$APPDIR/usr/lib/" 2>/dev/null || echo "warning: cannot bundle $lib" >&2 ;;
    esac
  done || true
fi

echo "staged linux dist in $DIST and AppDir in $APPDIR:"
ls "$DIST" "$DIST/bin" "$DIST/plugins"
ls "$APPDIR" "$APPDIR/usr/bin" | head -30
