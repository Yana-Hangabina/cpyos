#!/bin/sh
# Build a CPYOS plugin ELF for Read Pico (ESP32-S3 / Xtensa).
# Usage: build_xtensa.sh <plugin.c> <out.elf>
set -eu
here="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
repo="$(CDPATH= cd -- "$here/../.." && pwd)"
src="${1:?usage: build_xtensa.sh <plugin.c> <out.elf>}"
out="${2:?usage: build_xtensa.sh <plugin.c> <out.elf>}"
TC="${TC:-$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc}"
STRIP="${STRIP:-${TC%gcc}strip}"
mkdir -p "$(dirname "$out")"
"$TC" -std=gnu17 -Os \
  -fPIC -shared -nostdlib -nostartfiles -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fvisibility=hidden -fdata-sections -ffunction-sections \
  -Wl,--gc-sections -Wl,--strip-all \
  -mlongcalls -mno-text-section-literals -e app_main \
  -I "$repo/src/plugins" \
  -o "$out" "$src"
"$STRIP" --strip-unneeded \
  --remove-section=.comment --remove-section=.dynamic \
  --remove-section=.xt.lit --remove-section=.xt.prop \
  --remove-section=.xtensa.info "$out"
ls -l "$out"
