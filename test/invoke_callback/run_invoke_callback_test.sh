#!/bin/sh
# Host regression for the empty-callback guard.
#
#   lib/Callbacks/InvokeCallback.h    -- the invocation wrappers (the crash fix)
#   lib/Callbacks/InvokeCallback.cpp  -- the once-per-site report table
#
# Both files are compiled from the tree, so the shipped wrappers are under test.
# The firmware is built -fno-exceptions: an empty std::function still *throws*
# std::bad_function_call, no application frame has a landing pad, and the device
# dies at std::terminate -- the field report was a bare
#   abort() was called at PC 0x423569f3 on core 1
# with no backtrace.  So this suite checks two things: the call degrades, and the
# degradation stays visible (exactly one error line per site per boot).
#
# Mutations, each removing one decision, must turn the suite red.  A patch that
# does not modify the file, or modifies the wrong place, fails outright rather
# than reporting a vacuous green.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HDR_REL="lib/Callbacks/InvokeCallback.h"
CPP_REL="lib/Callbacks/InvokeCallback.cpp"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/invoke-callback-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

prepare_copy() {
  dir=$1
  mkdir -p "$dir/lib/Callbacks"
  cp "$REPO_ROOT/$HDR_REL" "$dir/$HDR_REL"
  cp "$REPO_ROOT/$CPP_REL" "$dir/$CPP_REL"
}

run_suite() {
  out=$1
  root=$2
  mkdir -p "$(dirname "$out")"
  # The owner records each skip in the fault ring, so the real FaultLog is part of
  # this link -- compiled with -DSIMULATOR for plain statics and a settable clock,
  # which keeps the harness free of RTC/esp_timer doubles.
  "$CXX" -std=c++20 -O1 -Wall -Wextra -DSIMULATOR -fsanitize=address,undefined \
    -I"$REPO_ROOT/lib/Diagnostics" \
    -c "$REPO_ROOT/lib/Diagnostics/FaultLog.cpp" -o "$out.faultlog.o"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$root/lib/Callbacks" -I"$SCRIPT_DIR/stubs" -I"$REPO_ROOT/lib/Diagnostics" \
    -o "$out" "$SCRIPT_DIR/InvokeCallbackTest.cpp" "$root/$CPP_REL" "$out.faultlog.o"
  "$out"
}

# run_mutation <label> <marker> <target-rel> <patch>
guard_and_run() {
  label=$1
  marker=$2
  target=$3
  patch=$4
  dir="$BUILD_DIR/m_$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  prepare_copy "$dir"
  "$patch" "$dir/$target"
  if cmp -s "$dir/$target" "$REPO_ROOT/$target"; then
    echo "FAIL: '$label' did not modify $target; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/$target"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker' in $target)" >&2
    exit 1
  fi
  if run_suite "$dir-run/test" "$dir" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($label)" >&2
    exit 1
  fi
  echo "mutation detected ($label)"
}

# --- the mutations -----------------------------------------------------------------------

# 1. The wrapper stops testing the function: back to bad_function_call -> abort.
drop_value_guard() {
  perl -0pi -e 's/  if \(!fn\) \{\n    noteEmpty\(tag\);\n    return fallback;/  if (false) {  \/* XM-FN *\n    noteEmpty(tag);\n    return fallback;/' "$1"
}
# 2. Same for the void path (ButtonNavigator, parser hooks).
drop_void_guard() {
  perl -0pi -e 's/  if \(!fn\) \{\n    noteEmpty\(tag\);\n    return;\n  \}/  if (false) {  \/* XM-VOID *\n    noteEmpty(tag);\n    return;\n  }/' "$1"
}
# 3. Report on every frame: a per-keypress log line buries every other message,
#    which is the failure mode the table exists to prevent.
always_report() {
  perl -0pi -e 's/if \(first\) reportEmpty\(tag\);/reportEmpty(tag);  \/* XM-ONCE *\//' "$1"
}
# 5. The skip is silent in the ring: the crash screen then cannot say that a
#    callback was missing, which is the whole reason the owner records it.
drop_record() {
  perl -0pi -e 's/fault::raise\(fault::Code::CallbackEmpty, static_cast<uint32_t>\(slot\)\);/(void)slot;  \/* XM-NORAISE *\//' "$1"
}
# 4. Write past the fixed table: the allocation-free bound is the only thing
#    keeping this safe on the render task.
drop_table_bound() {
  perl -0pi -e 's/if \(seen < kKnownTags\) \{/if (true) {  \/* XM-BOUND *\//' "$1"
}

echo "== invoke callback: wrappers and reporting =="
run_suite "$BUILD_DIR/normal/test" "$REPO_ROOT"

guard_and_run "value wrapper invokes an empty function" '/* XM-FN *'   "$HDR_REL" drop_value_guard
guard_and_run "void wrapper invokes an empty function"  '/* XM-VOID *' "$HDR_REL" drop_void_guard
guard_and_run "every skip logs again"                   '/* XM-ONCE */' "$CPP_REL" always_report
guard_and_run "the skip is not recorded"                '/* XM-NORAISE */' "$CPP_REL" drop_record
guard_and_run "report table bound removed"              '/* XM-BOUND */' "$CPP_REL" drop_table_bound

echo "invoke callback host regression: PASS"

# On hardware both directions are one bench command (CROSSPOINT_SERIAL_BENCH=1):
#   CMD:CRASHTEST func   -> an empty std::function<void(int)> on purpose; the
#                            guard logs `CB: empty callback skipped: crashtest.func`
#                            and the device stays up.  Without the guard the same
#                            command aborts at std::terminate -- the field crash.
#   CMD:CRASHTEST new    -> a 1 GiB operator new on purpose: the new_handler
#                            names the failure on the crash screen.
# Deleting a bare invocation is caught by
# scripts/tests/test_callback_invocation_guard.py, not here.
