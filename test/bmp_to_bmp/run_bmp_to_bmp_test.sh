#!/usr/bin/env bash
# Host check for the sleep-cover scaling pass (BmpToBmpConverter + Bitmap::readRowGray).
# No device, no network at run time.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STUBS="$ROOT/test/chapter_html_slim_parser/stubs"
GIF_STUBS="$ROOT/test/gif_support/stubs"

OUT="${TMPDIR:-/tmp}/bmp_to_bmp_test.$$"

${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
  '-DvTaskDelay(x)=((void)0)' \
  -I "$GIF_STUBS" -I "$STUBS" \
  -I "$ROOT/lib/BmpToBmpConverter" \
  -I "$ROOT/lib/GifSupport" \
  -I "$ROOT/lib/GfxRenderer" \
  -I "$ROOT/lib/Memory" \
  -o "$OUT" \
  "$ROOT/test/bmp_to_bmp/bmp_to_bmp_test.cpp" \
  "$ROOT/lib/BmpToBmpConverter/BmpToBmpConverter.cpp" \
  "$ROOT/lib/GfxRenderer/Bitmap.cpp" \
  "$ROOT/lib/GfxRenderer/BitmapHelpers.cpp"

"$OUT"
rm -f "$OUT"
