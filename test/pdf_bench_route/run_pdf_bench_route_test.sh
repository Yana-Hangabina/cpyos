#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/PdfBenchRoute.h
TEST=test/pdf_bench_route/PdfBenchRouteTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
variant() { g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$TEST"; }
patch() {
  mkdir -p "$WORK/$1"
  ~/.platformio/penv/bin/python - "$SRC" "$WORK/$1/PdfBenchRoute.h" "$2" "$3" <<'PYEOF'
import sys, pathlib
src, dst, needle, replacement = sys.argv[1:5]
text = pathlib.Path(src).read_text()
assert needle in text, f"the rule moved: {needle}"
pathlib.Path(dst).write_text(text.replace(needle, replacement, 1))
PYEOF
  cmp -s "$WORK/$1/PdfBenchRoute.h" "$SRC" && { echo "mutation $1 changed nothing" >&2; exit 1; }
  variant "$WORK/$1" "$WORK/$1/bin"
  if "$WORK/$1/bin" >/dev/null 2>&1; then echo "MUTATION SURVIVED ($1): $4" >&2; exit 1; fi
  echo "killed (as intended)"
}
echo "=== the rule as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/PdfBenchRoute.h"; variant "$WORK" "$WORK/base"; "$WORK/base"
echo "=== mutation 1: always ignore a non-foreground reader (the old behaviour) ==="
patch m1 "if (readerIsInStack) return Route::DismissAndResend;" "" "commands keep vanishing behind a keyboard"
echo "=== mutation 2: pop even when no reader is open ==="
patch m2 "return Route::Ignore;" "return Route::DismissAndResend;" "a stray command pops whatever page is showing"
echo "=== mutation 3: dismiss instead of forwarding a foreground reader ==="
patch m3 "if (readerIsForeground) return Route::Forward;" "if (readerIsForeground) return Route::DismissAndResend;" "a working command becomes a pop"
NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
[ "$NOW" = "$PRISTINE" ] || { echo "header changed under the test" >&2; exit 1; }
echo "pdf bench route: all checks and three mutations pass"
