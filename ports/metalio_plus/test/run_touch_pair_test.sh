#!/usr/bin/env bash
# Pinch math (two-point classification) under ASan/UBSan.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/../components/cpyos_ui/compat" \
    -o /tmp/touch_pair_test "$here/test_touch_pair.cpp"
/tmp/touch_pair_test
