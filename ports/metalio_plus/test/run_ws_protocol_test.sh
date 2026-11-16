#!/usr/bin/env bash
# Host test for the WebSocket upload START parser (ASan + UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/metalio_ws_protocol_test"
clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$here/main" "$here/test/test_ws_protocol.cpp" -o "$out"
"$out"
