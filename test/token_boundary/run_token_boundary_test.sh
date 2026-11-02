#!/usr/bin/env bash
# Host regression for the justification gap rule (upstream 8b2ba5e9).
#
# Compiles lib/Epub/Epub/TokenBoundary.h directly -- the header is pure (no HAL, no
# Page types), so this runs without cmake or the firmware toolchain.
# Also runs a mutation step: if a space token without a continuation becomes
# stretchable again (the pre-fix rule), the test must fail. That proves the
# assertions are load-bearing rather than merely present.
set -euo pipefail

cd "$(dirname "$0")/../.."

INCLUDE=lib/Epub/Epub
OUT=${TMPDIR:-/tmp}/token_boundary_host_test

g++ -std=c++20 -O1 -Wall -Wextra -I "$INCLUDE" -o "$OUT" test/token_boundary/TokenBoundaryHostTest.cpp
"$OUT"

# Mutation 1: the old rule, which stretched any attached space token.
MUT=${TMPDIR:-/tmp}/token_boundary_mut
rm -rf "$MUT"
mkdir -p "$MUT/hyphenation"
cp "$INCLUDE/TokenBoundary.h" "$MUT/TokenBoundary.h"
cp "$INCLUDE/hyphenation/HyphenationCommon.h" "$MUT/hyphenation/HyphenationCommon.h"
python3 - "$MUT/TokenBoundary.h" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
needle = "return !continues || isStretchableSpace;"
if needle not in src:
    sys.exit("mutation needle missing: the gap rule changed shape")
open(path, "w").write(src.replace(needle, "return !continues || true;"))
PY
g++ -std=c++20 -O1 -I "$MUT" -o "${MUT}/test" test/token_boundary/TokenBoundaryHostTest.cpp
if "${MUT}/test" > /dev/null 2>&1; then
  echo "FAIL: restoring the pre-fix rule did not fail the test (assertions are weak)"
  exit 1
fi
echo "PASS: pre-fix rule mutation detected"

# Mutation 2: never stretch an attached space, which would break no-break-space justification.
cp "$INCLUDE/TokenBoundary.h" "$MUT/TokenBoundary.h"
python3 - "$MUT/TokenBoundary.h" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
needle = "return !continues || isStretchableSpace;"
if needle not in src:
    sys.exit("mutation needle missing: the gap rule changed shape")
open(path, "w").write(src.replace(needle, "return !continues;"))
PY
g++ -std=c++20 -O1 -I "$MUT" -o "${MUT}/test2" test/token_boundary/TokenBoundaryHostTest.cpp
if "${MUT}/test2" > /dev/null 2>&1; then
  echo "FAIL: dropping stretchable spaces did not fail the test"
  exit 1
fi
echo "PASS: no-stretch mutation detected"
