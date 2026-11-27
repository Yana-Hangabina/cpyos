#!/usr/bin/env bash
# Host regression for the PDF worker wait rule (src/activities/reader/PdfJobWait.h).
#
# Each mutation is a mistake this rule was built to prevent, and each one is
# checked to actually change the source before it is allowed to be called a
# mutation -- a sed that matches nothing turns the whole suite green by
# construction, which is worse than having no suite.
set -euo pipefail
cd "$(dirname "$0")/../.."

SRC=src/activities/reader/PdfJobWait.h
TEST=test/pdf_job_wait/PdfJobWaitTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)

variant() {  # <header-dir> <binary>
  g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$TEST"
}

patch() {  # <name> <needle> <replacement>
  mkdir -p "$WORK/$1"
  ~/.platformio/penv/bin/python - "$SRC" "$WORK/$1/PdfJobWait.h" "$2" "$3" <<'PYEOF'
import sys, pathlib
src, dst, needle, replacement = sys.argv[1:5]
text = pathlib.Path(src).read_text()
assert needle in text, f"the rule moved; update this mutation: {needle}"
pathlib.Path(dst).write_text(text.replace(needle, replacement, 1))
PYEOF
  if cmp -s "$WORK/$1/PdfJobWait.h" "$SRC"; then
    echo "mutation $1 did not change the source" >&2
    exit 1
  fi
  variant "$WORK/$1" "$WORK/$1/bin"
  if "$WORK/$1/bin" >/dev/null 2>&1; then
    echo "MUTATION SURVIVED ($1): $4" >&2
    exit 1
  fi
  echo "killed (as intended)"
}

echo "=== the rule as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/PdfJobWait.h"
variant "$WORK" "$WORK/base"
"$WORK/base"

echo "=== mutation 1: elapsed clamped instead of modular (the millis() wrap) ==="
patch m1 "return nowMs - startedAtMs;" \
         "return nowMs > startedAtMs ? nowMs - startedAtMs : 0;" \
         "a 49-day uptime would make the wait never expire"

echo "=== mutation 2: announcements only on the timeout ==="
patch m2 "const bool due = elapsed >= static_cast<uint32_t>(announced + 1u) * kProgressEveryMs;" \
         "const bool due = false;" \
         "a slow page would print nothing until it was already lost"

echo "=== mutation 3: announcing on entry (the signal drowns) ==="
patch m3 "const bool due = elapsed >= static_cast<uint32_t>(announced + 1u) * kProgressEveryMs;" \
         "const bool due = announced == 0u || elapsed >= static_cast<uint32_t>(announced) * kProgressEveryMs;" \
         "every page turn would log 'pdf worker waiting', and nobody would read it as a diagnosis"

echo "=== mutation 4: a zero timeout waits forever ==="
patch m4 "const bool timedOut = elapsed >= timeoutMs;" \
         "const bool timedOut = timeoutMs != 0 && elapsed >= timeoutMs;" \
         "'no timeout' became reachable again, which is the silent hang"

NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
if [ "$NOW" != "$PRISTINE" ]; then
  echo "the header changed under the test; results are not attributable" >&2
  exit 1
fi
echo "pdf job wait: all checks and four mutations pass"
