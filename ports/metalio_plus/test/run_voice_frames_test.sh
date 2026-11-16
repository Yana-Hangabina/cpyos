#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/plus_voice_frames_test"
"${CXX:-c++}" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/components/cpyos_app" -o "$out" \
    "$here/test/voice_frames_host_test.cpp"
"$out"
