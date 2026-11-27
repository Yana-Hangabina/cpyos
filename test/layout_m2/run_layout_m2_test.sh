#!/usr/bin/env bash
# Host regression for layout milestone M2 (block-level CSS font-size).
#
# Bare g++ runner: the cmake suites need network FetchContent for gtest, and this
# test only needs the real parser/layout sources plus a font-aware renderer stub.
# It compiles the production sources unchanged, then rebuilds the parser with CSS
# font-size ignored and requires the first binary to fail, so the assertions cannot
# pass vacuously.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${TMPDIR:-/tmp}/layout_m2_build"
CXX="${CXX:-g++}"
CC="${CC:-cc}"

INCLUDES=(
  -I"$ROOT/test/layout_m2/stubs"
  -I"$ROOT/test/chapter_html_slim_parser/stubs"
  -I"$ROOT/lib"
  -I"$ROOT/lib/Epub"
  -I"$ROOT/lib/Epub/Epub"
  -I"$ROOT/lib/Epub/Epub/parsers"
  -I"$ROOT/lib/Epub/Epub/css"
  -I"$ROOT/lib/Epub/Epub/blocks"
  -I"$ROOT/lib/EpdFont"
  -I"$ROOT/lib/Utf8"
  -I"$ROOT/lib/Memory"
  -I"$ROOT/lib/Serialization"
  -I"$ROOT/lib/MiniBidi"
  -I"$ROOT/lib/FsHelpers"
  -I"$ROOT/lib/XmlParserUtils"
  -I"$ROOT/lib/Logging"
  -I"$ROOT/src"
)

SOURCES=(
  "$ROOT/test/layout_m2/LayoutM2Test.cpp"
  "$ROOT/test/chapter_html_slim_parser/UpstreamParserLinkStubs.cpp"
  "$ROOT/lib/Epub/Epub/Page.cpp"
  "$ROOT/lib/Epub/Epub/blocks/TextBlock.cpp"
  "$ROOT/lib/MiniBidi/BidiUtils.cpp"
  "$ROOT/lib/Epub/Epub/css/CssParser.cpp"
  "$ROOT/lib/Epub/Epub/ParsedText.cpp"
  "$ROOT/lib/Epub/Epub/CjkBreaks.cpp"
  "$ROOT/lib/Epub/Epub/WordStore.cpp"
  "$ROOT/lib/Epub/Epub/converters/ImageDimsProbe.cpp"
  "$ROOT/lib/Utf8/Utf8.cpp"
)

mkdir -p "$BUILD"

# minibidi is C with extern "C" declarations in BidiUtils.cpp; compile it as C.
"$CC" -std=c11 -O1 -c "$ROOT/lib/MiniBidi/minibidi.c" -o "$BUILD/minibidi.o"

echo "== building layout M2 host test"
"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m2_test" \
  "${SOURCES[@]}" \
  "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

echo "== running layout M2 host test"
"$BUILD/layout_m2_test"

echo "== mutation: ignore CSS font-size, the test must go red"
mkdir -p "$BUILD/mut"
python3 - "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" "$BUILD/mut/ChapterHtmlSlimParser.cpp" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
# The copy lives outside the tree, so the relative fontIds include has to go through -Isrc.
text = text.replace('#include "../../../../src/fontIds.h"', "#include <fontIds.h>")
needle = "blockStyle.fontScale = std::clamp(scale, MIN_BLOCK_FONT_SCALE, MAX_BLOCK_FONT_SCALE);"
assert text.count(needle) == 1, "mutation anchor missing"
text = text.replace(needle, "blockStyle.fontScale = 1.0f;  // MUTATION: ignore CSS font-size")
open(dst, "w", encoding="utf-8").write(text)
PY

"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m2_mutant" \
  "${SOURCES[@]}" \
  "$BUILD/mut/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

if "$BUILD/layout_m2_mutant" >/dev/null 2>&1; then
  echo "FAIL: mutation survived (CSS font-size can be ignored without the test noticing)" >&2
  exit 1
fi
echo "== mutation detected"
