#!/usr/bin/env bash
# Host regression for CSS `font-weight/font-style: inherit` (upstream 1bd323dd).
#
# Compiles the real lib/Epub/Epub/css/CssParser.cpp against the CSS test's file-backed
# HAL stubs: no cmake, no firmware toolchain. Covers value interpretation, the cascade,
# inline styles and the CSS cache wire round-trip (value 2 must decode).
set -euo pipefail

cd "$(dirname "$0")/../.."

OUT=${TMPDIR:-/tmp}/css_inherit_host_test

g++ -std=c++20 -O1 -Wall -Wextra \
  -I test/css_parser/stubs -I lib/Epub/Epub/css -I lib/Memory \
  -o "$OUT" test/css_inherit/CssInheritHostTest.cpp lib/Epub/Epub/css/CssParser.cpp
"$OUT"
