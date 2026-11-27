#!/bin/sh
# Host regression for the fault ring the crash screen prints.
#
#   lib/Diagnostics/FaultLog.cpp    -- the RTC-surviving ring, the panic classifier
#   lib/Diagnostics/FaultCodes.h    -- the code/tag/meaning table
#
# It compiles the REAL source with -DSIMULATOR, which swaps only the storage class
# (RTC_NOINIT_ATTR -> static), the clock (uptime seconds -> a value the test sets)
# and the reset-reason lookup. raise/copy/format/classify are the shipped code,
# because every interesting failure mode here lives in those branches: dedupe,
# eviction order, the "one death, one entry" suppression, and a tag that gets
# chopped in half on a tight buffer.
#
# Why this suite exists: the artifacts this produces are a photograph and a text
# file a non-technical user sends. A ring that dedupes wrongly reads as "happened
# 400 times, calmly"; one that stops recording when full hides the fault that
# explains the crash; a truncated tag decodes to less than nothing.
#
# Each mutation removes one decision and must turn the suite red. Two vacuous green
# paths are refused outright (both have bitten this tree before):
#   * a patch that does not modify the file at all      -- caught by cmp
#   * a patch that lands somewhere unintended           -- caught by marker
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
CPP_REL="lib/Diagnostics/FaultLog.cpp"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/record-faults-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out=$1
  root=$2
  mkdir -p "$(dirname "$out")"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -DSIMULATOR -fsanitize=address,undefined \
    -I"$root/lib/Diagnostics" \
    -o "$out" "$SCRIPT_DIR/RecordFaultsTest.cpp" "$root/$CPP_REL"
  "$out"
}

mutate() {
  label=$1
  marker=$2
  dir="$BUILD_DIR/m_$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  mkdir -p "$dir/lib/Diagnostics"
  cp "$REPO_ROOT/lib/Diagnostics/FaultCodes.h" "$REPO_ROOT/lib/Diagnostics/FaultLog.h" "$dir/lib/Diagnostics/"
  cp "$REPO_ROOT/$CPP_REL" "$dir/$CPP_REL"
  # The heredoc at the call site is a python function body; assembling it into a
  # real file (rather than `python3 - <<PY`) keeps the patch text quoted and free
  # of shell escaping.
  {
    echo 'import sys'
    echo 'path = sys.argv[1]'
    echo 'with open(path, encoding="utf-8") as handle:'
    echo '    text = handle.read()'
    echo 'def patch(text):'
    cat
    echo 'with open(path, "w", encoding="utf-8") as handle:'
    echo '    handle.write(patch(text))'
  } > "$dir/patch.py"
  python3 "$dir/patch.py" "$dir/$CPP_REL"
  if cmp -s "$dir/$CPP_REL" "$REPO_ROOT/$CPP_REL"; then
    echo "FAIL: '$label' did not modify $CPP_REL; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/$CPP_REL"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker')" >&2
    exit 1
  fi
  # Compile and run separately: a mutation that merely does not build would be
  # reported as "detected", which is a green that proves nothing.
  mkdir -p "$dir-run"
  if ! "$CXX" -std=c++20 -O1 -Wall -Wextra -DSIMULATOR -fsanitize=address,undefined \
       -I"$dir/lib/Diagnostics" -o "$dir-run/test" "$SCRIPT_DIR/RecordFaultsTest.cpp" "$dir/$CPP_REL" \
       >"$dir/compile.log" 2>&1; then
    echo "FAIL: mutation '$label' does not compile; it proves nothing" >&2
    sed -n '1,20p' "$dir/compile.log" >&2
    exit 1
  fi
  if "$dir-run/test" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($label)" >&2
    exit 1
  fi
  echo "mutation detected ($label)"
}

echo "== fault ring: record, order, classify, format =="
run_suite "$BUILD_DIR/normal/test" "$REPO_ROOT"

# 1. A repeat claims a new slot instead of counting the existing one, so 21 hits of
#    one fault fill the ring and push everything else off the screen.
mutate "no dedupe on repeat" '/* XM-DEDUPE */' <<'PY'
    return text.replace(
        "  for (uint8_t i = 0; i < kCapacity; ++i) {\n    if (gSlots[i].code == value) return i;\n  }",
        "  (void)value; /* XM-DEDUPE */ // a repeat is now treated as a brand new code", 1)
PY

# 2. Eviction recycles the most recently touched entry. The newcomer is still
#    recorded, so only the entry that explains the crash disappears.
mutate "evict the newest not the oldest" '/* XM-EVICT */' <<'PY'
    text = text.replace("uint32_t oldest = UINT32_MAX;", "uint32_t oldest = 0;  /* XM-EVICT */", 1)
    return text.replace("if (gSlots[i].seq < oldest) {", "if (gSlots[i].seq > oldest) {", 1)
PY

# 3. The abort that follows a terminate handler is classified again, so one death
#    shows up as two faults and the vaguer one wins the newest slot.
mutate "no suppression after a C++ failure" '/* XM-SUPPRESS */' <<'PY'
    return text.replace("  if (gCppFailureNoted) return;\n", "  /* XM-SUPPRESS */\n", 1)
PY

# 4. copy() walks the array by index instead of by touch stamp: the screen then
#    leads with the oldest thing it recorded.
mutate "ring ordered by slot index" '/* XM-ORDER */' <<'PY'
    return text.replace("if (best < 0 || gSlots[i].seq > bestSeq) {", "if (best < 0) {  /* XM-ORDER */", 1)
PY

# 5. formatLine reserves only the separator, so a tight buffer gets half a tag --
#    "STK-0" on a photograph is a code that does not exist.
mutate "tags may be truncated mid-entry" '/* XM-TRUNC */' <<'PY'
    text = text.replace(
        "if (used + static_cast<uint16_t>(length) + 1 > capacity) break;",
        "if (used + 1 >= capacity) break;  /* XM-TRUNC */", 1)
    return text.replace(
        "for (int k = 0; k < length; ++k) line[used++] = piece[k];",
        "for (int k = 0; k < length && used < capacity - 1; ++k) line[used++] = piece[k];", 1)
PY

echo "fault ring host regression: PASS"

# What is pinned here is the ring's own behaviour. Which site raises which code, and
# whether every declared code is reachable at all, is scripts/tests/test_fault_codes.py;
# the device half of the contract (a photographed screen that decodes) is CMD:FAULTS
# and CMD:CRASHTEST in a bench build.
