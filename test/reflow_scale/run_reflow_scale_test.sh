#!/usr/bin/env bash
# Host regression for the reflow width-scale rule.
#
# The rule decides whether a page build pays ~10 s for an exact scale or takes a
# sampled one; a bug here is either a 12 s wait per page or lines that overflow
# the panel, and neither shows up in a build log.  The mutations copy the header
# next to the test in a scratch tree, because the test's own quoted include
# resolves to its directory first -- -include would have left both versions in
# play and compiled as if nothing changed.
set -euo pipefail
cd "$(dirname "$0")/../.."

SRC=src/activities/reader/ReflowScale.h
TEST=test/reflow_scale/ReflowScaleTest.cpp
WORK=$(mktemp -d)
PRISTINE=$(shasum -a 256 "$SRC" | cut -d' ' -f1)

# The shipped rule runs under the sanitizers; the mutants only need to be
# *compiled*, and the empty-array form of an optional flag trips `set -u`, so
# the two cases are written out instead of folded into one helper.
compile_variant() {  # <header-dir> <binary>
  g++ -std=c++20 -O1 -Wall -Wextra -I "$1" -o "$2" "$TEST"
}

echo "=== the rule as shipped (ASan+UBSan) ==="
cp "$SRC" "$WORK/ReflowScale.h"
g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I "$WORK" -o "$WORK/base" "$TEST"
"$WORK/base"

echo "=== mutation 2: no plausibility band ==="
mkdir -p "$WORK/m2"
sed 's/return scale >= kMinPlausibleScale \&\& scale <= kMaxPlausibleScale;/return true;/' \
    "$SRC" > "$WORK/m2/ReflowScale.h"
cmp -s "$WORK/m2/ReflowScale.h" "$SRC" && { echo "mutation 2 did not change the source" >&2; exit 1; }
compile_variant "$WORK/m2" "$WORK/m2/bin"
if "$WORK/m2/bin" >/dev/null 2>&1; then
  echo "MUTATION SURVIVED: a garbage scale (0.01x, 5x) would be trusted" >&2
  exit 1
fi
echo "killed (as intended)"

echo "=== mutation 4: the load factor stops refusing to insert ==="
mkdir -p "$WORK/m4"
sed 's/if (used \* 4u >= kSlots \* 3u) return;/if (false) {}/' "$SRC" > "$WORK/m4/ReflowScale.h"
cmp -s "$WORK/m4/ReflowScale.h" "$SRC" && { echo "mutation 4 did not change the source" >&2; exit 1; }
compile_variant "$WORK/m4" "$WORK/m4/bin"
if "$WORK/m4/bin" >/dev/null 2>&1; then
  echo "MUTATION SURVIVED: the table can be filled past its stated load factor" >&2
  exit 1
fi
echo "killed (as intended)"

echo "=== mutation 5: one probe slot only ==="
mkdir -p "$WORK/m5"
sed 's/for (unsigned probe = 0; probe < 8; ++probe)/for (unsigned probe = 0; probe < 1; ++probe)/g' \
    "$SRC" > "$WORK/m5/ReflowScale.h"
cmp -s "$WORK/m5/ReflowScale.h" "$SRC" && { echo "mutation 5 did not change the source" >&2; exit 1; }
compile_variant "$WORK/m5" "$WORK/m5/bin"
if "$WORK/m5/bin" >/dev/null 2>&1; then
  echo "MUTATION SURVIVED: a colliding advance is silently dropped (a real reflow cost)" >&2
  exit 1
fi
echo "killed (as intended)"

NOW=$(shasum -a 256 "$SRC" | cut -d' ' -f1)
if [ "$NOW" != "$PRISTINE" ]; then
  echo "the header changed under the test; results are not attributable" >&2
  exit 1
fi
echo "reflow scale: all checks and the surviving mutations pass"
