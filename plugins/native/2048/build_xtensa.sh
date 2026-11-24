#!/bin/sh
# Build the 2048 native plugin for Read Pico (ESP32-S3, Xtensa).
#
# Uses the same PlatformIO Xtensa toolchain and the same relocation-free
# contract as Phase 0: -nostdlib -shared -e app_main, no libc imports, all
# services through cpyos_plugin_api.h.
set -eu

here="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
repo="$(CDPATH= cd -- "$here/../../.." && pwd)"
TC="${TC:-$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc}"
STRIP="${STRIP:-${TC%gcc}strip}"
OUT="${1:-$here/dist-xtensa/plugin.elf}"

mkdir -p "$(dirname "$OUT")"
"$TC" -std=gnu17 -Os \
  -fPIC -shared -nostdlib -nostartfiles -ffreestanding -fno-builtin \
  -fvisibility=hidden -fdata-sections -ffunction-sections \
  -Wl,--gc-sections -Wl,--strip-all \
  -mlongcalls -mno-text-section-literals -e app_main \
  -I "$repo/src/plugins" \
  -o "$OUT" "$here/main/plugin.c"

# Mirror the loader's project_elf() strip set; these sections are not part of
# the runtime image and confuse esp_elf on Xtensa.
"$STRIP" --strip-unneeded \
  --remove-section=.comment \
  --remove-section=.dynamic \
  --remove-section=.xt.lit \
  --remove-section=.xt.prop \
  --remove-section=.xtensa.info "$OUT"

cp "$here/manifest-xtensa.json" "$(dirname "$OUT")/manifest.json"
ls -l "$OUT" "$(dirname "$OUT")/manifest.json"
shasum -a 256 "$OUT"
