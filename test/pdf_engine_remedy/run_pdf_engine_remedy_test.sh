#!/bin/sh
# Host regression for pdf_module::refusalIsDownloadable() and the reader's card encoding
# (src/pdf/PdfModuleRuntime.h).  See PdfEngineRemedyTest.cpp for why this mapping is a
# user-facing decision rather than a detail: after an app-only update Incompatible is the
# NORMAL state, and NoPsram/MapFailed are the ones a download cannot touch.
#
# Marker discipline as elsewhere in this repo's host suites: a patch that does not modify
# the file, or that modifies the wrong place, fails the run instead of reporting a green.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
TARGET_REL="src/pdf/PdfModuleRuntime.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pdf-engine-remedy-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  root=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$root/src" -o "$out_dir/remedy_test" "$SCRIPT_DIR/PdfEngineRemedyTest.cpp"
  "$out_dir/remedy_test"
}

mutate() {
  label=$1
  marker=$2
  patch=$3
  dir="$BUILD_DIR/m_$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  mkdir -p "$dir/$(dirname "$TARGET_REL")"
  cp "$REPO_ROOT/$TARGET_REL" "$dir/$TARGET_REL"
  "$patch" "$dir/$TARGET_REL"
  if cmp -s "$dir/$TARGET_REL" "$REPO_ROOT/$TARGET_REL"; then
    echo "FAIL: '$label' did not modify the header; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/$TARGET_REL"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker')" >&2
    exit 1
  fi
  if run_suite "$dir-run" "$dir" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($label)" >&2
    exit 1
  fi
  echo "mutation detected ($label)"
}

# The post-OTA case removed: every upgraded device gets "engine is for another firmware"
# with no way out from the book it was trying to read.
drop_wrong_app() {
  perl -0pi -e 's/return refusal == Refusal::Incompatible \|\| /return \/* RM-WRONGAPP *\/ refusal == Refusal::NoPartition || /' "$1"
}
# A memory-state failure turned into a download offer: 1.9 MB fetched, still broken, and the
# real remedy (power cycle) never named.
offer_download_for_memory_state() {
  perl -0pi -e 's/return refusal == Refusal::Incompatible \|\| /return refusal == Refusal::MapFailed || refusal == Refusal::NoPsram || refusal == Refusal::Incompatible || \/* RM-MEMSTATE *\/ /' "$1"
}
# Off-by-one in the card slot decode: the card would name the reason of the PREVIOUS state.
drop_decode_offset() {
  perl -0pi -e 's{return static_cast<Refusal>\(stored == 0 \? 0 : stored - 1\);}{return static_cast<Refusal>(stored == 0 ? 0 : stored);  /* RM-DECODE */}' "$1"
}
# Dropping the +1 makes Refusal::NoPartition encode to 0, i.e. "no problem", so the reader
# silently stops showing the card at all.
drop_encode_offset() {
  perl -0pi -e 's{return static_cast<uint8_t>\(refusal\) \+ 1;}{return static_cast<uint8_t>(refusal);  /* RM-ENCODE */}' "$1"
}

echo "== pdf engine remedy: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT"

mutate "post-OTA refusal not offered a download" '/* RM-WRONGAPP */' drop_wrong_app
mutate "memory-state refusal offered a download" '/* RM-MEMSTATE */' offer_download_for_memory_state
mutate "card slot decoded without the +1 offset" '/* RM-DECODE */' drop_decode_offset
mutate "card slot encoded without the +1 offset" '/* RM-ENCODE */' drop_encode_offset

echo "pdf engine remedy host regression: PASS"
