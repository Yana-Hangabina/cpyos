#!/usr/bin/env bash
# Build the 2048 native plugin for the Metalio E-INK4 Plus (ESP32-S31).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"

idf_path="${CPYOS_IDF_PATH:-/Users/hongbin/Documents/projects/cpyos/.scratch/esp-idf-6.1}"
if [[ ! -f "$idf_path/export.sh" ]]; then
  echo "ESP-IDF 6.1 not found at $idf_path (set CPYOS_IDF_PATH)" >&2
  exit 1
fi
# shellcheck disable=SC1090
. "$idf_path/export.sh" >/dev/null

cd "$here"
if [[ ! -f sdkconfig ]]; then
  idf.py --preview -B build set-target esp32s31
fi
idf.py --preview -B build build elf

mkdir -p dist
cp build/plugin2048.app.elf dist/plugin.elf
cp manifest.json dist/manifest.json
ls -l dist/plugin.elf dist/manifest.json
shasum -a 256 dist/plugin.elf
