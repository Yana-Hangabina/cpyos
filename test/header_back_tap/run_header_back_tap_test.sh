#!/usr/bin/env bash
# Host regression for the padded header Back hit area (HeaderBackTapTarget.h).
# Pure header, no device, no network.
set -euo pipefail

cd "$(dirname "$0")/../.."

OUT="$(mktemp -t header_back_tap.XXXXXX)"
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++20 -O1 -Wall -Wextra -Werror -I src \
  -o "$OUT" test/header_back_tap/HeaderBackTapTargetTest.cpp

"$OUT"
