#!/usr/bin/env bash
# Host regression for layout milestone M5b (per-word font runs).
#
# Bare g++ runner: the cmake suites need network FetchContent for gtest. It compiles the
# production parser/layout sources unchanged against the M2 font-aware renderer stub, then
# rebuilds twice with a broken implementation (measurement that ignores the word's slot, and
# an arena that never reserves the slot region) and requires this test to fail, so the checks
# cannot pass vacuously.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${TMPDIR:-/tmp}/layout_m5b_build"
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
  "$ROOT/test/layout_m5b/LayoutM5bTest.cpp"
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

echo "== building layout M5b host test"
"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m5b_test" \
  "${SOURCES[@]}" \
  "$ROOT/lib/Epub/Epub/ParsedText.cpp" \
  "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

echo "== running layout M5b host test"
"$BUILD/layout_m5b_test"

# Mutation 1: every word is measured with the line face, so an inline run lays out exactly
# like body text. The position assertions must go red.
echo "== mutation 1: measurement ignores the word's slot, the test must go red"
mkdir -p "$BUILD/mut_measure"
python3 - "$ROOT/lib/Epub/Epub/ParsedText.cpp" "$BUILD/mut_measure/ParsedText.cpp" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
needle = """int ParsedText::fontIdAt(const size_t index, const int fontId) const {
  if (index >= wordFontSlots.size()) return fontId;"""
assert text.count(needle) == 1, "mutation anchor missing"
text = text.replace(needle, """int ParsedText::fontIdAt(const size_t index, const int fontId) const {
  return fontId;  // MUTATION: ignore the word's slot
  if (index >= wordFontSlots.size()) return fontId;""")
open(dst, "w", encoding="utf-8").write(text)
PY

"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m5b_noslot_mutant" \
  "${SOURCES[@]}" \
  "$BUILD/mut_measure/ParsedText.cpp" \
  "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

if "$BUILD/layout_m5b_noslot_mutant" >/dev/null 2>&1; then
  echo "FAIL: mutation survived (an inline run lays out like body text)" >&2
  exit 1
fi
echo "== mutation 1 detected"

# Mutation 2: the arena never reserves the slot region, so a mixed line is short by one byte
# per word. The arena assertion must go red.
echo "== mutation 2: the arena drops the slot region, the test must go red"
mkdir -p "$BUILD/mut_arena"
python3 - "$ROOT/lib/Epub/Epub/blocks/TextBlock.cpp" "$BUILD/mut_arena/TextBlock.cpp" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
needle = """  if (hasFontSlots) {
    size += wordCount;
  }"""
assert text.count(needle) == 1, "mutation anchor missing"
text = text.replace(needle, """  if (hasFontSlots) {
    size += 0;  // MUTATION: no slot region
  }""")
open(dst, "w", encoding="utf-8").write(text)
PY

"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m5b_noarena_mutant" \
  "$ROOT/test/layout_m5b/LayoutM5bTest.cpp" \
  "$ROOT/test/chapter_html_slim_parser/UpstreamParserLinkStubs.cpp" \
  "$ROOT/lib/Epub/Epub/Page.cpp" \
  "$BUILD/mut_arena/TextBlock.cpp" \
  "$ROOT/lib/MiniBidi/BidiUtils.cpp" \
  "$ROOT/lib/Epub/Epub/css/CssParser.cpp" \
  "$ROOT/lib/Epub/Epub/CjkBreaks.cpp" \
  "$ROOT/lib/Epub/Epub/WordStore.cpp" \
  "$ROOT/lib/Epub/Epub/converters/ImageDimsProbe.cpp" \
  "$ROOT/lib/Utf8/Utf8.cpp" \
  "$ROOT/lib/Epub/Epub/ParsedText.cpp" \
  "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

if "$BUILD/layout_m5b_noarena_mutant" >/dev/null 2>&1; then
  echo "FAIL: mutation survived (the slot region costs no arena bytes)" >&2
  exit 1
fi
echo "== mutation 2 detected"
