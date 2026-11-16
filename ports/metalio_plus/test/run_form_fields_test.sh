#!/usr/bin/env bash
# Host test for the form/JSON helpers (ASan + UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/metalio_form_fields_test"
clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$here/main" "$here/test/test_form_fields.cpp" -o "$out"
"$out"
