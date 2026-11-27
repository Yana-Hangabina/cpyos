#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/activities/reader/PdfFailCardLayout.h
TEST=test/pdf_fail_card/PdfFailCardTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
variant() { g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$TEST"; }
patch() {
  mkdir -p "$WORK/$1"
  ~/.platformio/penv/bin/python - "$SRC" "$WORK/$1/PdfFailCardLayout.h" "$2" "$3" <<'PYEOF'
import sys, pathlib
src, dst, needle, replacement = sys.argv[1:5]
text = pathlib.Path(src).read_text()
assert needle in text, f"the layout moved: {needle}"
pathlib.Path(dst).write_text(text.replace(needle, replacement, 1))
PYEOF
  cmp -s "$WORK/$1/PdfFailCardLayout.h" "$SRC" && { echo "mutation $1 changed nothing" >&2; exit 1; }
  variant "$WORK/$1" "$WORK/$1/bin"
  if "$WORK/$1/bin" >/dev/null 2>&1; then echo "MUTATION SURVIVED ($1): $4" >&2; exit 1; fi
  echo "killed (as intended)"
}
echo "=== the layout as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/PdfFailCardLayout.h"; variant "$WORK" "$WORK/base"; "$WORK/base"
echo "=== mutation 1: inclusive right edge (one pixel of the gap becomes Skip) ==="
patch m1 "x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h" "x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h" "adjacent taps overlap"
echo "=== mutation 2: no shrink on narrow screens (a pill leaves the panel) ==="
patch m2 "if (2 * pillW + spec.gap > maxPair) {" "if (false) {" "a half-visible button on a narrow screen"
echo "=== mutation 3: pills not separated by the gap ==="
patch m3 "out.skip.x = left + pillW + spec.gap;" "out.skip.x = left + pillW;" "the two pills touch, so a tap in between is a coin flip"
NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
[ "$NOW" = "$PRISTINE" ] || { echo "header changed under the test" >&2; exit 1; }
echo "pdf fail card: all checks and three mutations pass"
