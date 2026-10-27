#!/bin/sh
# Host regression for GfxRenderer::getGlyphBitmap's resident-bitmap fallback.
#
# The device rebooted with "Cache error / MMU entry fault" while drawing a Chinese blurb:
# an SD-card face in stub mode has EpdFontData::bitmap == nullptr (SdCardFont.cpp memsets
# stubData and never assigns it), and a missed overflow-ring lookup fell through to
# &fontData->bitmap[glyph->dataOffset]. &nullptr[large] is a non-null, unmapped pointer that
# survived the caller's `== nullptr` check and faulted on the first read.
#
# The test compiles the SAME header-only function the renderer calls (no copy), and the final
# grep pins that the renderer actually routes through it -- otherwise the helper test would
# keep passing while an unguarded fallback was reintroduced at the call site.
#
# No cmake / gtest needed. Run from anywhere:
#   bash test/glyph_bitmap_null_base/run_glyph_bitmap_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
CXX=${CXX:-c++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/glyph-null-base.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

# -Werror is deliberately omitted: EpdFontData.h (pre-existing) triggers Clang's
# -Wnon-c-typedef-for-linkage, and the project's host tests compile with -Wall -Wextra only.
FLAGS="-std=c++20 -O1 -Wall -Wextra -I$REPO_ROOT/lib/EpdFont"
# Match the other host tests: ASan+UBSan when the compiler supports them. UBSan must
# ABORT rather than recover: a mutant that removes the guard computes &nullptr[offset],
# and with the default recover behaviour the assertion can pass anyway (the compiler is
# free to exploit that UB). -fno-sanitize-recover turns it into a hard failure.
if printf 'int main(){}\n' | "$CXX" -x c++ -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$BUILD_DIR/probe" - 2>/dev/null; then
  FLAGS="$FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
fi

# shellcheck disable=SC2086
"$CXX" $FLAGS "$SCRIPT_DIR/GlyphBitmapNullBaseTest.cpp" -o "$BUILD_DIR/test"
"$BUILD_DIR/test"

if ! grep -q 'return residentGlyphBitmap(fontData, glyph);' "$REPO_ROOT/lib/GfxRenderer/GfxRenderer.cpp"; then
  echo "FAIL: GfxRenderer::getGlyphBitmap no longer routes through residentGlyphBitmap()" >&2
  exit 1
fi
echo "glyph bitmap null-base guard: OK"
