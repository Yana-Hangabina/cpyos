#!/bin/sh
# Host regression for the PDF line-merge heuristic (src/pdf/PdfLineMerge.h).
# Pure text, ASan/UBSan. Mutation runs prove the rules are load-bearing:
# disabling the sentence-terminal stop, or the de-hyphenation, must go red.
#
#   bash test/pdf_line_merge/run_pdf_line_merge_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="pdf/PdfLineMerge.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pdf-line-merge-test.XXXXXX")
# A mutation harness must never be able to edit the tree it is testing.  One
# real accident: a perl -0pi line whose file argument was lost made perl treat
# the NEXT shell words -- including $REPO_ROOT/src/... -- as files to rewrite in
# place, and the pristine header came back with a mutation marker in it.  So the
# digest is taken here and re-checked before this script may print PASS.
PRISTINE_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  inc_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$inc_dir" -I"$REPO_ROOT/src" -I"$REPO_ROOT/lib/Utf8" \
    -o "$out_dir/merge_test" "$SCRIPT_DIR/PdfLineMergeTest.cpp" "$REPO_ROOT/lib/Utf8/Utf8.cpp"
  "$out_dir/merge_test"
}

echo "== pdf line merge: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src"

echo "== pdf line merge: mutation (sentence-terminal stop disabled) =="
mkdir -p "$BUILD_DIR/mut1/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut1/$HEADER_REL"
perl -0pi -e 's/const bool prevTerminal = endsSentence\(prev\);/const bool prevTerminal = false;/' \
  "$BUILD_DIR/mut1/$HEADER_REL"
if run_suite "$BUILD_DIR/mut1run" "$BUILD_DIR/mut1" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (terminal stop)" >&2
  exit 1
fi
echo "mutation detected"

# Two mutations cover the de-hyphenation decision, because the shipped default is
# now "off": flipping the default, and ignoring the flag entirely. Either one must
# turn the suite red -- that is what makes "off" a tested choice rather than a
# comment.  Both runs assert the patch actually applied, so a silently-failing perl
# expression can never be reported as "mutation detected".
echo "== pdf line merge: mutation (default de-hyphenation flipped back on) =="
mkdir -p "$BUILD_DIR/mut2/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut2/$HEADER_REL"
perl -0pi -e "s/bool deHyphenate = false;/bool deHyphenate = true;/" "$BUILD_DIR/mut2/$HEADER_REL"
if cmp -s "$BUILD_DIR/mut2/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL"; then
  echo "FAIL: mutation did not apply (default flip)" >&2
  exit 1
fi
if run_suite "$BUILD_DIR/mut2run" "$BUILD_DIR/mut2" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (de-hyphenation)" >&2
  exit 1
fi
echo "mutation detected"


echo "== pdf line merge: mutation (hard-break split removed) =="
mkdir -p "$BUILD_DIR/mut3/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut3/$HEADER_REL"
perl -0pi -e 's/const std::vector<std::string> lines = splitHardBreaks\(in\);/const std::vector<std::string> lines = in;/' \
  "$BUILD_DIR/mut3/$HEADER_REL"
if run_suite "$BUILD_DIR/mut3run" "$BUILD_DIR/mut3" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (hard-break split)" >&2
  exit 1
fi
echo "mutation detected"

echo "== pdf line merge: mutation (median-relative unwrap disabled) =="
mkdir -p "$BUILD_DIR/mut4/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut4/$HEADER_REL"
perl -0pi -e 's/lastLineShort = !endsHyphen && shortEnoughToBeABreak\(length, median, opt\.unwrapPercent\);/lastLineShort = false;/' \
  "$BUILD_DIR/mut4/$HEADER_REL"
if run_suite "$BUILD_DIR/mut4run" "$BUILD_DIR/mut4" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (median-relative unwrap)" >&2
  exit 1
fi
echo "mutation detected"

echo "== pdf line merge: mutation (de-hyphenation flag ignored) =="
mkdir -p "$BUILD_DIR/mut4/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mut4/$HEADER_REL"
perl -0pi -e "s/if \(deHyphenate && !a\.empty\(\) && a\.back\(\) == '-'\)/if (true \&\& !a.empty() \&\& a.back() == '-')/" "$BUILD_DIR/mut4/$HEADER_REL"
if cmp -s "$BUILD_DIR/mut4/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL"; then
  echo "FAIL: mutation did not apply (flag ignored)" >&2
  exit 1
