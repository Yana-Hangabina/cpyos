#!/usr/bin/env bash
# Host check for static GIF support (GifToBmpConverter + GifSupport) against the
# real pinned AnimatedGIF, built for the host. No device, no network at run time.
#
# AnimatedGIF comes from the PlatformIO libdeps (already patched by the build),
# or from $ANIMATEDGIF_DIR. If neither exists, the script clones the pinned
# commit into a temp directory (needs network).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PIN="67cfacafe12ef4926c7713342c57d2a4af040e5e"
STUBS="$ROOT/test/chapter_html_slim_parser/stubs"
GIF_STUBS="$ROOT/test/gif_support/stubs"

GIFLIB="${ANIMATEDGIF_DIR:-$ROOT/.pio/libdeps/${PIOENV:-readpico}/AnimatedGIF}"
if [ ! -d "$GIFLIB/src" ]; then
  GIFLIB="$(mktemp -d)/AnimatedGIF"
  echo "AnimatedGIF not found in libdeps; cloning $PIN into $GIFLIB"
  git clone -q https://github.com/bitbank2/AnimatedGIF.git "$GIFLIB"
  git -C "$GIFLIB" checkout -q "$PIN"
fi
python3 "$ROOT/scripts/patch_animatedgif.py" "$GIFLIB" >/dev/null

OUT="${TMPDIR:-/tmp}/gif_support_test.$$"

${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -D__LINUX__ -DMAX_WIDTH=3072 \
  '-DvTaskDelay(x)=((void)0)' \
  -I "$GIF_STUBS" -I "$STUBS" \
  -I "$ROOT/lib/GifSupport" \
  -I "$ROOT/lib/GifToBmpConverter" \
  -I "$ROOT/lib/GfxRenderer" \
  -I "$ROOT/lib/Epub/Epub/converters" \
  -I "$ROOT/lib/Memory" \
  -I "$GIFLIB/src" \
  -o "$OUT" \
  "$ROOT/test/gif_support/gif_bmp_test.cpp" \
  "$ROOT/lib/GifToBmpConverter/GifToBmpConverter.cpp" \
  "$ROOT/lib/Epub/Epub/converters/ImageDimsProbe.cpp" \
  "$ROOT/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp" \
  "$ROOT/lib/GfxRenderer/BitmapHelpers.cpp" \
  "$GIFLIB/src/AnimatedGIF.cpp"

"$OUT"
rm -f "$OUT"
