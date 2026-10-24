#!/usr/bin/env bash
# Prepares a CARLA_ROOT for Windows builds: the official Carla win64 release supplies the
# libraries and bridges, the matching source tag supplies the headers.
#
#   scripts/fetch-carla-windows.sh <output-dir> [version]
#
# Afterwards configure with -DCARLA_ROOT=<output-dir>.
set -euo pipefail

out="${1:?usage: $0 <output-dir> [version]}"
version="${2:-2.5.9}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

curl -fsSL -o "$work/carla.zip" "https://github.com/falkTX/Carla/releases/download/v${version}/Carla-${version}-win64.zip"
git -c advice.detachedHead=false clone -q --depth 1 --branch "v${version}" --filter=blob:none --sparse https://github.com/falkTX/Carla "$work/src"
git -C "$work/src" sparse-checkout set source/backend source/includes source/utils

mkdir -p "$out/include/carla/includes" "$out/include/carla/utils" "$out/lib/carla"

# libraries and helper executables only, not Carla's own Python/Qt5 GUI
unzip -q -j -o "$work/carla.zip" \
    "Carla-${version}-win64/Carla/libcarla_standalone2.dll" \
    "Carla-${version}-win64/Carla/libcarla_utils.dll" \
    "Carla-${version}-win64/Carla/carla-bridge-*.exe" \
    "Carla-${version}-win64/Carla/carla-discovery-*.exe" \
    -d "$out/lib/carla"

src="$work/src/source"
cp "$src"/backend/CarlaBackend.h "$src"/backend/CarlaHost.h "$src"/backend/CarlaUtils.h "$out/include/carla/"
cp "$src"/includes/*.h "$out/include/carla/includes/"
cp "$src"/utils/*.hpp "$out/include/carla/utils/"

echo "Carla ${version} ready in ${out}"
ls "$out/lib/carla"
