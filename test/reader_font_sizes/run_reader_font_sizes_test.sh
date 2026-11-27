#!/usr/bin/env bash
# Host regression for the curated reader-size ladder and the fixed-.cpfont
# fallback merge (no device, no font files, no network).
set -euo pipefail
cd "$(dirname "$0")/../.."

CXX="${CXX:-g++}"
OUT="$(mktemp -t reader_font_sizes.XXXXXX)"
trap 'rm -f "$OUT"' EXIT

"$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
  -I src -I lib/EpdFont \
  -o "$OUT" test/reader_font_sizes/ReaderFontSizesTest.cpp
"$OUT"
