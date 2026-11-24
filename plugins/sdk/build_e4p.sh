#!/bin/sh
# Build a CPYOS plugin ELF for Metalio E-INK4 Plus (ESP32-S31 / RISC-V).
# Usage: build_e4p.sh <plugin.c> <out.elf>
set -eu
here="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
repo="$(CDPATH= cd -- "$here/../.." && pwd)"
src="${1:?usage: build_e4p.sh <plugin.c> <out.elf>}"
out="${2:?usage: build_e4p.sh <plugin.c> <out.elf>}"
TC="${TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-gcc}"
STRIP="${STRIP:-${TC%gcc}strip}"
mkdir -p "$(dirname "$out")"
"$TC" -std=gnu17 -Os \
  -fPIC -shared -nostdlib -nostartfiles -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fvisibility=hidden -fdata-sections -ffunction-sections \
  -Wl,--gc-sections -Wl,--strip-all -Wl,--strip-discarded -e app_main \
  -I "$repo/src/plugins" \
  -o "$out" "$src"
"$STRIP" --strip-unneeded --remove-section=.comment --remove-section=.dynamic \
  --remove-section=.riscv.attributes "$out"
ls -l "$out"
