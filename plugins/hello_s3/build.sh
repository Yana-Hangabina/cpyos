#!/bin/sh
# Build the readpico hello plugin with the same Xtensa toolchain PlatformIO
# uses for the host firmware. The ELF is a shared object with app_main as the
# entry; libc calls stay undefined and are resolved by the host elfloader.
set -eu
TC=${TC:-$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc}
STRIP=${STRIP:-${TC%gcc}strip}
OUT=${1:-hello.app.elf}
"$TC" -std=gnu17 -Os \
  -fPIC -shared -nostdlib -nostartfiles \
  -fvisibility=hidden -fdata-sections -ffunction-sections \
  -Wl,--gc-sections -Wl,--strip-all \
  -mlongcalls -e app_main -o "$OUT" hello.c
# Mirror the loader's project_elf() strip set: .dynamic and Xtensa metadata
# sections are not part of the runtime ELF the loader relocates.
exec "$STRIP" --strip-unneeded \
  --remove-section=.comment \
  --remove-section=.dynamic \
  --remove-section=.xt.lit \
  --remove-section=.xt.prop \
  --remove-section=.xtensa.info "$OUT"
