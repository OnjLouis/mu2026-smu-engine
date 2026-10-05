#!/usr/bin/env bash
# Stage a macOS release tree from a built BUILD dir (UNIVERSAL=1 build).
# Usage: package-macos.sh <build-dir> <dist-dir>
# AUv3 (.app/.appex) is intentionally excluded: it only sings with ROMs baked
# in, which are never redistributed (see doc/release-builds.md).
set -euo pipefail

BUILD="${1:?usage: $0 <build-dir> <dist-dir>}"
DIST="${2:?usage: $0 <build-dir> <dist-dir>}"

BINARIES="verify boot render panel statetest live gui blocktime vst3probe aubprobe clapprobe"

mkdir -p "$DIST/bin" "$DIST/plugins"

for b in $BINARIES; do
  if [ -f "$BUILD/$b" ]; then
    cp -f "$BUILD/$b" "$DIST/bin/"
  else
    echo "warning: missing $BUILD/$b" >&2
  fi
done

for p in "$BUILD/S-MU2000.vst3" "$BUILD/S-MU2000.clap" "$BUILD/S-MU2000.component"; do
  if [ -e "$p" ]; then
    cp -rf "$p" "$DIST/plugins/"
  else
    echo "warning: missing $p" >&2
  fi
done

# Ad-hoc sign so first launch is a right-click-Open, not a hard block.
# Unsigned bundles get killed by Gatekeeper on recent macOS; ad-hoc (-s -)
# is enough for a continuous build (no notarization).
if command -v codesign >/dev/null 2>&1; then
  for p in "$DIST/plugins"/S-MU2000.vst3 "$DIST/plugins"/S-MU2000.clap "$DIST/plugins"/S-MU2000.component; do
    [ -e "$p" ] || continue
    codesign --force --deep --sign - "$p" || echo "warning: codesign failed for $p" >&2
  done
  for b in "$DIST"/bin/*; do
    [ -f "$b" ] || continue
    codesign --force --sign - "$b" || echo "warning: codesign failed for $b" >&2
  done
fi

# The photo-style panel art: the gui finds art/real one level above bin/
# (layout.cpp, find_default). Without it the standalone gui falls back to
# the plain built-in panel.
mkdir -p "$DIST/art"
cp -rf art/real "$DIST/art/"

cp -f LICENSE "$DIST/LICENSE.txt"
cp -f NOTICE.txt "$DIST/NOTICE.txt"
[ -f doc/vst3-readme.txt ] && cp -f doc/vst3-readme.txt "$DIST/plugins/vst3-readme.txt" || true

printf '# Put the path to your ROM folder on the first line, e.g.\n# /Users/you/roms\n' > "$DIST/roms.txt.example"

# The gui and the plug-ins are the point of a release: stop rather than
# publish a zip without them (a wrong build dir once shipped only verify,
# render and statetest)
for need in "$DIST/bin/gui" "$DIST/plugins/S-MU2000.vst3"; do
  [ -e "$need" ] || { echo "error: $need is missing" >&2; exit 1; }
done

echo "staged macos dist in $DIST:"
ls "$DIST" "$DIST/bin" "$DIST/plugins"
