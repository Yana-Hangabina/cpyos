#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/pdf/PdfDeadline.h
TEST=test/pdf_deadline/PdfDeadlineTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
variant() { g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$TEST"; }
patch() {
  mkdir -p "$WORK/$1"
  ~/.platformio/penv/bin/python - "$SRC" "$WORK/$1/PdfDeadline.h" "$2" "$3" <<'PYEOF'
import sys, pathlib
src, dst, needle, replacement = sys.argv[1:5]
text = pathlib.Path(src).read_text()
assert needle in text, f"the rule moved: {needle}"
pathlib.Path(dst).write_text(text.replace(needle, replacement, 1))
PYEOF
  cmp -s "$WORK/$1/PdfDeadline.h" "$SRC" && { echo "mutation $1 changed nothing" >&2; exit 1; }
  variant "$WORK/$1" "$WORK/$1/bin"
  if "$WORK/$1/bin" >/dev/null 2>&1; then echo "MUTATION SURVIVED ($1): $4" >&2; exit 1; fi
  echo "killed (as intended)"
}
echo "=== the rule as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/PdfDeadline.h"; variant "$WORK" "$WORK/base"; "$WORK/base"
echo "=== mutation 1: plain unsigned comparison (breaks across the wrap) ==="
patch m1 "return deadlineMs != 0 && static_cast<int32_t>(nowMs - deadlineMs) >= 0;" \
         "return deadlineMs != 0 && nowMs >= deadlineMs;" "a 49-day uptime flips the budget"
echo "=== mutation 2: zero deadline means expired (no budget becomes instant failure) ==="
patch m2 "return deadlineMs != 0 && static_cast<int32_t>(nowMs - deadlineMs) >= 0;" \
         "return static_cast<int32_t>(nowMs - deadlineMs) >= 0;" "callers that pass 0 fail every page"
echo "=== mutation 3: the boundary is exclusive ==="
patch m3 "static_cast<int32_t>(nowMs - deadlineMs) >= 0" "static_cast<int32_t>(nowMs - deadlineMs) > 0" \
         "a budget expires one tick late (unpinned boundary)"
NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
[ "$NOW" = "$PRISTINE" ] || { echo "header changed under the test" >&2; exit 1; }
echo "pdf deadline: all checks and three mutations pass"
