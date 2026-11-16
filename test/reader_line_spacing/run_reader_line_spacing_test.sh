#!/usr/bin/env bash
# Host check for the reader line-spacing percentage override (header-only, no
# device, no Arduino headers).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/reader_line_spacing_test.$$"

${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -Werror \
  -I "$ROOT/src" \
  -o "$OUT" \
  "$ROOT/test/reader_line_spacing/ReaderLineSpacingTest.cpp"

"$OUT"
rm -f "$OUT"
