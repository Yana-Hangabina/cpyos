#!/usr/bin/env bash
# Host regression for the custom HTTP header line parser (upstream 65e3f9d0).
#
# Compiles lib/HttpHeader/HttpHeader.cpp directly -- the code is pure (no HAL,
# no Arduino), so this runs without cmake or the firmware toolchain.
# Also runs a mutation step: if the formatter stops emitting ": " the round-trip
# assertion must fail, which proves the assertions are load-bearing.
set -euo pipefail

cd "$(dirname "$0")/../.."

INCLUDE=lib/HttpHeader
OUT=${TMPDIR:-/tmp}/http_header_host_test

g++ -std=c++20 -O1 -Wall -Wextra -I "$INCLUDE" -I lib/AsciiText -o "$OUT" \
  test/http_header/HttpHeaderHostTest.cpp "$INCLUDE/HttpHeader.cpp"
"$OUT"

MUT=${TMPDIR:-/tmp}/http_header_mut
rm -rf "$MUT"
mkdir -p "$MUT"
cp "$INCLUDE/HttpHeader.h" "$MUT/HttpHeader.h"
python3 - "$INCLUDE/HttpHeader.cpp" "$MUT/HttpHeader.cpp" <<'PY'
import sys
src = open(sys.argv[1]).read()
needle = 'return header.name + ": " + (maskValue ? "******" : header.value);'
if needle not in src:
    sys.exit("mutation needle missing: the formatter changed shape")
open(sys.argv[2], "w").write(src.replace(needle, 'return header.name + "=" + (maskValue ? "******" : header.value);'))
PY
g++ -std=c++20 -O1 -I "$MUT" -I lib/AsciiText -o "${MUT}/test" test/http_header/HttpHeaderHostTest.cpp "$MUT/HttpHeader.cpp"
if "${MUT}/test" > /dev/null 2>&1; then
  echo "FAIL: a broken formatter did not fail the test (assertions are weak)"
  exit 1
fi
echo "PASS: formatter mutation detected"
