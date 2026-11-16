#!/usr/bin/env bash
# Host test for the 1-bit framebuffer -> BMP encoder (ASan + UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/metalio_bmp_export_test"
clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$here/main" "$here/test/test_bmp_export.cpp" -o "$out"
"$out"
