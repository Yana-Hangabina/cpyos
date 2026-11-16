#!/usr/bin/env bash
# Host regression for the BLE HID report decoder (ASan + UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="$(mktemp -t ble_hid_decode.XXXXXX)"
trap 'rm -f "$bin"' EXIT
g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I "$here/../components/cpyos_ui" -I "$here/../components/cpyos_ui/compat" -I "$here/../../../src" \
    -o "$bin" "$here/test_ble_hid_decode.cpp"
"$bin"
