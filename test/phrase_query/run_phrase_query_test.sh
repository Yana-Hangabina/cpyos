#!/usr/bin/env bash
# Host regression for src/util/SelectionText.h — the reader's selection-to-phrase
# assembly (paragraph / gap / glued joins, CJK adjacency, edge cleaning) and the
# dictionary candidate ladder. Pure code: no HAL, no device, no network.
#
# The mutation step removes the CJK adjacency rule from a probe copy of the
# header; a Chinese two-character selection must then join with a space and the
# suite must go red, proving the rule (and its assertion) is load-bearing.
set -euo pipefail

cd "$(dirname "$0")/../.."

INCLUDE=src/util
OUT=${TMPDIR:-/tmp}/phrase_query_test

g++ -std=c++20 -O1 -Wall -Wextra -Werror \
  -I lib/Utf8 -I src \
  -o "$OUT" test/phrase_query/PhraseQueryTest.cpp lib/Utf8/Utf8.cpp
"$OUT"

# Mutation: disable the CJK adjacency rule (suppress nothing).
MUT=${TMPDIR:-/tmp}/phrase_query_mut
rm -rf "$MUT"
mkdir -p "$MUT/util"
cp "$INCLUDE/SelectionText.h" "$MUT/util/SelectionText.h"
python3 - "$MUT/util/SelectionText.h" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
needle = "inline bool gluesToNeighbour(const uint32_t cp) { return utf8IsCjkBreakable(cp) && !utf8IsHangul(cp); }"
if needle not in src:
    sys.exit("mutation needle missing: the CJK adjacency rule changed")
open(path, "w").write(src.replace(needle, "inline bool gluesToNeighbour(const uint32_t) { return false; }"))
PY
g++ -std=c++20 -O1 -I lib/Utf8 -I "$MUT" -I src -o "${MUT}/test" \
  test/phrase_query/PhraseQueryTest.cpp lib/Utf8/Utf8.cpp
if "${MUT}/test" > /dev/null 2>&1; then
  echo "FAIL: dropping the CJK gap rule did not fail the test (assertions are weak)"
  exit 1
fi
echo "PASS: CJK gap-rule mutation detected"
