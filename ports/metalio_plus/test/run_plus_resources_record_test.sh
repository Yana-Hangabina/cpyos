#!/usr/bin/env bash
# Host regression for the CloudZao installed-resource record parser.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/plus_resources_record_test"
cc="${CXX:-c++}"
"$cc" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/components/cpyos_app" -o "$out" "$here/test/plus_resources_record_host_test.cpp"
"$out"
