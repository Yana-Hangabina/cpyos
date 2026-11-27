#!/bin/sh
# Host regression for src/activities/settings/PdfUpdateWatchdog.h -- the rule that
# decides when a PDF engine download is dead.
#
# Both mistakes this suite pins were made on device, in the same function, and
# neither one produced a compiler warning:
#   * the baseline started at 0, so "no progress" was measured from power-on and
#     the first body-stage tick cancelled a transfer 733 ms after it began;
#   * the manifest stage's ~50 s (TLS 1.2 retry on a phone hotspot) was charged to
#     the body stage, so even a healthy line looked stalled before it started.
# A wrong bound here is user-visible as "engine update always fails on my network",
# and a too-long bound is a spinner that never ends -- so both windows are asserted
# on each side, and the wrap is asserted because millis() really does wrap.
#
#   bash test/pdf_update_watchdog/run_pdf_update_watchdog_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="activities/settings/PdfUpdateWatchdog.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pdf-update-watchdog-test.XXXXXX")
PRISTINE_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  include_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$include_dir" -o "$out_dir/wd_test" "$SCRIPT_DIR/PdfUpdateWatchdogTest.cpp"
  "$out_dir/wd_test"
}

echo "== pdf update watchdog: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src/activities/settings"

# mutate <label> <marker> <perl expression>
mutate() {
  label=$1
  marker=$2
  expr=$3
  dir="$BUILD_DIR/m$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  mkdir -p "$dir/settings"
  cp "$REPO_ROOT/src/$HEADER_REL" "$dir/settings/PdfUpdateWatchdog.h"
  perl -0pi -e "$expr" "$dir/settings/PdfUpdateWatchdog.h"
  if cmp -s "$dir/settings/PdfUpdateWatchdog.h" "$REPO_ROOT/src/$HEADER_REL"; then
    echo "FAIL: '$label' did not modify the header; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/settings/PdfUpdateWatchdog.h"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker')" >&2
    exit 1
  fi
  if run_suite "$dir-run" "$dir/settings" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($label)" >&2
    exit 1
  fi
  echo "mutation detected ($label)"
}

# 1. The bug as shipped: treat "no baseline yet" as "baseline was power-on".
mutate "an unset baseline is read as zero" '/* WD-NOBASE */' \
  's/baselineMs == kNoBaseline/false \&\& baselineMs == kNoBaseline/'

# 2. Collapsing the two windows into one: a first byte then expires at 45 s, which
#    is exactly what a cold handshake on a slow AP does not deserve.
mutate "first byte gets the mid-stream bound" '/* WD-LIMIT */' \
  's/downloaded == 0 \? kFirstByteMs : kMidStreamMs/kMidStreamMs/'

# 3. A "safer-looking" clamp: `now < baseline` is the wrap straddle, and clamping it
#    to zero hides a genuine stall forever.
mutate "elapsed clamps instead of wrapping" '/* WD-MODULAR */' \
  's/decision.elapsedMs = nowMs - baselineMs;/decision.elapsedMs = nowMs > baselineMs ? nowMs - baselineMs : 0u;/'

# 4. Forgetting to re-base when the body stage begins: the manifest's time charges
#    to the download again.
mutate "stage entry does not re-base" '/* WD-ENTRY */' \
  's/if \(previousStage != StallStage::Body\) return nowMs;/if (previousStage != StallStage::Body) return currentBaseline;/'

if [ "$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')" != "$PRISTINE_SHA" ]; then
  echo "FAIL: the pristine header changed under the mutation harness" >&2
  exit 1
fi

echo "pdf update watchdog: normal suite and 4 mutations all behaved"
