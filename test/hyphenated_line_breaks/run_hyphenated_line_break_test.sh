#!/usr/bin/env bash
# Host regression for the hyphenated line breaker (ParsedText).
#
# Upstream cb2a4c78 fixed the same class of bug: the greedy hyphenation path
# measured lines without the ruby overhang or kinsoku hanging, so a paragraph
# wrapped differently depending on whether hyphenation was on, and ruby could be
# pushed past the page edge. CPYOS's DP breaker already applied both rules, so the
# test asserts the two paths agree, plus the two ruby cases that used to differ.
#
# Compiles the REAL lib/Epub/Epub/ParsedText.cpp with the chapter-parser stubs
# (no font, framebuffer, SD card or gtest). Run from anywhere:
#   bash test/hyphenated_line_breaks/run_hyphenated_line_break_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
CXX=${CXX:-c++}
CC=${CC:-cc}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hyph-break.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

STUBS="$REPO_ROOT/test/chapter_html_slim_parser/stubs $REPO_ROOT/test/chapter_html_slim_parser/upstream_stubs"
INCLUDES=""
for dir in $STUBS; do INCLUDES="$INCLUDES -I$dir"; done
INCLUDES="$INCLUDES -I$REPO_ROOT/lib -I$REPO_ROOT/lib/Epub -I$REPO_ROOT/lib/Epub/Epub \
  -I$REPO_ROOT/lib/EpdFont -I$REPO_ROOT/lib/Utf8 -I$REPO_ROOT/lib/Memory \
  -I$REPO_ROOT/lib/Serialization -I$REPO_ROOT/lib/MiniBidi -I$REPO_ROOT/lib/FsHelpers"
FLAGS="-std=c++20 -O1 -Wall -Wextra -Werror -DCROSSPOINT_EMBED_HYPHENATION"
# -Werror is off for the stubs' own unused parameters.
LIB_FLAGS="-std=c++20 -O1 -DCROSSPOINT_EMBED_HYPHENATION"

# sanitizers when the toolchain has them; UBSan must abort (a mutant that reads a
# stale break index would otherwise "recover" and pass)
if printf 'int main(){}\n' | "$CXX" -x c++ -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$BUILD_DIR/probe" - 2>/dev/null; then
  LIB_FLAGS="$LIB_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
fi

# minibidi is C, and must keep C linkage for BidiUtils' extern "C" symbols.
"$CC" -c -o "$BUILD_DIR/minibidi.o" -I"$REPO_ROOT/lib/MiniBidi" "$REPO_ROOT/lib/MiniBidi/minibidi.c"

# shellcheck disable=SC2086
build_and_run() {
  local src="$1" exe="$2" out
  # shellcheck disable=SC2086
  "$CXX" $LIB_FLAGS $INCLUDES -o "$exe" \
    "$REPO_ROOT/test/hyphenated_line_breaks/HyphenatedLineBreakTest.cpp" \
    "$src" \
    "$REPO_ROOT/lib/Epub/Epub/CjkBreaks.cpp" \
    "$REPO_ROOT/lib/Epub/Epub/WordStore.cpp" \
    "$REPO_ROOT/lib/Epub/Epub/blocks/TextBlock.cpp" \
    "$REPO_ROOT/lib/Epub/Epub/hyphenation/Hyphenator.cpp" \
    "$REPO_ROOT/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp" \
    "$REPO_ROOT/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp" \
    "$REPO_ROOT/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp" \
    "$REPO_ROOT/lib/MiniBidi/BidiUtils.cpp" \
    "$BUILD_DIR/minibidi.o" \
    "$REPO_ROOT/lib/Utf8/Utf8.cpp"
  "$exe"
}

echo "== real source"
build_and_run "$REPO_ROOT/lib/Epub/Epub/ParsedText.cpp" "$BUILD_DIR/real"

# Mutations: each drops one half of the ruby reservation and MUST go red, which is
# what proves the assertions above are tied to the code under test.
mutate() {
  local name="$1" from="$2" to="$3"
  local dir="$BUILD_DIR/$name"
  mkdir -p "$dir"
  FROM="$from" TO="$to" python3 - "$REPO_ROOT/lib/Epub/Epub/ParsedText.cpp" "$dir/ParsedText.cpp" <<'PY'
import os, sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding='utf-8').read()
needle, replacement = os.environ['FROM'], os.environ['TO']
if needle not in text:
    sys.exit('mutation needle not found: ' + needle)
open(dst, 'w', encoding='utf-8').write(text.replace(needle, replacement, 1))
PY
  if build_and_run "$dir/ParsedText.cpp" "$BUILD_DIR/$name.bin" > "$BUILD_DIR/$name.log" 2>&1; then
    echo "MUTANT SURVIVED: $name" >&2
    cat "$BUILD_DIR/$name.log" >&2
    exit 1
  fi
  echo "ok   mutant killed: $name"
}

echo "== mutations"
mutate no_ruby_start_offset \
  'int lineWidth = calculateRubyExtraStartOffset(lineStart, wordWidths.size(), renderer, fontId);' \
  'int lineWidth = 0;  // MUTANT'
mutate no_ruby_end_offset \
  'const int extraEndOffset = calculateRubyExtraEndOffset(lineStart, currentIndex, renderer, fontId);' \
  'const int extraEndOffset = 0;  // MUTANT'

echo "all checks passed"
