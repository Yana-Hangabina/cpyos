#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/activities/reader/PdfFailStreak.h
TEST=test/pdf_fail_streak/PdfFailStreakTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
variant() { g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$TEST"; }
patch() {
  mkdir -p "$WORK/$1"
  ~/.platformio/penv/bin/python - "$SRC" "$WORK/$1/PdfFailStreak.h" "$2" "$3" <<'PYEOF'
import sys, pathlib
src, dst, needle, replacement = sys.argv[1:5]
text = pathlib.Path(src).read_text()
assert needle in text, f"the rule moved: {needle}"
pathlib.Path(dst).write_text(text.replace(needle, replacement, 1))
PYEOF
  cmp -s "$WORK/$1/PdfFailStreak.h" "$SRC" && { echo "mutation $1 changed nothing" >&2; exit 1; }
  variant "$WORK/$1" "$WORK/$1/bin"
  if "$WORK/$1/bin" >/dev/null 2>&1; then echo "MUTATION SURVIVED ($1): $4" >&2; exit 1; fi
  echo "killed (as intended)"
}
echo "=== the rule as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/PdfFailStreak.h"; variant "$WORK" "$WORK/base"; "$WORK/base"
echo "=== mutation 1: escalate at two failures ==="
patch m1 "constexpr int kEscalateAfter = 3;" "constexpr int kEscalateAfter = 2;" "a skip-worthy page run would throw the book away"
echo "=== mutation 2: never escalate ==="
patch m2 "return consecutiveFailures >= kEscalateAfter;" "return false;" "a wholly unreadable book keeps offering 'next page'"
echo "=== mutation 3: a success does not reset the streak ==="
patch m3 "return rendered ? 0 : streak + 1;" "return rendered ? streak : streak + 1;" "isolated bad pages accumulate into a false 'unreadable'"
NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
[ "$NOW" = "$PRISTINE" ] || { echo "header changed under the test" >&2; exit 1; }
echo "pdf fail streak: all checks and three mutations pass"
