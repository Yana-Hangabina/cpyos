#!/bin/bash
# Regenerates the embedded Song serif used for page titles (CPY, ReadPico).
# Source: Noto Serif SC Bold (设计「宋体特黑」), OFL (notofonts/noto-cjk, Serif/SubsetOTF/SC),
# vendored at ../builtinFonts/source/NotoSerifSC/ (OFL text kept at
# scripts/LICENSE.NotoSerifSC.txt since the source dir is gitignored). Glyph set: the band/subpage
# title strings — lib/EpdFont/scripts/cn_title_chars.txt (scanned from the
# title call sites plus every *_TITLE translation; regenerate the same way if
# titles change). Everything outside the subset falls back whole-run to UI_12.
set -euo pipefail
cd "$(dirname "$0")"
PYTHON="${PYTHON:-python3}"
SOURCE_OTF="../builtinFonts/source/NotoSerifSC/NotoSerifSC-Bold.otf"
CHARSET_FILE="cn_title_chars.txt"
SUBSET_OTF="instanced_fonts/NotoSerifSC-title.otf"
OUT="../builtinFonts/notoserif_title_23.h"
SIZE=23  # fontconvert renders at 150dpi -> ~48px em, the mockups' title size
[ -f "$SOURCE_OTF" ] || { echo "Error: $SOURCE_OTF not found (OFL, from notofonts/noto-cjk Serif/SubsetOTF/SC)." >&2; exit 1; }
mkdir -p instanced_fonts
"$PYTHON" -m fontTools.subset "$SOURCE_OTF" \
  --output-file="$SUBSET_OTF" \
  --text-file="$CHARSET_FILE" \
  --unicodes="U+0020-007E,U+00A0-00FF,U+2010-2026,U+3000-303F,U+FF00-FF9F,U+FFA1-FFEF,U+FFFD" \
  --layout-features='*' --notdef-outline --recommended-glyphs --no-hinting \
  --drop-tables+=DSIG,GSUB,GPOS
tmp="${OUT}.tmp"
"$PYTHON" fontconvert.py notoserif_title "$SIZE" "$SUBSET_OTF" \
  --2bit \
  --additional-intervals 0x4E00,0x9FFF \
  --additional-intervals 0x3000,0x303F \
  --additional-intervals 0xFF00,0xFFEF \
  > "$tmp"
[ -s "$tmp" ] || { rm -f "$tmp"; echo "Error: empty output" >&2; exit 1; }
mv "$tmp" "$OUT"
echo "Done: $OUT ($(wc -c < "$OUT") bytes). Re-run build-font-ids.sh to refresh NOTOSERIF_TITLE_FONT_ID."
