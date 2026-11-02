#!/usr/bin/env bash
# Host check for the OPDS 2.0 JSON parser. Compiles the real parser against the
# real streaming JSON tokenizer; no device, no network, no account.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/opds_json_test.$$"

${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -Werror \
  -I "$ROOT/lib/OpdsParser" \
  -I "$ROOT/freeink-sdk/libs/network/JsonSax/include" \
  -o "$OUT" \
  "$ROOT/test/opds_json/opds_json_test.cpp" \
  "$ROOT/lib/OpdsParser/OpdsJsonParser.cpp" \
  "$ROOT/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp"

"$OUT"
rm -f "$OUT"
