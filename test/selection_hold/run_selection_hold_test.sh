#!/bin/sh
# Host regression for the shared press-and-hold gesture machine
#   src/activities/reader/SelectionHold.h
#
# Both readers used to carry their own copy of this state machine, and the copies
# had drifted: the PDF one cancelled a long press on any frame without a touch
# sample.  That is not a style difference -- the Read Pico panel stops reporting a
# stationary finger, so the same gesture worked in the EPUB reader and silently
# failed in the PDF one.  This suite pins the surviving behaviour, and mutation 1
# re-introduces the exact old PDF rule to prove the suite sees it.
#
# Each mutation stamps a marker at its own target, and the runner refuses a patch
# that did not land (cmp) or landed somewhere else (missing marker) -- both false
# greens happened while these harnesses were being written.
#
#   bash test/selection_hold/run_selection_hold_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="activities/reader/SelectionHold.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/selection-hold-test.XXXXXX")
# A mutation harness must never be able to edit the tree it is testing.  One
# real accident: a perl -0pi line whose file argument was lost made perl treat
# the NEXT shell words -- including $REPO_ROOT/src/... -- as files to rewrite in
# place, and the pristine header came back with a mutation marker in it.  So the
# digest is taken here and re-checked before this script may print PASS.
PRISTINE_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  include_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$include_dir" -o "$out_dir/hold_test" "$SCRIPT_DIR/SelectionHoldTest.cpp"
  "$out_dir/hold_test"
}

# mutate <label> <marker> <perl expression>   (perl runs in slurp mode)
mutate() {
  label=$1
  marker=$2
  expr=$3
  dir="$BUILD_DIR/m$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  mkdir -p "$dir/reader"
  cp "$REPO_ROOT/src/$HEADER_REL" "$dir/reader/SelectionHold.h"
  perl -0pi -e "$expr" "$dir/reader/SelectionHold.h"
  if cmp -s "$dir/reader/SelectionHold.h" "$REPO_ROOT/src/$HEADER_REL"; then
    echo "FAIL: '$label' did not modify the header; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/reader/SelectionHold.h"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker')" >&2
    exit 1
  fi
  if run_suite "$dir-run" "$dir/reader" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($label)" >&2
    exit 1
  fi
  echo "mutation detected ($label)"
}

echo "== selection hold: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src/activities/reader"

# 1. The original PDF behaviour: any frame without a sample ends the press.
mutate "one dropped sample cancels the press (the old PDF rule)" '/* XM-NOGAP */' \
  's!if \(static_cast<int32_t>\(now - lastHeldMs_\) < static_cast<int32_t>\(kHoldGapMs\)\) \{!if (false) { /* XM-NOGAP */!'

# 2. No fire from the last known point: a finger the panel stopped reporting
#    silently does nothing.
mutate "last-point firing removed" '/* XM-NOLAST */' \
  's!if \(static_cast<int32_t>\(now - startMs_\) >= static_cast<int32_t>\(kPressMs\)\) \{\n      tracking_ = false;\n      fireX_ = lastX_!if (false) { /* XM-NOLAST */\n      tracking_ = false;\n      fireX_ = lastX_!'

# 3. Off-by-one on the hold timer (both occurrences, so the marker is global).
mutate "hold timer needs one more millisecond than documented" '/* XM-OFFBY */' \
  's!>= static_cast<int32_t>\(kPressMs\)!> static_cast<int32_t>(kPressMs) /* XM-OFFBY */!g'

# 4. A box instead of a circle cancels a diagonal drag that is still inside slop.
mutate "slop becomes a per-axis box" '/* XM-BOX */' \
  's!if \(dx \* dx \+ dy \* dy > kSlopPx \* kSlopPx\) \{!if (dx > kSlopPx || dy > kSlopPx) { /* XM-BOX */!'

# 5. Measuring travel from the previous frame lets a slow creep never cancel.
mutate "slop measured from the previous frame" '/* XM-CREEP */' \
  's!const int dx = c\.x - startX_;!const int dx = c.x - lastX_; /* XM-CREEP */!'

# 6. The repaint gate shifts by one millisecond.
mutate "repaint gate boundary shifts" '/* XM-REPAINT */' \
  's!return static_cast<int32_t>\(now - last\) >= static_cast<int32_t>\(kDragRepaintMs\);!return static_cast<int32_t>(now - last) > static_cast<int32_t>(kDragRepaintMs) /* XM-REPAINT */;!'

# 7. A release edge that leaves tracking set resurrects the press afterwards.
mutate "release edge stops clearing tracking" '/* XM-STUCK */' \
  's!if \(c\.ended\) \{\n      tracking_ = false;!if (c.ended) { /* XM-STUCK */!'

# 8. The opening-press window is widened to nothing: the chip bar fires on the gesture that
#    opened it, which is the defect this guard exists for (and the one the PDF reader shipped
#    with while the EPUB reader had the fix).
mutate "the bar's opening-press window is removed" '/* XM-BARSWALLOW */' \
  's~return openedMs == 0 ||~return true || /* XM-BARSWALLOW */~'

NOW_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
if [ "$NOW_SHA" != "$PRISTINE_SHA" ]; then
  echo "FAIL: a mutation modified the pristine header under src/ -- the tree is dirty" >&2
  git -C "$REPO_ROOT" diff --stat -- "$HEADER_REL" >&2 || true
  exit 1
fi
echo "selection hold host regression: PASS"
