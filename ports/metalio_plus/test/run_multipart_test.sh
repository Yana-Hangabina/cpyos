#!/usr/bin/env bash
# Host test for the streaming multipart parser (ASan + UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/metalio_multipart_test"
clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$here/main" "$here/test/test_multipart.cpp" -o "$out"
"$out"
