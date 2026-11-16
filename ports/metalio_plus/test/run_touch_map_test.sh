#!/usr/bin/env bash
# Host test for the portrait touch mapping (ASan + UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/metalio_touch_map_test"
clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$here/components/cpyos_ui" "$here/test/test_touch_map.cpp" -o "$out"
"$out"
