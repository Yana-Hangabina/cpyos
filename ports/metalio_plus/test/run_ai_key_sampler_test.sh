#!/usr/bin/env bash
# Host regression for the AI/BOOT key short/long classifier.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/plus_ai_key_sampler_test"
cc="${CXX:-c++}"
"$cc" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/main" -o "$out" \
    "$here/test/ai_key_sampler_host_test.cpp" "$here/main/plus_ai_key_sampler.cpp"
"$out"
