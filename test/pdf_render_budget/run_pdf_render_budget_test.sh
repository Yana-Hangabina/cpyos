#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/pdf/PdfRenderBudget.h
TEST=test/pdf_render_budget/PdfRenderBudgetTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
variant() { g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$TEST"; }
patch() {
  mkdir -p "$WORK/$1"
  ~/.platformio/penv/bin/python - "$SRC" "$WORK/$1/PdfRenderBudget.h" "$2" "$3" <<'PYEOF'
import sys, pathlib
src, dst, needle, replacement = sys.argv[1:5]
text = pathlib.Path(src).read_text()
assert needle in text, f"the rule moved: {needle}"
pathlib.Path(dst).write_text(text.replace(needle, replacement, 1))
PYEOF
  cmp -s "$WORK/$1/PdfRenderBudget.h" "$SRC" && { echo "mutation $1 changed nothing" >&2; exit 1; }
  variant "$WORK/$1" "$WORK/$1/bin"
  if "$WORK/$1/bin" >/dev/null 2>&1; then echo "MUTATION SURVIVED ($1): $4" >&2; exit 1; fi
  echo "killed (as intended)"
}
echo "=== the rule as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/PdfRenderBudget.h"; variant "$WORK" "$WORK/base"; "$WORK/base"
echo "=== mutation 1: the 8 MB constants drift ==="
patch m1 "return {1536u << 10, 256u << 10, 4u << 20, 3u << 20};" "return {2048u << 10, 256u << 10, 4u << 20, 3u << 20};" "the measured device would silently change behaviour"
echo "=== mutation 2: scaled margins ignore the heap size ==="
patch m2 "const uint32_t eighth = totalPsramBytes / 8;" "const uint32_t eighth = (8u << 20) / 8;" "a 2 MB board would be given 8 MB margins"
echo "=== mutation 3: contiguous slack larger than free slack ==="
patch m3 "return {eighth, eighth / 4, totalPsramBytes / 2, totalPsramBytes / 3};" "return {eighth, eighth * 2, totalPsramBytes / 2, totalPsramBytes / 3};" "the rung would ask for more contiguous bytes than it allows free"
NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
[ "$NOW" = "$PRISTINE" ] || { echo "header changed under the test" >&2; exit 1; }
echo "pdf render budget: all checks and three mutations pass"
