#!/bin/sh
# Host regression for the PDF engine update manifest parser.
#   bash test/pdf_engine_manifest/run_pdf_engine_manifest_test.sh
# Mutation run: removing the https-only guard must turn the suite red.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="pdf/PdfEngineManifest.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pdf-engine-manifest-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  inc_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$inc_dir" -o "$out_dir/test" "$SCRIPT_DIR/PdfEngineManifestTest.cpp"
  "$out_dir/test"
}

echo "== pdf engine manifest: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src"

echo "== mutation (https-only guard removed) =="
mkdir -p "$BUILD_DIR/mut/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut/$HEADER_REL"
# Drop the TLS-only check so an http:// URL would parse.
perl -0pi -e 's{if \(url\.rfind\("https://", 0\) != 0\)[^\n]*\n}{}' "$BUILD_DIR/mut/$HEADER_REL"
if run_suite "$BUILD_DIR/mutrun" "$BUILD_DIR/mut" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (https-only guard)" >&2
  exit 1
fi
echo "mutation detected"

echo "pdf engine manifest host regression: PASS"
