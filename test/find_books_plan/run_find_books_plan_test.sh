#!/usr/bin/env bash
# Find-books vertical plan: authored parity at 100%, fit-to-body above it.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
c++ -std=c++20 -O1 -Wall -Wextra -Werror -I src -o "$OUT/t" test/find_books_plan/FindBooksPlanTest.cpp
"$OUT/t"
