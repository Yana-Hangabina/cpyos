#!/usr/bin/env bash
# Host regression for layout milestone M3 (page breaks, orphans/widows, keep-with-next).
#
# Bare g++ runner: the cmake suites need network FetchContent for gtest. It compiles
# the production policy header, parser and layout sources unchanged, then rebuilds
# twice with a broken implementation and requires this test to fail, so the checks
# cannot pass vacuously.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${TMPDIR:-/tmp}/layout_m3_build"
CXX="${CXX:-g++}"
CC="${CC:-cc}"

# The M2 stub is a font-aware renderer double with a 16 px section line height; M3
# lays out one face per block, so it needs no other renderer behaviour.
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
  "$ROOT/test/layout_m3/LayoutM3Test.cpp"
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

echo "== building layout M3 host test"
"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m3_test" \
  "${SOURCES[@]}" \
  "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

echo "== running layout M3 host test"
"$BUILD/layout_m3_test"

# Mutation 1: the policy stops protecting the paragraph's first line. The pure
# policy checks in this binary must catch it.
echo "== mutation 1: policy no longer carries an orphan, the test must go red"
mkdir -p "$BUILD/mut_policy/Epub"
python3 - "$ROOT/lib/Epub/Epub/PageBreakPolicy.h" "$BUILD/mut_policy/Epub/PageBreakPolicy.h" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
needle = "    carry.paragraphLines = state.paragraphLinesOnPage;\n"
assert text.count(needle) == 1, "mutation anchor missing"
text = text.replace(needle, "    carry.paragraphLines = 0;  // MUTATION: ignore the orphan\n")
open(dst, "w", encoding="utf-8").write(text)
PY

"$CXX" -std=c++20 -O1 -g -I"$BUILD/mut_policy" "${INCLUDES[@]}" -o "$BUILD/layout_m3_policy_mutant" \
  "${SOURCES[@]}" \
  "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

if "$BUILD/layout_m3_policy_mutant" >/dev/null 2>&1; then
  echo "FAIL: mutation survived (a page may end with an orphan)" >&2
  exit 1
fi
echo "== mutation 1 detected"

# Mutation 2: the parser never carries lines, which must break the paragraph cases
# even though the policy itself is intact.
echo "== mutation 2: the parser ignores the carry decision, the test must go red"
mkdir -p "$BUILD/mut_parser"
python3 - "$ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp" "$BUILD/mut_parser/ChapterHtmlSlimParser.cpp" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
# The copy lives outside the tree, so the relative fontIds include has to go through -Isrc.
text = text.replace('#include "../../../../src/fontIds.h"', "#include <fontIds.h>")
needle = "  return page_break_policy::decide(state, [this](const size_t lines) { return canCarry(lines); });"
assert text.count(needle) == 1, "mutation anchor missing"
text = text.replace(needle, "  return {};  // MUTATION: ignore the carry decision")
open(dst, "w", encoding="utf-8").write(text)
PY

"$CXX" -std=c++20 -O1 -g "${INCLUDES[@]}" -o "$BUILD/layout_m3_parser_mutant" \
  "${SOURCES[@]}" \
  "$BUILD/mut_parser/ChapterHtmlSlimParser.cpp" \
  "$BUILD/minibidi.o" -lexpat

if "$BUILD/layout_m3_parser_mutant" >/dev/null 2>&1; then
  echo "FAIL: mutation survived (the page break can drop the carry and nothing notices)" >&2
  exit 1
fi
echo "== mutation 2 detected"
