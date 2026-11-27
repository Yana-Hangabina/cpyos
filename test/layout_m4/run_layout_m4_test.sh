#!/usr/bin/env bash
# Host regression for layout milestone M4b (CSS border/shade boxes on the page).
#
# Bare g++ runner: the cmake suites need network FetchContent for gtest. It compiles the
# production parser/layout sources unchanged against the M2 renderer stub, then rebuilds
# twice with a broken implementation and requires this test to fail, so the checks cannot
# pass vacuously.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${TMPDIR:-/tmp}/layout_m4_build"
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
  "$ROOT/test/layout_m4/LayoutM4Test.cpp"
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

echo "== building layout M4b host test"
"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m4_test" \
  "${SOURCES[@]}" \
  "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

echo "== running layout M4b host test"
"$BUILD/layout_m4_test"

# Mutation 1: the parser stops emitting boxes at all. Every box assertion must go red.
echo "== mutation 1: no box is ever emitted, the test must go red"
mkdir -p "$BUILD/mut_open"
python3 - "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" "$BUILD/mut_open/ChapterHtmlSlimParser.cpp" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
# The copy lives outside the tree, so the relative fontIds include goes through -Isrc.
text = text.replace('#include "../../../../src/fontIds.h"', "#include <fontIds.h>")
needle = "  if (borderBox.element == nullptr && !verticalText && layoutParagraph != nullptr && line->getBlockStyle().hasBox) {"
assert text.count(needle) == 1, "mutation anchor missing"
text = text.replace(needle, "  if (false) {  // MUTATION: never emit a box")
open(dst, "w", encoding="utf-8").write(text)
PY

"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m4_open_mutant" \
  "${SOURCES[@]}" \
  "$BUILD/mut_open/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

if "$BUILD/layout_m4_open_mutant" >/dev/null 2>&1; then
  echo "FAIL: mutation survived (decorated blocks render without a box)" >&2
  exit 1
fi
echo "== mutation 1 detected"

# Mutation 2: a continuation keeps the top edge, so a box cut by a page break reads as
# two separate boxes.
echo "== mutation 2: the cut top edge is not dropped, the test must go red"
mkdir -p "$BUILD/mut_cut"
python3 - "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" "$BUILD/mut_cut/ChapterHtmlSlimParser.cpp" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
text = text.replace('#include "../../../../src/fontIds.h"', "#include <fontIds.h>")
needle = "      borderBoxContinues ? 0 : blockStyle.borderTop, blockStyle.borderRight, blockStyle.borderBottom,"
assert text.count(needle) == 1, "mutation anchor missing"
text = text.replace(needle, "      blockStyle.borderTop, blockStyle.borderRight, blockStyle.borderBottom,")
open(dst, "w", encoding="utf-8").write(text)
PY

"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m4_cut_mutant" \
  "${SOURCES[@]}" \
  "$BUILD/mut_cut/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

if "$BUILD/layout_m4_cut_mutant" >/dev/null 2>&1; then
  echo "FAIL: mutation survived (a split box still draws both horizontal edges)" >&2
  exit 1
fi
echo "== mutation 2 detected"
