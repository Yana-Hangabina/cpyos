#!/usr/bin/env bash
# Host regression for layout milestone M6 (::first-letter drop caps).
#
# Bare g++ runner: the cmake suites need network FetchContent for gtest. It compiles the
# production parser/layout sources unchanged against the M2 renderer stub, then rebuilds
# twice with a broken implementation (a layout that ignores the cap reservation, and a
# parser that never emits the element) and requires this test to fail, so the checks
# cannot pass vacuously.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${TMPDIR:-/tmp}/layout_m6_build"
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
  "$ROOT/test/layout_m6/LayoutM6Test.cpp"
  "$ROOT/test/chapter_html_slim_parser/UpstreamParserLinkStubs.cpp"
  "$ROOT/lib/Epub/Epub/Page.cpp"
  "$ROOT/lib/Epub/Epub/blocks/TextBlock.cpp"
  "$ROOT/lib/MiniBidi/BidiUtils.cpp"
  "$ROOT/lib/Epub/Epub/css/CssParser.cpp"
  "$ROOT/lib/Epub/Epub/CjkBreaks.cpp"
  "$ROOT/lib/Epub/Epub/WordStore.cpp"
  "$ROOT/lib/Epub/Epub/converters/ImageDimsProbe.cpp"
  "$ROOT/lib/Utf8/Utf8.cpp"
)

mkdir -p "$BUILD"

# minibidi is C with extern "C" declarations in BidiUtils.cpp; compile it as C.
"$CC" -std=c11 -O1 -c "$ROOT/lib/MiniBidi/minibidi.c" -o "$BUILD/minibidi.o"

echo "== building layout M6 host test"
"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m6_test" \
  "${SOURCES[@]}" \
  "$ROOT/lib/Epub/Epub/ParsedText.cpp" \
  "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

echo "== running layout M6 host test"
"$BUILD/layout_m6_test"

# Mutation 1: the layout drops the cap reservation, so the following lines stay flush
# with the margin and would run under the glyph. The indent assertions must go red.
echo "== mutation 1: the reservation is ignored, the test must go red"
mkdir -p "$BUILD/mut_layout"
python3 - "$ROOT/lib/Epub/Epub/ParsedText.cpp" "$BUILD/mut_layout/ParsedText.cpp" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
needle = "  dropCapActive = dropCapLines > 0 && dropCapWidth > 0 && !blockStyle.isRtl;"
assert text.count(needle) == 1, "mutation anchor missing"
text = text.replace(needle, "  dropCapActive = false;  // MUTATION: ignore the drop cap")
open(dst, "w", encoding="utf-8").write(text)
PY

"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m6_nolayout_mutant" \
  "${SOURCES[@]}" \
  "$BUILD/mut_layout/ParsedText.cpp" \
  "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

if "$BUILD/layout_m6_nolayout_mutant" >/dev/null 2>&1; then
  echo "FAIL: mutation survived (capped lines are not indented)" >&2
  exit 1
fi
echo "== mutation 1 detected"

# Mutation 2: the parser never places the element, so nothing is drawn where the
# reservation left room. The emission assertions must go red.
echo "== mutation 2: the cap is never emitted, the test must go red"
mkdir -p "$BUILD/mut_emit"
python3 - "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" "$BUILD/mut_emit/ChapterHtmlSlimParser.cpp" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
# The copy lives outside the tree, so the relative fontIds include goes through -Isrc.
text = text.replace('#include "../../../../src/fontIds.h"', "#include <fontIds.h>")
needle = "  if (dropCap.emitPending) {"
assert text.count(needle) == 1, "mutation anchor missing"
text = text.replace(needle, "  if (false) {  // MUTATION: never emit the cap")
open(dst, "w", encoding="utf-8").write(text)
PY

"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m6_noemit_mutant" \
  "${SOURCES[@]}" \
  "$ROOT/lib/Epub/Epub/ParsedText.cpp" \
  "$BUILD/mut_emit/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

if "$BUILD/layout_m6_noemit_mutant" >/dev/null 2>&1; then
  echo "FAIL: mutation survived (a drop-cap paragraph renders without its glyph)" >&2
  exit 1
fi
echo "== mutation 2 detected"
