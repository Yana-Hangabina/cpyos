#!/usr/bin/env bash
# Host test for the SD file-transfer path rules (ASan + UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="$(mktemp -t transfer_paths.XXXXXX)"
trap 'rm -f "$bin"' EXIT
g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/../main" -o "$bin" "$here/test_transfer_paths.cpp"
"$bin"
