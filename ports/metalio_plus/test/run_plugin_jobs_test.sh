#!/usr/bin/env bash
# Host test for the browser plugin job table (ASan + UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/metalio_plugin_jobs_test"
clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$here/main" "$here/test/plugin_jobs_host_test.cpp" -o "$out"
"$out"
