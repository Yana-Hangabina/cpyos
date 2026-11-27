#!/bin/sh
# Host regression for the PDF annotation re-anchoring passes (src/activities/reader/
# QuoteAnchor.h).  Pure std C++, no HAL, so it runs here.
#
# Every mutation targets one design decision.  A mutation that "did not apply" is a hard
# failure, never a skip: a silently-missed perl expression once made this script print
# "mutation detected" for a rule it was not testing at all.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="activities/reader/QuoteAnchor.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/quote-anchor-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  inc_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$inc_dir" -o "$out_dir/quote_test" "$SCRIPT_DIR/QuoteAnchorTest.cpp"
  "$out_dir/quote_test"
}

mutate() {
  name=$1
  shift
  dir="$BUILD_DIR/mut_$(printf '%s' "$name" | tr -cd '[:alnum:]')"
  mkdir -p "$dir/$(dirname "$HEADER_REL")"
  cp "$REPO_ROOT/src/$HEADER_REL" "$dir/$HEADER_REL"
  "$@" "$dir/$HEADER_REL"
  if cmp -s "$dir/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL"; then
    echo "FAIL: '$name' did not modify the header; the run below would pass vacuously" >&2
    exit 1
  fi
  if run_suite "$dir-run" "$dir" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($name)" >&2
    exit 1
  fi
  echo "mutation detected ($name)"
}

# 1. Without the whitespace-insensitive pass, a highlight taken before 智能合并断行 is
#    switched on stops matching: the merged CJK line has no space where the quote does.
drop_relaxed() {
  perl -0pi -e 's/  const size_t hit = bestOccurrence\(dense, denseNeedle, denseLine, denseNeedle\.size\(\), hintLine\);/  const size_t hit = std::string::npos;/' "$1"
}
# 2. The loosened pass must never beat an exact match.
swap_passes() {
  perl -0pi -e 's/  const size_t at = bestOccurrence\(page, needle, lineOfChar, needle\.size\(\), hintLine\);/  const size_t at = std::string::npos;/' "$1"
}
# 3. Lines must be joined WITH a separator, or a quote spanning two of them cannot match.
drop_separator() {
  perl -0pi -e "s/if \\(!page\\.empty\\(\\) && page\\.back\\(\\) != ' '\\) \\{/if (false) {/g" "$1"
}
# 4/5. Each side of the Latin word guard, independently.
drop_left_guard() {
  perl -0pi -e 's/if \(!cutsLatinWord\(before, needle\.front\(\)\) && !cutsLatinWord\(needle\.back\(\), after\)\)/if (!cutsLatinWord(needle.back(), after))/' "$1"
}
drop_right_guard() {
  perl -0pi -e 's/if \(!cutsLatinWord\(before, needle\.front\(\)\) && !cutsLatinWord\(needle\.back\(\), after\)\)/if (!cutsLatinWord(before, needle.front()))/' "$1"
}
# 6. The guard must not apply to CJK, which has no word separators to line up with.
guard_cjk() {
  perl -0pi -e "s/  return \\(c >= '0' && c <= '9'\\) \\|\\| \\(c >= 'a' && c <= 'z'\\) \\|\\| \\(c >= 'A' && c <= 'Z'\\);/  return true;/" "$1"
}
# 7. Boundary lines carry the exact span.  Painting them full width is precisely the
#    EPUB-parity regression the span resolution exists to prevent.
flatten_head_span() {
  perl -0pi -e 's/    startByte = clampCol\(span\.firstCol, 0\);\n    endByte = width;/    startByte = 0;\n    endByte = width;/' "$1"
}
# 8. "Matched, but on another line" must stay distinct from "no match at all": collapsing
#    them lets the caller fall through to stale stored offsets and underline a line the
#    quote never covered.
collapse_elsewhere() {
  perl -0pi -e 's/  if \(line < span\.firstLine \|\| line >= span\.lastLine\) return kElsewhere;/  if (line < span.firstLine || line >= span.lastLine) return kAbsent;/' "$1"
}

echo "== quote anchor: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src"

mutate "relaxed pass removed" drop_relaxed
mutate "relaxed pass preferred over exact" swap_passes
mutate "line separator not inserted" drop_separator
mutate "left word-boundary guard dropped" drop_left_guard
mutate "right word-boundary guard dropped" drop_right_guard
mutate "word guard applied to CJK too" guard_cjk
mutate "first boundary line painted full width" flatten_head_span
mutate "matched-elsewhere collapsed into absent" collapse_elsewhere

# 9. The stored line is the only thing that separates two copies of the same sentence. If the
#    ranking is dropped, a highlight silently moves to the first occurrence on the page --
#    a different passage than the one the reader marked.
drop_hint_ranking() {
  perl -0pi -e 's/if \(best == std::string::npos \|\| distance < bestDistance\)/if (best == std::string::npos)/' "$1"
}
# 10. And if it ranks the WRONG way, the underline jumps to the farthest copy.
invert_hint_ranking() {
  perl -0pi -e 's/if \(best == std::string::npos \|\| distance < bestDistance\)/if (best == std::string::npos || distance > bestDistance)/' "$1"
}


mutate "hint ranking dropped" drop_hint_ranking
mutate "hint ranking inverted" invert_hint_ranking

echo "quote anchor host regression: PASS"
