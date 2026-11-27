#!/bin/sh
# Host regression for the engine-buffer paragraph splitter (src/pdf/PdfTextSplit.h).
# Mutation: dropping the `i == n` end-of-buffer case must lose the last
# paragraph of every page and go red.
#
#   bash test/pdf_text_split/run_pdf_text_split_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="pdf/PdfTextSplit.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pdf-text-split-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  inc_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$inc_dir" -I"$REPO_ROOT/src" \
    -o "$out_dir/split_test" "$SCRIPT_DIR/PdfTextSplitTest.cpp"
  "$out_dir/split_test"
}

echo "== pdf text split: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src"

echo "== pdf text split: mutation (end-of-buffer case removed) =="
mkdir -p "$BUILD_DIR/mut1/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut1/$HEADER_REL"
perl -0pi -e 's/if \(i == n \|\| \(i \+ 1 < n/if \(false || (i + 1 < n/' \
  "$BUILD_DIR/mut1/$HEADER_REL"
if run_suite "$BUILD_DIR/mut1run" "$BUILD_DIR/mut1" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (end-of-buffer tail)" >&2
  exit 1
fi
echo "mutation detected"

echo "== pdf text split: mutation (lone trailing newline treated as a break) =="
mkdir -p "$BUILD_DIR/mut2/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut2/$HEADER_REL"
perl -0pi -e 's/\(i \+ 1 < n && buffer\[i\] == .\\n. && buffer\[i \+ 1\] == .\\n.\)/(buffer[i] == 0x0a)/' \
  "$BUILD_DIR/mut2/$HEADER_REL"
if run_suite "$BUILD_DIR/mut2run" "$BUILD_DIR/mut2" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (single-newline split)" >&2
  exit 1
fi
echo "mutation detected"

echo "pdf text split host regression: PASS"