fi
if run_suite "$BUILD_DIR/mut4run" "$BUILD_DIR/mut4" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (de-hyphenation always on)" >&2
  exit 1
fi
echo "mutation detected"


# Blankness must be codepoint-aware: a paragraph of U+3000 / NBSP / ZWSP is invisible on
# paper, and treating it as text is exactly the "runs of blank lines" the option exists to
# remove.  Reverting to a byte comparison has to turn the suite red.
echo "== pdf line merge: mutation (blankness back to a byte comparison) =="
mkdir -p "$BUILD_DIR/mutblank/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mutblank/$HEADER_REL"
perl -0pi -e 's/if \(utf8IsBlankText\(para\)\) \{/if (para.empty()) { \/\* BLANKREG *\//' \
  "$BUILD_DIR/mutblank/$HEADER_REL"
cmp -s "$BUILD_DIR/mutblank/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL" && {
  echo "FAIL: the blankness mutation did not apply; the run below would pass vacuously" >&2; exit 1; }
grep -q 'BLANKREG' "$BUILD_DIR/mutblank/$HEADER_REL" || {
  echo "FAIL: the blankness mutation landed somewhere unexpected" >&2; exit 1; }
if run_suite "$BUILD_DIR/mutblankrun" "$BUILD_DIR/mutblank" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (byte-level blankness)" >&2
  exit 1
fi
echo "mutation detected (byte-level blankness)"

# --- the table-of-contents guards (2026-11-19) -----------------------------------
# These exist because the median-relative rule is backwards on a page whose rows are
# all alike: measured, 6 TOC rows collapsed into one paragraph and a poem became
# 「床前明月光疑是地上霜…」 -- the option inventing the defect it repairs, on by
# default.  Each mutation removes one of the three guards and must turn the suite red.

echo "== pdf line merge: mutation (page-level table guard removed) =="
mkdir -p "$BUILD_DIR/muttoc/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/muttoc/$HEADER_REL"
perl -0pi -e 's!return shape\.lines >= 3 && shape\.entryRows \* 2 >= shape\.lines;!return false; /* TOC-GUARD */!' \
  "$BUILD_DIR/muttoc/$HEADER_REL"
cmp -s "$BUILD_DIR/muttoc/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL" && {
  echo "FAIL: the table guard mutation did not apply" >&2; exit 1; }
grep -q 'TOC-GUARD' "$BUILD_DIR/muttoc/$HEADER_REL" || {
  echo "FAIL: the table guard mutation landed somewhere unexpected" >&2; exit 1; }
if run_suite "$BUILD_DIR/muttocrun" "$BUILD_DIR/muttoc" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (page-level table guard)" >&2
  exit 1
fi
echo "mutation detected (page-level table guard)"

echo "== pdf line merge: mutation (a leader/number row may still be merged forward) =="
mkdir -p "$BUILD_DIR/mutrow/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mutrow/$HEADER_REL"
perl -0pi -e 's%const bool canMerge = !prevTerminal && !prevAloneIsShort && !prevIsEntryRow;%const bool canMerge = !prevTerminal && !prevAloneIsShort; /* ROW-GUARD */%' \
  "$BUILD_DIR/mutrow/$HEADER_REL"
cmp -s "$BUILD_DIR/mutrow/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL" && {
  echo "FAIL: the entry-row mutation did not apply (or became identical)" >&2; exit 1; }
grep -q 'ROW-GUARD' "$BUILD_DIR/mutrow/$HEADER_REL" || {
  echo "FAIL: the entry-row mutation landed somewhere unexpected" >&2; exit 1; }
if run_suite "$BUILD_DIR/mutrowrun" "$BUILD_DIR/mutrow" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (entry row merged forward)" >&2
  exit 1
fi
echo "mutation detected (entry row merged forward)"

