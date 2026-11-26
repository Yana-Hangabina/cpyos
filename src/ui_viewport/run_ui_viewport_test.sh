#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out="${TMPDIR:-/tmp}/cpyos_ui_viewport_test"
g++ -std=c++20 -O1 -Wall -Wextra -I lib/GfxRenderer -o "$out" src/UiViewportTest.cpp
"$out"
