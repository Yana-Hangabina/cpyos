#!/usr/bin/env bash
# Host regression for the CSS cascade absorbed from upstream 0305aaa6 (layout M1):
# compound (#id / .a.b) selectors, descendant/child combinators, specificity ordering,
# ::first-letter storage, the newly parsed properties and the preserved-whitespace helper.
#
# Compiles the real lib/Epub/Epub/css/CssParser.cpp against the CSS parser's file-backed HAL
# stubs, so no cmake/gtest/firmware toolchain is needed. Two mutation builds must fail:
#   * dropping the selector-context specificity term
#   * refusing descendant matches
set -euo pipefail

cd "$(dirname "$0")/../.."

CXX=${CXX:-g++}
OUT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/css-cascade-host.XXXXXX")
trap 'rm -rf "$OUT_DIR"' EXIT HUP INT TERM

compile() {
  local impl=$1
  local binary=$2
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I test/css_parser/stubs -I lib/Epub/Epub/css -I lib/Epub/Epub/parsers -I lib/Memory \
    -o "$binary" test/css_cascade/CssCascadeHostTest.cpp "$impl"
}

mutate() {
  local label=$1
  local perl_expr=$2
  local impl="$OUT_DIR/$label.cpp"
  cp lib/Epub/Epub/css/CssParser.cpp "$impl"
  perl -0pi -e "$perl_expr" "$impl"
  if compile "$impl" "$OUT_DIR/$label" >"$OUT_DIR/$label.log" 2>&1; then
    if "$OUT_DIR/$label" >>"$OUT_DIR/$label.log" 2>&1; then
      echo "FAIL: mutation was not detected ($label)" >&2
      cat "$OUT_DIR/$label.log" >&2
      exit 1
    fi
  fi
  grep -q "^FAIL:" "$OUT_DIR/$label.log" || {
    echo "FAIL: mutation run did not report a failing check ($label)" >&2
    cat "$OUT_DIR/$label.log" >&2
    exit 1
  }
}

echo "== css_cascade: normal =="
compile lib/Epub/Epub/css/CssParser.cpp "$OUT_DIR/normal"
"$OUT_DIR/normal"

echo "== css_cascade: mutation (context specificity dropped) =="
mutate mut_specificity 's/addMatch\(specificity \+ contextSpecificity\(context\)/addMatch(specificity/'

echo "== css_cascade: mutation (descendant matching disabled) =="
mutate mut_descendant 's/bool ancestorsMatch\(std::string_view context, const CssAncestor\* ancestors, size_t candidates\) \{/bool ancestorsMatch(std::string_view context, const CssAncestor* ancestors, size_t candidates) {\n  return false;/'

echo "css_cascade: ok (normal + 2 mutations detected)"
