#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/activities/reader/PdfReaderUi.h
TEST=test/pdf_reader_ui/PdfReaderUiTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
variant() { g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$TEST"; }
patch() {
  mkdir -p "$WORK/$1"
  ~/.platformio/penv/bin/python - "$SRC" "$WORK/$1/PdfReaderUi.h" "$2" "$3" <<'PYEOF'
import sys, pathlib
src, dst, needle, replacement = sys.argv[1:5]
text = pathlib.Path(src).read_text()
assert needle in text, f"the rule moved: {needle}"
pathlib.Path(dst).write_text(text.replace(needle, replacement, 1))
PYEOF
  cmp -s "$WORK/$1/PdfReaderUi.h" "$SRC" && { echo "mutation $1 changed nothing" >&2; exit 1; }
  variant "$WORK/$1" "$WORK/$1/bin"
  if "$WORK/$1/bin" >/dev/null 2>&1; then echo "MUTATION SURVIVED ($1): $4" >&2; exit 1; fi
  echo "killed (as intended)"
}
echo "=== the table as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/PdfReaderUi.h"; variant "$WORK" "$WORK/base"; "$WORK/base"
echo "=== mutation 1: the notice lets input through (tap also turns the page) ==="
patch m1 "if (in == Input::None) return Action::Swallow;
    return Action::DismissHint;" "return Action::Base;" "the bug this round fixed comes back"
echo "=== mutation 2: the escalated card still offers a skip ==="
patch m2 "return s.escalated ? Action::LeaveReader : Action::SkipForward;" "return Action::SkipForward;" "an unreadable file keeps offering 'next page' forever"
echo "=== mutation 3: a load failure yields the frame ==="
patch m3 "return in == Input::None ? Action::Swallow : Action::LeaveReader;" "return Action::Base;" "the dead-end card becomes a dead page"
NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
[ "$NOW" = "$PRISTINE" ] || { echo "header changed under the test" >&2; exit 1; }
echo "=== mutation 4: a missed tap re-runs the page ==="
patch m4 "case Input::TapOther: return Action::Swallow;" "case Input::TapOther: return Action::RetryPage;" "a stray tap pays another full render budget"
echo "=== mutation 5: the skip result ignores whether the turn moved ==="
patch m5 "return moved ? SkipResult::LeaveCard : SkipResult::StayOnCard;" "return SkipResult::LeaveCard;" "skip on the last page silently retries the failed page"
echo "pdf reader ui: all checks and five mutations pass"
