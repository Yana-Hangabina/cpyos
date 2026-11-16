#!/usr/bin/env bash
# Host regression for the byte-mode QR version table (ECC-L capacities).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/plus_qr_version_test"
cc="${CXX:-c++}"
"$cc" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/../../src/util" -o "$out" "$here/test/qr_version_host_test.cpp"
"$out"
