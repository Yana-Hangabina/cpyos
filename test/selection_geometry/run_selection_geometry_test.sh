#!/bin/sh
# Host regression for the pure selection geometry shared by the EPUB and PDF
# readers (src/activities/reader/SelectionGeometry.h). No device, no renderer:
# it pins the hit-test, range membership, drag-clamp, tap-extend, handle
# geometry and handle hit rules with ASan/UBSan.
#
# Two mutation runs prove the checks are load-bearing: letting the start handle
# cross the end, or mirroring the horizontal handle offsets, must turn the suite
# red.
#
#   bash test/selection_geometry/run_selection_geometry_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="activities/reader/SelectionGeometry.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/selection-geometry-test.XXXXXX")
# A mutation harness must never be able to edit the tree it is testing.  One
# real accident: a perl -0pi line whose file argument was lost made perl treat
# the NEXT shell words -- including $REPO_ROOT/src/... -- as files to rewrite in
# place, and the pristine header came back with a mutation marker in it.  So the
# digest is taken here and re-checked before this script may print PASS.
PRISTINE_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  inc_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$inc_dir" -I"$REPO_ROOT/src" \
    -o "$out_dir/geometry_test" "$SCRIPT_DIR/SelectionGeometryTest.cpp"
  "$out_dir/geometry_test"
}

echo "== selection geometry: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src"

echo "== selection geometry: mutation (start handle may cross the end) =="
mkdir -p "$BUILD_DIR/mut1/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut1/$HEADER_REL"
perl -0pi -e 's/if \(before\(r\.endLine, r\.endWord, line, word\)\) return false;/if (false) return false;/' \
  "$BUILD_DIR/mut1/$HEADER_REL"
if run_suite "$BUILD_DIR/mut1run" "$BUILD_DIR/mut1" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (drag clamp)" >&2
  exit 1
fi
echo "mutation detected"

echo "== selection geometry: mutation (horizontal handle offsets mirrored) =="
mkdir -p "$BUILD_DIR/mut2/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut2/$HEADER_REL"
perl -0pi -e 's/hx\[0\] = ends\.firstX - k - 4;\n    hx\[1\] = ends\.lastX \+ ends\.lastW \+ 4;/hx[0] = ends.firstX + 4;\n    hx[1] = ends.lastX - k - 4;/' \
  "$BUILD_DIR/mut2/$HEADER_REL"
if run_suite "$BUILD_DIR/mut2run" "$BUILD_DIR/mut2" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (handle geometry)" >&2
  exit 1
fi
echo "mutation detected"

# --- key widening (selection_geom::indexOf / stepWord) -------------------------
# Each mutation re-creates a mistake a reader would make silently: a selection that
# jumps to the top of the page, or a vertical step that forgets the column.  The
# marker check refuses a patch that landed somewhere else, and cmp refuses one that
# did not land at all.
echo "== selection geometry: mutation (a missing focus word defaults to the page start) =="
mkdir -p "$BUILD_DIR/mut3/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut3/$HEADER_REL"
perl -0pi -e 's/if \(words\[i\]\.line == line && words\[i\]\.word == word\) return static_cast<int>\(i\);\n  \}\n  return -1;/if (words[i].line == line \&\& words[i].word == word) return static_cast<int>(i);\n  }\n  return 0; \/\* XM-INDEXOF *\//' "$BUILD_DIR/mut3/$HEADER_REL"
grep -qF '/* XM-INDEXOF */' "$BUILD_DIR/mut3/$HEADER_REL" || { echo "FAIL: mutation did not land (indexOf)" >&2; exit 1; }
if run_suite "$BUILD_DIR/mut3run" "$BUILD_DIR/mut3" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (indexOf missing word)" >&2
  exit 1
fi
echo "mutation detected"

echo "== selection geometry: mutation (a vertical step forgets the column) =="
mkdir -p "$BUILD_DIR/mut4/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut4/$HEADER_REL"
perl -0pi -e 's/if \(distance < bestDistance\) \{/if (best < 0) { \/\* XM-FIRSTWORD *\//' "$BUILD_DIR/mut4/$HEADER_REL"
grep -qF '/* XM-FIRSTWORD */' "$BUILD_DIR/mut4/$HEADER_REL" || { echo "FAIL: mutation did not land (stepWord column)" >&2; exit 1; }
if run_suite "$BUILD_DIR/mut4run" "$BUILD_DIR/mut4" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (stepWord keeps the column)" >&2
  exit 1
fi
echo "mutation detected"

echo "== selection geometry: mutation (Right at the last word wraps to the first) =="
mkdir -p "$BUILD_DIR/mut5/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut5/$HEADER_REL"
perl -0pi -e 's/return next >= words\.size\(\) \? -1 : static_cast<int>\(next\);/return next >= words.size() ? 0 : static_cast<int>(next); \/\* XM-WRAP *\//' "$BUILD_DIR/mut5/$HEADER_REL"
grep -qF '/* XM-WRAP */' "$BUILD_DIR/mut5/$HEADER_REL" || { echo "FAIL: mutation did not land (stepWord right edge)" >&2; exit 1; }
if run_suite "$BUILD_DIR/mut5run" "$BUILD_DIR/mut5" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (stepWord right edge)" >&2
  exit 1
fi
echo "mutation detected"

NOW_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
if [ "$NOW_SHA" != "$PRISTINE_SHA" ]; then
  echo "FAIL: a mutation modified the pristine header under src/ -- the tree is dirty" >&2
  git -C "$REPO_ROOT" diff --stat -- "$HEADER_REL" >&2 || true
  exit 1
fi
echo "selection geometry host regression: PASS"
