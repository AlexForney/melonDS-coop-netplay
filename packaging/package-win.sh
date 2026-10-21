#!/bin/bash
# Builds a standalone Windows folder of the fork (exe + every DLL it needs + Qt plugins).
# Run in the MSYS2 UCRT64 shell from the repo root: bash packaging/package-win.sh [outdir]
set -e
OUT=${1:-/d/melonDS-coop-dist/melonDS-coop-netplay}
rm -rf "$OUT"; mkdir -p "$OUT"
cp build/melonDS.exe "$OUT/"
# no Qt Multimedia backend: its only one here is ffmpeg (~150 MB), and melonDS needs it only
# for the DSi camera
windeployqt6 --release --no-translations --no-opengl-sw --no-system-d3d-compiler \
  --no-compiler-runtime --skip-plugin-types multimedia "$OUT/melonDS.exe" > /dev/null
# every non-system DLL of the exe and the Qt plugins, from the UCRT64 prefix
for i in 1 2 3; do
  find "$OUT" -name '*.exe' -o -name '*.dll' | while read -r f; do
    ldd "$f" | awk '{print $3}' | grep -i '^/ucrt64/' || true
  done | sort -u | while read -r d; do
    [ -f "$OUT/$(basename "$d")" ] || cp "$d" "$OUT/"
  done
done
cp LICENSE "$OUT/LICENSE.txt"
cp packaging/README-netplay.txt "$OUT/README.txt"
echo "packaged into $OUT"; du -sh "$OUT"
