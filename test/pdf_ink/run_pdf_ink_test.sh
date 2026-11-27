#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/pdf/PdfInk.h
TEST=test/pdf_ink/PdfInkTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
variant() { g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$TEST"; }
patch() {
  mkdir -p "$WORK/$1"
  ~/.platformio/penv/bin/python - "$SRC" "$WORK/$1/PdfInk.h" "$2" "$3" <<'PYEOF'
import sys, pathlib
src, dst, needle, replacement = sys.argv[1:5]
text = pathlib.Path(src).read_text()
assert needle in text, f"the rule moved: {needle}"
pathlib.Path(dst).write_text(text.replace(needle, replacement, 1))
PYEOF
  cmp -s "$WORK/$1/PdfInk.h" "$SRC" && { echo "mutation $1 changed nothing" >&2; exit 1; }
  variant "$WORK/$1" "$WORK/$1/bin"
  if "$WORK/$1/bin" >/dev/null 2>&1; then echo "MUTATION SURVIVED ($1): $4" >&2; exit 1; fi
  echo "killed (as intended)"
}
echo "=== the rule as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/PdfInk.h"; variant "$WORK" "$WORK/base"; "$WORK/base"
echo "=== mutation 1: nothing is ever blank (a scan shows a white sheet) ==="
patch m1 "inline bool blank(const float inkRatio) { return inkRatio < kMinInk; }" \
         "inline bool blank(const float inkRatio) { return false; }" \
         "the silent blank page is back"
echo "=== mutation 2: any stray ink counts as blank (real pages get rejected) ==="
patch m2 "constexpr float kMinInk = 5e-4f;  // 0.05% of the page" \
         "constexpr float kMinInk = 5e-2f;  // 5% of the page" \
         "a sparse text page is thrown away as unreadable"
echo "=== mutation 3: the boundary comparison is inclusive ==="
patch m3 "return inkRatio < kMinInk;" "return inkRatio <= kMinInk;" \
         "the floor literals in the test no longer describe the rule"
NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
[ "$NOW" = "$PRISTINE" ] || { echo "header changed under the test" >&2; exit 1; }
echo "=== mutation 4: the one-tone guard disappears (a blank scan reads as full ink) ==="
patch m4 "  if (high < low + 24u) return 0.0f;" "" "a flat page reports 100% ink"
echo "=== mutation 5: the midpoint sample stops counting as ink ==="
patch m5 "for (unsigned v = 0; v <= mid; ++v) dark += hist[v];" "for (unsigned v = 0; v < mid; ++v) dark += hist[v];" "the '<=' boundary is unpinned"
echo "pdf ink: all checks and five mutations pass"
