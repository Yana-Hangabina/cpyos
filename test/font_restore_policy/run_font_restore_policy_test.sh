#!/usr/bin/env bash
# Host check for the render-memory restore rule (src/FontRestorePolicy.h).
#
# Shipped firmware released the reader font state for every Wi-Fi bring-up and
# never put it back, so screens drawn afterwards fell to the ~1,500-codepoint UI
# subset in app0 and boxed characters the data partition carries. The fix is a
# pairing with four gating terms; a conjunction tested only as a whole can pass
# with any one term deleted, so each term gets its own mutation here.
#
# The mutations run against a copy. The header is hashed before and after so a
# broken patch can never leave a marker behind in the tree of record.
set -euo pipefail

cd "$(dirname "$0")/../.."

HEADER=src/FontRestorePolicy.h
OUT=${TMPDIR:-/tmp}/font_restore_policy_host_test
MUTDIR=${TMPDIR:-/tmp}/font_restore_policy_mut
BEFORE=$(shasum -a 256 "$HEADER" | awk '{print $1}')

g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I src \
    -o "$OUT" test/font_restore_policy/FontRestorePolicyTest.cpp
"$OUT"

rm -rf "$MUTDIR"
mkdir -p "$MUTDIR"

mutate() {  # mutate.py <needle> <replacement>
  python3 - "$MUTDIR/FontRestorePolicy.h" "$1" "$2" <<'PY'
import sys
path, needle, replacement = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(path).read()
if src.count(needle) != 1:
    sys.exit("mutation needle missing or ambiguous (%d occurrences): %s" % (src.count(needle), needle))
open(path, 'w').write(src.replace(needle, replacement))
PY
}

run_mutation() {  # run_mutation <name> <needle> <replacement>
  local name=$1
  cp "$HEADER" "$MUTDIR/FontRestorePolicy.h"
  mutate "$2" "$3"
  if g++ -std=c++20 -O1 -Wall -Wextra -I "$MUTDIR" \
       -o "${MUTDIR}/test" test/font_restore_policy/FontRestorePolicyTest.cpp > /dev/null 2>&1; then
    if "${MUTDIR}/test" > /dev/null 2>&1; then
      echo "FAIL: the ${name} mutation was not detected"
      exit 1
    fi
  else
    echo "FAIL: the ${name} mutation did not compile"
    exit 1
  fi
  echo "PASS: ${name} mutation detected"
}

# 1. The shipped bug: act regardless of whether anything was reclaimed.
run_mutation "pending ignored" \
  '  return situation.pending && situation.radioIdle && !situation.stationClaimed && !situation.networkHeld &&' \
  '  return situation.radioIdle && !situation.stationClaimed && !situation.networkHeld &&'

# 2. Restoring while a driver command executes.
run_mutation "driver command ignored" \
  'situation.pending && situation.radioIdle && !situation.stationClaimed' \
  'situation.pending && !situation.stationClaimed'

# 3. Restoring between the mode change and the join.
run_mutation "bring-up claim ignored" \
  '&& !situation.stationClaimed && !situation.networkHeld' \
  '&& !situation.networkHeld'

# 4. Restoring in the middle of a request or download.
run_mutation "network hold ignored" \
  '&& !situation.stationClaimed && !situation.networkHeld' \
  '&& !situation.stationClaimed'

# 5. The quiet floor removed: restore on the very next loop pass.
run_mutation "quiet floor removed" \
  'situation.elapsedMs >= kMinQuietMs;' \
  'true;'

AFTER=$(shasum -a 256 "$HEADER" | awk '{print $1}')
if [ "$BEFORE" != "$AFTER" ]; then
  echo "FAIL: the original header changed (${BEFORE} -> ${AFTER})"
  exit 1
fi
echo "PASS: ${HEADER} untouched"
echo "font_restore_policy: all mutations detected"
