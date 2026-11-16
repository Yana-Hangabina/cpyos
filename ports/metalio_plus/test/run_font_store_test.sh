#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/../main" -o /tmp/font_store_test "$here/test_font_store.cpp"
/tmp/font_store_test
