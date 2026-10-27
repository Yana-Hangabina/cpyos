#!/bin/bash
# Cross-compile MuPDF 1.26.3 for ESP32-S3 (Xtensa), PDF-only, external CMaps.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/mupdf-1.26.3-source"
TC="${XTENSA_TOOLCHAIN:-$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin}"
OUT="${MUPDF_OUT:-build/xtensa_nocjk}"

make -C "$S" -j"${JOBS:-8}" OUT="$OUT" libs \
  CC="$TC/xtensa-esp32s3-elf-gcc" \
  CXX="$TC/xtensa-esp32s3-elf-g++" \
  AR="$TC/xtensa-esp32s3-elf-ar" \
  RANLIB="$TC/xtensa-esp32s3-elf-ranlib" \
  build=release \
  html=no xps=no mujs=no brotli=no extract=no barcode=no tesseract=no \
  XCFLAGS="-Os -mlongcalls -mtext-section-literals -ffunction-sections -fdata-sections -D_GNU_SOURCE -D_DEFAULT_SOURCE -DNO_CJK -DFZ_ENABLE_HYPHEN=0 -DTOFU -DTOFU_CJK -DTOFU_CJK_EXT -DTOFU_BASE14 -Wno-error=incompatible-pointer-types -Wno-incompatible-pointer-types -include $HERE/timegm_compat.h"
