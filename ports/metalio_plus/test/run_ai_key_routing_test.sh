#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/plus_ai_key_routing_test"
"${CXX:-c++}" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/main" -o "$out" \
    "$here/test/ai_key_routing_host_test.cpp" "$here/main/plus_ai_key_routing.cpp"
"$out"
