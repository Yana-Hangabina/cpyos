#!/usr/bin/env bash
# Host check for the allocation-ledger rules (lib/Memory/MemoryLedgerRules.h).
#
# The ledger is the diagnostic that four separate incidents asked for and none of
# them had: who holds this memory, and does the answer agree with the heap. The
# mutations are the three ways a ledger becomes a lie -- counting nothing and
# calling it clean, smoothing a gap into "consistent", and letting the table
# alias without reporting it.
set -euo pipefail

cd "$(dirname "$0")/../.."

HEADER=lib/Memory/MemoryLedgerRules.h
TEST=test/memory_ledger/MemoryLedgerRulesTest.cpp
OUT=${TMPDIR:-/tmp}/memory_ledger_host_test
MUTDIR=${TMPDIR:-/tmp}/memory_ledger_mut
BEFORE=$(shasum -a 256 "$HEADER" | awk '{print $1}')

g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I lib/Memory -o "$OUT" "$TEST"
"$OUT"

rm -rf "$MUTDIR"
mkdir -p "$MUTDIR"

mutate() {
  python3 - "$MUTDIR/MemoryLedgerRules.h" "$1" "$2" <<'PY'
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
  cp "$HEADER" "$MUTDIR/MemoryLedgerRules.h"
  mutate "$2" "$3"
  if g++ -std=c++20 -O1 -Wall -Wextra -I "$MUTDIR" -o "${MUTDIR}/test" "$TEST" > /dev/null 2>&1; then
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

# 1. The floor ignored: every 8-byte allocation enters a 256-row table, which then
#    steals rows from the blocks that matter.
run_mutation "floor ignored" \
  'constexpr bool tracks(const size_t bytes) { return bytes >= kFloorBytes; }' \
  'constexpr bool tracks(const size_t bytes) { return bytes > 0; }'

# 2. The shift dropped: tracked blocks 2 KiB apart collide on purpose, so the
#    table fills with steals and the totals stop matching anything.
run_mutation "slot index collapsed" \
  '  const uint32_t mixed = static_cast<uint32_t>((ptr >> 8u) * 2654435761u);
  return mixed >> (32u - 8u);  // kSlots == 256' \
  '  return 0;'

# 2b. The index this file first shipped with: shift-and-mask. Every check but the
#     distribution ones survives it, which is exactly why the distribution
#     assertions are in the suite.
run_mutation "shift-mask index restored" \
  '  const uint32_t mixed = static_cast<uint32_t>((ptr >> 8u) * 2654435761u);
  return mixed >> (32u - 8u);  // kSlots == 256' \
  '  return (ptr >> 8u) & (kSlots - 1u);'

# 3. PSRAM mistaken for internal RAM: the scarce region is the one the dump is
#    read for.
run_mutation "region confusion" \
  '  if (external) return Region::Psram;
  if (internal) return Region::Internal;' \
  '  if (internal) return Region::Internal;
  if (external) return Region::Psram;'

# 4. A gap smoothed into "consistent": the empty-looking-but-not case.
run_mutation "gap called consistent" \
  '  } else if (worst > bound) {
    check.verdict = Verdict::Gap;' \
  '  } else if (false) {
    check.verdict = Verdict::Gap;'

# 4b. The unsigned-compare mistake this file was written to catch: a negative gap
#     promoted to unsigned becomes the largest possible gap, so every
#     contradiction is filed as a gap and the reader hunts missing
#     instrumentation instead of double counting.
run_mutation "sign comparison restored" \
  '  const int32_t bound = tolerance > static_cast<uint32_t>(INT32_MAX) ? INT32_MAX : static_cast<int32_t>(tolerance);' \
  '  const uint32_t bound = tolerance;'


# 5. A contradiction reported as a gap: it sends the reader after missing
#    instrumentation instead of after double counting.
run_mutation "contradiction collapsed into gap" \
  '    check.verdict = Verdict::Contradiction;
    check.unexplainedBytes = static_cast<uint32_t>(-best);' \
  '    check.verdict = Verdict::Gap;
    check.unexplainedBytes = static_cast<uint32_t>(worst > 0 ? worst : 0);'

# 6. An unwired build answering "consistent": the dump would read like a clean
#    idle device in every build that forgot the --wrap flags.
run_mutation "unwired build looks clean" \
  '    check.verdict = Verdict::NotWired;
    return check;' \
  '    check.verdict = Verdict::Consistent;
    return check;'

# 7. Evicting the older block replaced by dropping the newer one: the totals then
#    keep counting a pointer whose free() can never find this slot.
run_mutation "eviction policy inverted" \
  'constexpr Action insertAction(const bool slotOccupied) { return slotOccupied ? Action::Steal : Action::Insert; }' \
  'constexpr Action insertAction(const bool) { return Action::Insert; }'

# 8. The gap is left unnamed, so "112 KB of internal RAM is unexplained" reads the
#    same whether the floor hid it or an allocator path was never wrapped.
run_mutation "gap cause never attributed" \
  '  } else {
    check.gapCause = counters.smallAllocBytes >= check.unexplainedBytes ? GapCause::PlausiblySubFloor' \
  '  } else {
    check.gapCause = false ? GapCause::PlausiblySubFloor'

# 9. The sub-floor comparison reversed: the floor gets blamed for what it cannot
#    possibly account for.
run_mutation "floor blamed backwards" \
  'counters.smallAllocBytes >= check.unexplainedBytes ? GapCause::PlausiblySubFloor' \
  'counters.smallAllocBytes <= check.unexplainedBytes ? GapCause::PlausiblySubFloor'

# 10. A contradiction blamed on the floor: the reader goes hunting missing
#     instrumentation while the bug is double counting.
run_mutation "contradiction blamed on the floor" \
  '  } else if (check.verdict == Verdict::Contradiction) {
    check.gapCause = GapCause::Bookkeeping;' \
  '  } else if (false) {
    check.gapCause = GapCause::Bookkeeping;'

AFTER=$(shasum -a 256 "$HEADER" | awk '{print $1}')
if [ "$BEFORE" != "$AFTER" ]; then
  echo "FAIL: the original header changed (${BEFORE} -> ${AFTER})"
  exit 1
fi
echo "PASS: ${HEADER} untouched"
echo "memory_ledger: all mutations detected"
