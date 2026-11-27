#!/bin/sh
# Host regression for the PSRAM-stack flash guard.
#
#   lib/Memory/StackSafety.cpp  -- the verdict (which stacks may touch internal flash)
#
# It compiles the REAL source against two ESP doubles in stubs/ (a movable stack
# pointer and the real ESP32-S3 PSRAM/DRAM address windows), so what is under test
# is the shipped decision, not a re-typed copy of it.
#
# Why a guard test exists at all: the 0.8.0 -> 0.8.2 field crash was
#   assert failed: ... (esp_task_stack_is_sane_cache_disabled())
# i.e. an internal-flash read from the render task, whose stack lives in PSRAM
# because CFF needs a 16 KB frame.  The cache disable takes the caller's own
# stack away, so the failure is a panic, not an error return -- and any new read
# site that forgets to ask this question re-creates it.
#
# Each mutation removes one decision and must turn the suite red.  Two vacuous
# green paths are refused outright (both have bitten this tree before):
#   * a patch that does not modify the file at all            -- caught by cmp
#   * a patch that modifies the wrong place in the file       -- caught by marker
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
CPP_REL="lib/Memory/StackSafety.cpp"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/stack-safety-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out=$1
  root=$2
  mkdir -p "$(dirname "$out")"
  # The guard records a fault code when it refuses, so the real FaultLog is part
  # of this link. It is compiled with -DSIMULATOR (plain statics, a settable clock)
  # so the harness needs no RTC, esp_timer or esp_system doubles.
  "$CXX" -std=c++20 -O1 -Wall -Wextra -DSIMULATOR -fsanitize=address,undefined \
    -I"$REPO_ROOT/lib/Diagnostics" \
    -c "$REPO_ROOT/lib/Diagnostics/FaultLog.cpp" -o "$out.faultlog.o"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$root/lib/Memory" -I"$SCRIPT_DIR/stubs" -I"$REPO_ROOT/lib/Diagnostics" \
    -o "$out" "$SCRIPT_DIR/StackSafetyTest.cpp" "$root/$CPP_REL" "$out.faultlog.o"
  "$out"
}

mutate() {
  label=$1
  marker=$2
  patch=$3
  dir="$BUILD_DIR/m_$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  mkdir -p "$dir/lib/Memory"
  cp "$REPO_ROOT/$CPP_REL" "$dir/$CPP_REL"
  "$patch" "$dir/$CPP_REL"
  if cmp -s "$dir/$CPP_REL" "$REPO_ROOT/$CPP_REL"; then
    echo "FAIL: '$label' did not modify $CPP_REL; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/$CPP_REL"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker')" >&2
    exit 1
  fi
  if run_suite "$dir-run/test" "$dir" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($label)" >&2
    exit 1
  fi
  echo "mutation detected ($label)"
}

# --- the mutations -----------------------------------------------------------------------

# 1. The two answers swapped: every PSRAM stack is told it may erase flash, and
#    every internal-RAM stack is refused.  This is the exact defect that crashed.
swap_verdict() {
  perl -0pi -e 's/\? FlashGuard::RefusedPsramStack\n\s*: FlashGuard::Allowed;/? FlashGuard::Allowed\n    : FlashGuard::RefusedPsramStack; \/* XM-SWAP *\//' "$1"
}
# 2. The bench seam stops restoring the hardware verdict, so CMD:CRASHTEST would
#    report a degradation that never actually happened on the guarded path.
ignore_clear() {
  perl -0pi -e 's/if \(forced >= 0\) return/if (forced > 0) return  \/* XM-NOCLEAR *\//' "$1"
}
# 3. The guard becomes a formality (the pre-0.8.2 world): nothing is ever refused.
always_allow() {
  perl -0pi -e 's/FlashGuard decide\(\) \{/FlashGuard decide() { return FlashGuard::Allowed;  \/* XM-ALWAYS *\//' "$1"
}
# 4. The verdict stays correct but is never written down. A refusal that only
#    exists on serial is exactly the support blind spot the site code removes, so
#    the recording is as much a decision as the answer is.
no_record() {
  perl -0pi -e 's/if \(verdict != FlashGuard::Allowed\) fault::raise\(site, detail\);/(void)site; (void)detail;  \/* XM-NORAISE *\//' "$1"
}

echo "== stack safety: verdict =="
run_suite "$BUILD_DIR/normal/test" "$REPO_ROOT"

mutate "verdict branches swapped"        '/* XM-SWAP */'   swap_verdict
mutate "override cannot be cleared"      '/* XM-NOCLEAR */' ignore_clear
mutate "guard always allows"             '/* XM-ALWAYS */'  always_allow
mutate "refusal is not recorded"         '/* XM-NORAISE */' no_record

echo "stack safety host regression: PASS"

# On hardware the same two directions are one serial command, in a bench build
# (CROSSPOINT_SERIAL_BENCH=1), which runs the *product* read path:
#   CMD:CRASHTEST guarded
#   [ERR] CORE: carrier probe refused: this task stack is in PSRAM
#   CRASHTEST guarded: degraded to fallback=-1, no crash
# The line pair is the contract: the read is refused, the caller reports
# "unknown", and the device stays up.  Deleting a guard from a call site is
# caught by scripts/tests/test_internal_flash_guard.py, not here.
