#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/activities/reader/PdfJobSlot.h
TEST=test/pdf_job_slot/PdfJobSlotTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
variant() { g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$TEST"; }
patch() {
  mkdir -p "$WORK/$1"
  ~/.platformio/penv/bin/python - "$SRC" "$WORK/$1/PdfJobSlot.h" "$2" "$3" <<'PYEOF'
import sys, pathlib
src, dst, needle, replacement = sys.argv[1:5]
text = pathlib.Path(src).read_text()
assert needle in text, f"the rule moved: {needle}"
pathlib.Path(dst).write_text(text.replace(needle, replacement, 1))
PYEOF
  cmp -s "$WORK/$1/PdfJobSlot.h" "$SRC" && { echo "mutation $1 changed nothing" >&2; exit 1; }
  variant "$WORK/$1" "$WORK/$1/bin"
  if "$WORK/$1/bin" >/dev/null 2>&1; then echo "MUTATION SURVIVED ($1): $4" >&2; exit 1; fi
  echo "killed (as intended)"
}
echo "=== the rule as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/PdfJobSlot.h"; variant "$WORK" "$WORK/base"; "$WORK/base"
echo "=== mutation 1: recycle a slot whose job is still running ==="
patch m1 "return state.done ? Verdict::Reuse : Verdict::Refuse;" "return Verdict::Reuse;" "the next job would overwrite memory the worker still uses"
echo "=== mutation 2: never recycle after a finished job ==="
patch m2 "return state.done ? Verdict::Reuse : Verdict::Refuse;" "return Verdict::Refuse;" "one slow page would refuse every later render"
echo "=== mutation 3: an expired wait returns the slot anyway ==="
patch m3 "inline bool slotReturnedByWait(const bool completed) { return completed; }" "inline bool slotReturnedByWait(const bool completed) { return true; }" "a timed-out wait would hand the worker's slot to the next job"
NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
[ "$NOW" = "$PRISTINE" ] || { echo "header changed under the test" >&2; exit 1; }
echo "=== mutation 4: deleting the worker under a running job ==="
patch m4 "inline bool mayStopWorker(const State state) { return !state.inFlight || state.done; }" \
         "inline bool mayStopWorker(const State state) { return true; }" \
         "leaving the reader would delete a task in the middle of a page"

echo "pdf job slot: all checks and four mutations pass"
