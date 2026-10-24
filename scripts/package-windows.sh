#!/usr/bin/env bash
# Collects chost.exe, chost-cli.exe, Qt, the MinGW runtime and Carla into one folder.
# Run from an MSYS2 MINGW64 shell after building.
#
#   scripts/package-windows.sh <build-dir> <carla-root> <output-dir>
set -euo pipefail

build="${1:?usage: $0 <build-dir> <carla-root> <output-dir>}"
carla="${2:?}"
out="${3:?}"

rm -rf "$out"
mkdir -p "$out"
cp "$build/chost.exe" "$build/chost-cli.exe" "$out/"
cp "$carla"/lib/carla/* "$out/"

deployqt="$(command -v windeployqt6 || command -v windeployqt-qt6 || command -v windeployqt)"
"$deployqt" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "$out/chost.exe"

# windeployqt only handles Qt itself; copy every other MinGW DLL that anything in the folder needs
for _ in 1 2 3; do
    find "$out" -name '*.exe' -o -name '*.dll' | while read -r f; do
        ldd "$f" 2>/dev/null | awk '{print $3}' | grep -i '/mingw64/' || true
    done | sort -u | while read -r dll; do
        [ -f "$out/$(basename "$dll")" ] || cp "$dll" "$out/"
    done
done

cp README.md "$out/README.md"
echo "packaged into $out"
