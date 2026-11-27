#!/bin/sh
# Host regression for the PDF block-splitting rule (src/pdf/PdfLineJoin.h).
#
# Why a split is needed at all: MuPDF hands the extractor ONE text block for two
# adjacent hard-wrapped paragraphs (test/pdf_line_join/make_cjk_block_probe.py measures
# it), and the extractor glued the lines together unconditionally.  Once the newline is
# gone the app-side heuristic cannot recover the boundary, so the module has to put it
# back from line geometry.
#
# Why a refusal is equally part of the rule: applied naively the ragged test shredded four
# real pages of a shipped manual from 73 paragraphs to 89 -- code listings and space-only
# lines are ragged by nature.  Mutations 2, 3 and 5 remove each half of that defence and
# must turn the suite red; join_real_pages.py re-measures the real pages.
#
# Each mutation stamps a marker at its own target, and the runner refuses a patch that did
# not land (cmp) or landed somewhere unexpected (missing marker) -- both false greens
# happened while these harnesses were being written.  The pristine digest at the end refuses
# to print PASS if any mutation touched the tree under src/.
#
#   bash test/pdf_line_join/run_pdf_line_join_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="pdf/PdfLineJoin.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pdf-line-join-test.XXXXXX")
PRISTINE_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  include_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$include_dir" -o "$out_dir/join_test" "$SCRIPT_DIR/PdfLineJoinTest.cpp"
  "$out_dir/join_test"
}

# mutate <label> <marker> <perl expression>   (perl runs in slurp mode)
mutate() {
  label=$1
  marker=$2
  expr=$3
  dir="$BUILD_DIR/m$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  mkdir -p "$dir/pdf"
  cp "$REPO_ROOT/src/$HEADER_REL" "$dir/pdf/PdfLineJoin.h"
  perl -0pi -e "$expr" "$dir/pdf/PdfLineJoin.h"
  # shellcheck disable=SC2016  # the marker is literal text, not a command substitution
  if cmp -s "$dir/pdf/PdfLineJoin.h" "$REPO_ROOT/src/$HEADER_REL"; then
    echo "FAIL: '$label' did not modify the header; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/pdf/PdfLineJoin.h"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker')" >&2
    exit 1
  fi
  if run_suite "$dir-run" "$dir/pdf" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($label)" >&2
    exit 1
  fi
  echo "mutation detected ($label)"
}

echo "== pdf line join: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src/pdf"

# The unit fixtures above are hand-drawn boxes.  This one lays out real Chinese text with
# MuPDF, feeds the resulting geometry to the same `paragraphStarts()` the module calls, and
# compares against the paragraph counts the SCRIPT chose -- so both directions of failure
# (splitting a listing, fusing two wrapped paragraphs) show up as a non-zero exit instead of
# as a plausible-looking number.  Needs PyMuPDF; skipped, not silently passed, without it.
if python3 -c "import fitz" >/dev/null 2>&1; then
  echo "== pdf line join: CJK ground-truth census =="
  python3 "$SCRIPT_DIR/make_cjk_corpus.py"
else
  echo "SKIP: CJK census needs PyMuPDF (python3 -m pip install pymupdf)"
fi

# 1. Every shortfall splits: the boundary check has to stay exact, or a column whose last
#    line is one point short of 25% becomes two paragraphs.
mutate "ragged threshold set to zero" '/* JJ-RAGGED0 */' \
  's!constexpr int kRaggedPercentOfColumn = 25;!constexpr int kRaggedPercentOfColumn = 0; /* JJ-RAGGED0 */!'

# 2. Every block counts as a text column: this is the whole over-splitting defect, restored
#    through the ratio instead of through the code.
mutate "the text-column requirement is dropped" '/* JJ-NOCOLUMN */' \
  's!  if \(looksLikeTextColumn\(stats\)\) \{!  if (true) { /* JJ-NOCOLUMN */!'

# 3. A line counts as "full" whatever where it ends, so a listing reads as a column.
mutate "the full-line tolerance is widened to everything" '/* JJ-FULLTOL */' \
  's!constexpr int kFullTolerancePercent = 5;!constexpr int kFullTolerancePercent = 100; /* JJ-FULLTOL */!'

# 4. A normal line gap reads as a paragraph gap.
mutate "the gap multiple is lowered to one" '/* JJ-GAP1 */' \
  's!constexpr int kGapMultiple = 3;!constexpr int kGapMultiple = 1; /* JJ-GAP1 */!'

# 5. A space-only line becomes ink: its tiny box is then the widest shortfall in the block.
# Delimiter is ~ here, not !: the pattern itself contains three bangs, and s!...! would
# have ended the expression at the first one -- which perl reports as a syntax error in the
# *modifier* position, not as "no match", so it looks like a broken regex rather than a
# broken delimiter.
mutate "blank lines are counted as text" '/* JJ-BLANKINK */' \
  's~if \(!line.printable \|\| !usable\(line.box\)\) continue;~if (!usable(line.box)) continue; /* JJ-BLANKINK */~g'

# 6. A blank line no longer forces the next line to start a paragraph.
mutate "the blank-line break is removed" '/* JJ-NOBREAK */' \
  's!if \(previous >= 0\) breakPending = true;!if (false) breakPending = true; /* JJ-NOBREAK */!'

# 7. A box with no width becomes usable: it is then the most short-of-margin line in the
#    block, which is exactly how a degenerate span would shred a paragraph.
mutate "degenerate boxes are accepted as lines" '/* JJ-USEBOX */' \
  's!return box.x1 > box.x0 && box.y1 >= box.y0;!return box.x1 >= box.x0 && box.y1 >= box.y0; /* JJ-USEBOX */!'

# 8. A block narrower than a glyph is measured as a column.  Both halves have to go at
#    once -- measureBlock refuses to count full lines for a sub-glyph block, which alone
#    would keep the ratio honest -- because a 1 pt column whose lines differ by half a
#    point otherwise reads as a 50% shortfall and splits.
mutate "the hairline-column guard is removed" '/* JJ-HAIRLINE */' \
  's!if \(column <= 1.f\) return Decision::Glue;!if (false) return Decision::Glue; /* JJ-HAIRLINE */!; s!if \(column > 1.f\) \{!if (true) { /* JJ-HAIRLINE2 */!'

# 9. The reader's option stops reaching the engine: `off` behaves like `on`.  A switch that
#    cannot actually switch is worse than no switch, because the reader believes the menu.
mutate "the paragraph-split switch is ignored" '/* JJ-SWITCHOFF */' \
  's~  if \(!enabled\) \{~  if (false) { /* JJ-SWITCHOFF */~'  # ~ delimiter: the pattern contains a bang

NOW_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
if [ "$NOW_SHA" != "$PRISTINE_SHA" ]; then
  echo "FAIL: a mutation modified the pristine header under src/ -- the tree is dirty" >&2
  git -C "$REPO_ROOT" diff --stat -- "$HEADER_REL" >&2 || true
  exit 1
fi
echo "pdf line join host regression: PASS"
