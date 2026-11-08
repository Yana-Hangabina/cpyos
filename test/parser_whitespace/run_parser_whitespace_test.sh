#!/usr/bin/env bash
# Host regression for `white-space` / <pre> in the real chapter parser (layout M1).
#
# Links the repository's chapter-parser test stubs (no cmake, no gtest): the TextBlock stub
# records every laid-out token, so the token stream proves whether runs were collapsed or kept.
# A mutation that ignores preservation must turn the suite red.
#
#   bash test/parser_whitespace/run_parser_whitespace_test.sh
set -euo pipefail

cd "$(dirname "$0")/../.."

CXX=${CXX:-g++}
OUT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/parser-ws-host.XXXXXX")
trap 'rm -rf "$OUT_DIR"' EXIT HUP INT TERM

compile() {
  local impl=$1
  local binary=$2
  "$CXX" -std=c++20 -O1 -w -fsanitize=address,undefined \
    -I test/chapter_html_slim_parser/stubs \
    -I lib -I lib/Epub -I lib/Epub/Epub -I lib/Epub/Epub/parsers -I lib/Epub/Epub/css \
    -I lib/EpdFont -I lib/Utf8 -I lib/Memory -I lib/Serialization -I lib/MiniBidi \
    -I lib/FsHelpers -I lib/XmlParserUtils -I src \
    -o "$binary" test/parser_whitespace/ParserWhitespaceHostTest.cpp \
    test/chapter_html_slim_parser/ParserLinkStubs.cpp \
    "$impl" lib/Epub/Epub/css/CssParser.cpp lib/Epub/Epub/ParsedText.cpp lib/Epub/Epub/CjkBreaks.cpp \
    lib/Epub/Epub/Section.cpp lib/Epub/Epub/WordStore.cpp lib/Epub/Epub/converters/ImageDimsProbe.cpp \
    lib/Utf8/Utf8.cpp -lexpat
}

echo "== parser_whitespace: normal =="
compile lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp "$OUT_DIR/normal"
"$OUT_DIR/normal"

echo "== parser_whitespace: mutation (preservation ignored) =="
cp lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp "$OUT_DIR/mut_parser.cpp"
perl -0pi -e 's/if \(self->effectivePreserveWhitespace\) \{/if (false) {/' "$OUT_DIR/mut_parser.cpp"
if compile "$OUT_DIR/mut_parser.cpp" "$OUT_DIR/mutant" >"$OUT_DIR/mutant.log" 2>&1; then
  if "$OUT_DIR/mutant" >>"$OUT_DIR/mutant.log" 2>&1; then
    echo "FAIL: mutation was not detected (preservation ignored)" >&2
    exit 1
  fi
fi
grep -q "FAIL:" "$OUT_DIR/mutant.log" || {
  echo "FAIL: mutation run did not report a failing check" >&2
  cat "$OUT_DIR/mutant.log" >&2
  exit 1
}

echo "parser_whitespace: ok (normal + mutation detected)"