echo "== pdf line merge: mutation (an enumerated row no longer starts a paragraph) =="
mkdir -p "$BUILD_DIR/mutenumeration/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mutenumeration/$HEADER_REL"
perl -0pi -e 's!                         startsEnumerated\(para\);!                         false; /* ENUM-GUARD */!' \
  "$BUILD_DIR/mutenumeration/$HEADER_REL"
cmp -s "$BUILD_DIR/mutenumeration/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL" && {
  echo "FAIL: the enumeration mutation did not apply" >&2; exit 1; }
grep -q 'ENUM-GUARD' "$BUILD_DIR/mutenumeration/$HEADER_REL" || {
  echo "FAIL: the enumeration mutation landed somewhere unexpected" >&2; exit 1; }
if run_suite "$BUILD_DIR/mutenumerationrun" "$BUILD_DIR/mutenumeration" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (enumeration start)" >&2
  exit 1
fi
echo "mutation detected (enumeration start)"


echo "== pdf line merge: mutation (an enumerated row may be merged forward again) =="
mkdir -p "$BUILD_DIR/mutenumfwd/$(dirname "$HEADER_REL")"
cp "$REPO_ROOT/src/$HEADER_REL" "$BUILD_DIR/mutenumfwd/$HEADER_REL"
perl -0pi -e 's%const bool prevIsEntryRow = isEntryRow\(prev\) \|\| startsEnumerated\(prev\);%const bool prevIsEntryRow = isEntryRow(prev); /* ENUMFWD-GUARD */%' \
  "$BUILD_DIR/mutenumfwd/$HEADER_REL"
cmp -s "$BUILD_DIR/mutenumfwd/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL" && {
  echo "FAIL: the enumerated-forward mutation did not apply" >&2; exit 1; }
grep -q 'ENUMFWD-GUARD' "$BUILD_DIR/mutenumfwd/$HEADER_REL" || {
  echo "FAIL: the enumerated-forward mutation landed somewhere unexpected" >&2; exit 1; }
if run_suite "$BUILD_DIR/mutenumfwdrun" "$BUILD_DIR/mutenumfwd" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (enumerated row merged forward)" >&2
  exit 1
fi
echo "mutation detected (enumerated row merged forward)"

NOW_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
if [ "$NOW_SHA" != "$PRISTINE_SHA" ]; then
  echo "FAIL: a mutation modified the pristine header under src/ -- the tree is dirty" >&2
  git -C "$REPO_ROOT" diff --stat -- "$HEADER_REL" >&2 || true
  exit 1
fi

# Each guard is pinned on its own, so a future edit that drops one of them cannot hide
# behind the others (the real-TOC fixture trips pageIsAnEntryList AND the row guards at
# once, which is why the combined mutations above are not enough to tell them apart).
for guard in LEADER NUMBER; do
  echo "== pdf line merge: mutation (${guard} predicate disabled) =="
  dir="$BUILD_DIR/mut_${guard}"
  mkdir -p "$dir/$(dirname "$HEADER_REL")"
  cp "$REPO_ROOT/src/$HEADER_REL" "$dir/$HEADER_REL"
  case "$guard" in
    LEADER) perl -0pi -e 's%if \(dots >= 3 \|\| middots >= 3 \|\| ellipses >= 2\) return true;%if (false) return true; /* LEADER-OFF */%' "$dir/$HEADER_REL" ;;
    NUMBER) perl -0pi -e 's%return \(cp >= .0. && cp <= .9.\) \|\| \(cp >= 0xFF10 && cp <= 0xFF19\);%return false; /* NUMBER-OFF */%' "$dir/$HEADER_REL" ;;
  esac
  cmp -s "$dir/$HEADER_REL" "$REPO_ROOT/src/$HEADER_REL" && {
    echo "FAIL: the ${guard} mutation did not apply" >&2; exit 1; }
  grep -q "${guard}-OFF" "$dir/$HEADER_REL" || {
    echo "FAIL: the ${guard} mutation landed somewhere unexpected" >&2; exit 1; }
  if run_suite "${dir}run" "$dir" >/dev/null 2>&1; then
    echo "FAIL: mutation was not detected (${guard} predicate)" >&2
    exit 1
  fi
  echo "mutation detected (${guard} predicate)"
done

echo "pdf line merge host regression: PASS"
