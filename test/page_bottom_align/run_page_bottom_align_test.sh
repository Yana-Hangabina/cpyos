#!/usr/bin/env bash
# Host regression for the vertical page bottom-align distribution.
#
# Compiles lib/Epub/Epub/PageBottomAlign.h directly (the header is pure: no HAL,
# no Page types), so this runs without cmake or the firmware toolchain.
# Also runs a mutation step: widening the per-gap cap must make the test fail,
# which proves the cap assertions are load-bearing.
set -euo pipefail

cd "$(dirname "$0")/../.."

INCLUDE=lib/Epub/Epub
OUT=${TMPDIR:-/tmp}/page_bottom_align_test

g++ -std=c++20 -O1 -Wall -Wextra -I "$INCLUDE" -o "$OUT" test/page_bottom_align/PageBottomAlignTest.cpp
"$OUT"

# Mutation: double the cap by patching the probe copy of the header.
MUT=${TMPDIR:-/tmp}/page_bottom_align_mut
rm -rf "$MUT"
mkdir -p "$MUT"
cp "$INCLUDE/PageBottomAlign.h" "$MUT/PageBottomAlign.h"
python3 - "$MUT/PageBottomAlign.h" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
needle = "const int maxExtraPerGap = std::max(lineHeight / 2, 1);"
if needle not in src:
    sys.exit("mutation needle missing: the cap expression changed")
open(path, "w").write(src.replace(needle, "const int maxExtraPerGap = std::max(lineHeight, 1);"))
PY
g++ -std=c++20 -O1 -I "$MUT" -o "${MUT}/test" test/page_bottom_align/PageBottomAlignTest.cpp
if "${MUT}/test" > /dev/null 2>&1; then
  echo "FAIL: doubling the per-gap cap did not fail the test (assertions are weak)"
  exit 1
fi
echo "PASS: cap mutation detected"
