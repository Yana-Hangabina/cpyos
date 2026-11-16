#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/plus_wav_header_test"
"${CXX:-c++}" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/components/cpyos_app" -o "$out" "$here/test/wav_header_host_test.cpp"
"$out"
