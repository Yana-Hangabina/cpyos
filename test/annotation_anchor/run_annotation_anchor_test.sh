#!/bin/sh
# Host regression for the annotation-anchor interval logic (src/activities/reader/
# AnnotationAnchor.h), used by the PDF reader to re-select an existing highlight.
# The mutation runs prove the two subtle rules are load-bearing: the half-open
# edge test, and widening only to words of the SAME annotation.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="activities/reader/AnnotationAnchor.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/annotation-anchor-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  inc_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$inc_dir" -o "$out_dir/anchor_test" "$SCRIPT_DIR/AnnotationAnchorTest.cpp"
  "$out_dir/anchor_test"
}

echo "== annotation anchor: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src"

echo "== mutation (interval becomes closed on both ends) =="
mkdir -p "$BUILD_DIR/mut1/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut1/$HEADER_REL"
perl -0pi -e 's/return static_cast<int>\(wordStart\) < spanEnd && static_cast<int>\(wordEnd\) > spanStart;/return static_cast<int>(wordStart) <= spanEnd \&\& static_cast<int>(wordEnd) >= spanStart;/' \
  "$BUILD_DIR/mut1/$HEADER_REL"
if cmp -s "$BUILD_DIR/mut1/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL"; then
  echo "FAIL: mutation did not apply" >&2
  exit 1
fi
if run_suite "$BUILD_DIR/mut1run" "$BUILD_DIR/mut1" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (closed interval)" >&2
  exit 1
fi
echo "mutation detected"

echo "== mutation (widening ignores which annotation covers a word) =="
mkdir -p "$BUILD_DIR/mut2/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut2/$HEADER_REL"
perl -0pi -e 's/if \(coverOf\(i\) != annotation\) continue;//' "$BUILD_DIR/mut2/$HEADER_REL"
if cmp -s "$BUILD_DIR/mut2/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL"; then
  echo "FAIL: mutation did not apply" >&2
  exit 1
fi
if run_suite "$BUILD_DIR/mut2run" "$BUILD_DIR/mut2" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (annotation identity ignored)" >&2
  exit 1
fi
echo "mutation detected"

echo "annotation anchor host regression: PASS"
