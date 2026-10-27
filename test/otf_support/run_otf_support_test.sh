#!/bin/sh
# Host regression for TF-card vector fonts: an OpenType face with PostScript
# outlines (CFF, `.otf`) must load through the SAME FreeType amalgamation the
# firmware compiles. The vendored build used to be truetype-only, so `.otf`
# failed FT_New_Face with 0x02 even though the registry advertised the
# extension — this pins the fix in both directions:
#   * FREEINK_FONT_ENABLE_CFF=1 : CFF fixture loads and rasterizes, and a
#     plain TrueType fixture still works (no regression).
#   * FREEINK_FONT_ENABLE_CFF=0 : the same CFF fixture MUST be rejected, so the
#     test would go red if someone "fixes" rejection by disabling the driver
#     check or if the flag stops reaching the amalgamation.
#
# No cmake / gtest needed: mirrors src/*/run_*.sh. Run from anywhere:
#   bash test/otf_support/run_otf_support_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
FONT_ROOT="$REPO_ROOT/freeink-sdk/libs/font/FreeInkFont"
INC="-I$FONT_ROOT/third_party/freetype/include"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/otf-support-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

CFF_FIXTURE="$SCRIPT_DIR/cff_fixture.otf"
TTF_FIXTURE="$REPO_ROOT/freeink-sdk/libs/book/FreeInkBook/test/fixtures/fonts/DejaVuSans.ttf"

if [ ! -f "$CFF_FIXTURE" ]; then
  echo "SKIP: $CFF_FIXTURE missing" >&2
  exit 0
fi
if [ ! -f "$TTF_FIXTURE" ]; then
  echo "SKIP: $TTF_FIXTURE missing (submodule fixture)" >&2
  exit 0
fi

build_variant() {
  variant=$1
  cff=$2
  mkdir -p "$BUILD_DIR/$variant"
  for src in "$FONT_ROOT"/src/freetype/*.c; do
    obj="$BUILD_DIR/$variant/$(basename "$src" .c).o"
    cc -std=c11 -O1 $INC -DFREEINK_FONT_ENABLE_CFF="$cff" -c "$src" -o "$obj"
  done
  cc -std=c99 -O1 $INC -DFREEINK_FONT_ENABLE_CFF="$cff" -o "$BUILD_DIR/$variant/test" \
    "$SCRIPT_DIR/OtfLoadHostTest.c" "$BUILD_DIR/$variant"/ft_*.o -lm
}

echo "== building FREEINK_FONT_ENABLE_CFF=1 =="
build_variant cff 1
echo "-- .otf (CFF outline) must load --"
"$BUILD_DIR/cff/test" "$CFF_FIXTURE" 1
echo "-- .ttf (TrueType outline) must still load --"
"$BUILD_DIR/cff/test" "$TTF_FIXTURE" 0

echo "== building FREEINK_FONT_ENABLE_CFF=0 =="
build_variant nocff 0
echo "-- .otf (CFF outline) must be rejected --"
if "$BUILD_DIR/nocff/test" "$CFF_FIXTURE" 0; then
  echo "FAIL: .otf loaded without the CFF driver — rejection no longer pinned" >&2
  exit 1
fi
echo "-- .ttf still loads on the CFF-less build --"
"$BUILD_DIR/nocff/test" "$TTF_FIXTURE" 0

# Split-stream pressure: the firmware serves streamed faces from a RAM prefix
# plus SD tail reads, so CFF's end-of-file tables must survive reads that
# straddle the cache boundary. Generated on the fly (needs fontTools); skip
# when unavailable, like the submodule fixtures above.
TAIL_FIXTURE="$BUILD_DIR/cff_tail.otf"
if python3 -c "import fontTools" 2>/dev/null; then
  python3 "$SCRIPT_DIR/gen_tail_fixture.py" "$CFF_FIXTURE" "$TAIL_FIXTURE" >/dev/null
  cc -std=c99 -O1 $INC -DFREEINK_FONT_ENABLE_CFF=1 -o "$BUILD_DIR/stream_test" \
    "$SCRIPT_DIR/OtfStreamHostTest.c" "$BUILD_DIR/cff"/ft_*.o -lm
  echo "-- split-stream (64 KB prefix of a padded CFF file) --"
  "$BUILD_DIR/stream_test" "$TAIL_FIXTURE" 65536
  echo "-- split-stream (4 KB prefix) --"
  "$BUILD_DIR/stream_test" "$TAIL_FIXTURE" 4096
  echo "-- fully-cached prefix (no tail reads expected) --"
  "$BUILD_DIR/stream_test" "$TAIL_FIXTURE" 99999999

  # Driver-order regressions the CFF registration could disturb: a TrueType
  # Collection must still be claimed by the tt driver (tt is registered before
  # cff), and driver choice follows CONTENT, not the file extension (a
  # glyf-outline font named .otf must still load).
  TTC_FIXTURE="$BUILD_DIR/dejavu.ttc"
  python3 - "$TTF_FIXTURE" "$TTC_FIXTURE" <<'PYEOF'
import sys
from fontTools.ttLib import TTFont, TTCollection
font = TTFont(sys.argv[1])
col = TTCollection()
col.fonts = [font, font]
col.save(sys.argv[2])
PYEOF
  cp "$TTF_FIXTURE" "$BUILD_DIR/dejavu_ttflavor.otf"
  echo "-- .ttc (TrueType collection) under the CFF build --"
  "$BUILD_DIR/cff/test" "$TTC_FIXTURE" 0
  echo "-- TrueType outlines named .otf (driver follows content) --"
  "$BUILD_DIR/cff/test" "$BUILD_DIR/dejavu_ttflavor.otf" 0
else
  echo "SKIP: fontTools unavailable for the split-stream fixture"
fi

echo ""
echo "ALL OTF SUPPORT CHECKS PASSED"
