#!/usr/bin/env bash
# Host regression for the recursive dictionary viewer's plain-definition
# tokenizer (src/util/PlainTextTokens.h). Pure code, no device, no network.
set -euo pipefail

cd "$(dirname "$0")/../.."

OUT="$(mktemp -t dict_selection.XXXXXX)"
trap 'rm -f "$OUT"' EXIT

g++ -std=c++20 -O1 -Wall -Wextra -Werror \
  -I lib/Utf8 -I src \
  -o "$OUT" test/dictionary_selection/DictionarySelectionTest.cpp lib/Utf8/Utf8.cpp

"$OUT"
