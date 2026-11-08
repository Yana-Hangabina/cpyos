#!/usr/bin/env bash
# Host check for the BLE reader radio policy. Header-only, no device, no BLE stack.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/ble_remote_policy_test.$$"

${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -Werror \
  -I "$ROOT/src" \
  -o "$OUT" \
  "$ROOT/test/ble_remote/ble_remote_policy_test.cpp"

"$OUT"
rm -f "$OUT"
