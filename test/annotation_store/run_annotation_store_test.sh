#!/bin/sh
# Host regression for the shared annotation identity rule (annotations::normalize,
# lib/Epub/Epub/AnnotationStore.cpp).  Pure std C++, no HAL, so it runs here.
#
# The mutation run is the point: if reverting the key to "range only" still passed,
# nothing here would actually be pinning the fix for the PDF line-index collision.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SRC_DIR="$REPO_ROOT/lib/Epub/Epub"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/annotation-store-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  src_dir=$1
  out=$2
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$src_dir" -o "$out" "$SCRIPT_DIR/AnnotationStoreTest.cpp" "$src_dir/AnnotationStore.cpp"
  "$out"
}

echo "== annotation store: normal =="
run_suite "$SRC_DIR" "$BUILD_DIR/store_test"

echo "== mutation (dedupe key reverts to range-only, the shipped bug) =="
mkdir -p "$BUILD_DIR/mut1"
cp "$SRC_DIR/AnnotationStore.h" "$SRC_DIR/AnnotationStore.cpp" "$BUILD_DIR/mut1/"
perl -0pi -e 's/\s*list\[i \+ 1\]\.quote == list\[i\]\.quote//' "$BUILD_DIR/mut1/AnnotationStore.cpp"
if cmp -s "$BUILD_DIR/mut1/AnnotationStore.cpp" "$SRC_DIR/AnnotationStore.cpp"; then
  echo "FAIL: mutation did not apply" >&2
  exit 1
fi
if run_suite "$BUILD_DIR/mut1" "$BUILD_DIR/mut1_test" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (quote removed from the identity key)" >&2
  exit 1
fi
echo "mutation detected"

echo "== mutation (clamp gate widened so degenerate ranges clamp too) =="
mkdir -p "$BUILD_DIR/mut2"
cp "$SRC_DIR/AnnotationStore.h" "$SRC_DIR/AnnotationStore.cpp" "$BUILD_DIR/mut2/"
perl -0pi -e 's/if \(a\.endOffset > a\.startOffset\) \{/if (a.endOffset >= a.startOffset) {/' \
  "$BUILD_DIR/mut2/AnnotationStore.cpp"
if cmp -s "$BUILD_DIR/mut2/AnnotationStore.cpp" "$SRC_DIR/AnnotationStore.cpp"; then
  echo "FAIL: mutation did not apply" >&2
  exit 1
fi
if run_suite "$BUILD_DIR/mut2" "$BUILD_DIR/mut2_test" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (clamp gate is not being asserted)" >&2
  exit 1
fi
echo "mutation detected"

echo "annotation store host regression: PASS"
