// Host regression for the PDF block-splitting rule (src/pdf/PdfLineJoin.h).
//
// MuPDF hands one `fz_stext_block` to the extractor for lines that PDF stored as
// separate, evenly-spaced lines.  Gluing them unconditionally fuses two paragraphs of
// wrapped Chinese into one, and the newline is gone before any app-side rule can see it.
// The rule here re-creates that boundary from line geometry -- and, just as importantly,
// REFUSES to when the block is not a text column, because the naive version shredded a
// real manual's code listings and space-only lines (73 paragraphs became 89).
//
// Every threshold and every guard is mutation-checked.  Real pages are measured through
// this same walk by join_real_pages.py driving PdfLineJoinProbe.cpp, so the numbers in
// docs/engineering/pdf-xip.md are not a Python restatement of the rule.
#include <cstdio>
#include <string>
#include <vector>

#include "PdfLineJoin.h"

static int failures = 0;
static int checks = 0;

static void check(const bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

// A line spanning `x0..x1` on a `y`-pitch of 16, 12 tall.
static pdf_join::LineRecord ln(const float x0, const float x1, const float y,
                               const bool printable = true) {
  return pdf_join::LineRecord{{x0, y, x1, y + 12.f}, printable};
}

// Six lines wrapped at 100..300, with a paragraph end after the third (short) one.
// Every line reaches the right edge except the short one, so this IS a text column.
static std::vector<pdf_join::LineRecord> wrappedColumn() {
  return {ln(100, 300, 0), ln(100, 300, 16), ln(100, 300, 32),
          ln(100, 160, 48),                      // ends the paragraph: 40 pt short of 300
          ln(100, 300, 64), ln(100, 300, 80)};
}

// A shell listing: every line lands at a different width. Fewer than 60% reach the right
// edge, so the column test has to switch the ragged rule off entirely.
static std::vector<pdf_join::LineRecord> codeListing() {
  return {ln(100, 300, 0),  ln(100, 140, 16), ln(100, 210, 32),
          ln(100, 120, 48), ln(100, 260, 64), ln(100, 130, 80)};
}

static size_t paragraphCount(const std::vector<pdf_join::LineRecord>& lines) {
  return pdf_join::paragraphStarts(lines).size();
}

// --- the splitting half -------------------------------------------------------------

static void testSplitsAWrappedColumn() {
  const auto lines = wrappedColumn();
  check(paragraphCount(lines) == 2, "six wrapped lines split into two paragraphs");
  const auto starts = pdf_join::paragraphStarts(lines);
  check(starts.size() == 2 && starts[0] == 0 && starts[1] == 4,
        "the break lands after the short line, not on it");
  const pdf_join::BlockStats stats = pdf_join::measureBlock(lines);
  check(stats.lines == 6 && stats.fullLines == 5, "a column counts its full lines");
  check(stats.columnLeft == 100.f && stats.columnRight == 300.f, "the column is the widest span");
  check(pdf_join::looksLikeTextColumn(stats), "five of six lines full is a text column");
}

static void testRaggedBoundary() {
  const pdf_join::BlockStats stats{100.f, 300.f, 12.f, 6, 5};
  // 25% of a 200 pt column = 50 pt. x1=250 -> shortfall 50 -> splits; x1=251 -> 49 -> does not.
  check(pdf_join::between(ln(100, 250, 0).box, ln(100, 300, 16).box, stats) ==
            pdf_join::Decision::NewParagraph,
        "a 25%-of-column shortfall splits");
  check(pdf_join::between(ln(100, 251, 0).box, ln(100, 300, 16).box, stats) == pdf_join::Decision::Glue,
        "one point less does not");
}

static void testGapSplitsEvenJustifiedText() {
  const pdf_join::BlockStats column{100.f, 300.f, 12.f, 6, 6};
  // gap = next.y0 - current.y1, so y0=48 against a 12-tall line is 36 = 3x the height.
  check(pdf_join::between(ln(100, 300, 0).box, ln(100, 300, 48).box, column) ==
            pdf_join::Decision::NewParagraph,
        "a gap of three line heights splits even fully justified text");
  check(pdf_join::between(ln(100, 300, 0).box, ln(100, 300, 47).box, column) ==
            pdf_join::Decision::Glue,
        "one point less is a normal line gap");
  // The ragged half needs a column; the gap half does not, which is what lets a two-line
  // block with real air inside it break.
  const std::vector<pdf_join::LineRecord> two{ln(100, 150, 0), ln(100, 300, 16)};
  const pdf_join::BlockStats twoStats = pdf_join::measureBlock(two);
  check(!pdf_join::looksLikeTextColumn(twoStats),
        "one line of two missing the margin is not a ratio a column would show");
  check(pdf_join::between(two[0].box, two[1].box, twoStats) == pdf_join::Decision::Glue,
        "so its short first line is not read as a paragraph end");
  // ... and a two-line block whose lines BOTH reach the margin has nothing for the ragged
  // test to find.  These two cases together are why no separate minimum line count is
  // needed (see the comment on kMinFullLinePercent).
  const std::vector<pdf_join::LineRecord> twoFull{ln(100, 300, 0), ln(100, 295, 16)};
  const pdf_join::BlockStats twoFullStats = pdf_join::measureBlock(twoFull);
  check(twoFullStats.fullLines == 2, "a line within 5% of the margin counts as full");
  check(pdf_join::between(twoFull[0].box, twoFull[1].box, twoFullStats) == pdf_join::Decision::Glue,
        "and a 5% shortfall is far below the 25% the ragged test needs");
  check(paragraphCount({ln(100, 300, 0), ln(100, 300, 52)}) == 2,
        "the gap test does run on a two-line block");
}

// --- the refusing half (both found on real pages) -----------------------------------

static void testCodeListingIsNotShredded() {
  const pdf_join::BlockStats stats = pdf_join::measureBlock(codeListing());
  check(stats.lines == 6, "the listing has six printable lines");
  check(stats.fullLines * 100 < stats.lines * 60, "fewer than 60% of them reach the margin");
  check(!pdf_join::looksLikeTextColumn(stats), "a listing is not a text column");
  check(paragraphCount(codeListing()) == 1, "a listing stays one paragraph -- no 89-paragraph pages");
}

static void testBlankLinesBreakButAreNotText() {
  // Space-only lines carry no character yet have a small box: the naive version read one
  // as a colossal shortfall and split there.
  const std::vector<pdf_join::LineRecord> lines{
      ln(100, 300, 0), ln(100, 118, 16, false), ln(100, 300, 32), ln(100, 300, 48)};
  const pdf_join::BlockStats stats = pdf_join::measureBlock(lines);
  check(stats.lines == 3, "a space-only line is not counted as a line of the column");
  check(stats.columnRight == 300.f, "and does not shrink the column");
  const auto starts = pdf_join::paragraphStarts(lines);
  check(starts.size() == 2 && starts[1] == 2,
        "it does force the next text line to start a paragraph");
  check(paragraphCount({ln(100, 300, 0), ln(100, 118, 16, false), ln(100, 300, 32)}) == 2,
        "one blank inside two full lines is still a break");
}

static void testShortColumnStillSplitsOnGap() {
  // A block at the kMinColumnLines boundary: three full lines + a short one.
  const std::vector<pdf_join::LineRecord> lines{ln(100, 300, 0), ln(100, 300, 16),
                                                ln(100, 300, 32), ln(100, 150, 48)};
  check(paragraphCount(lines) == 1, "four lines, last short: nothing follows the short line");
  auto withMore = lines;
  withMore.push_back(ln(100, 300, 64));
  check(paragraphCount(withMore) == 2, "adding the next paragraph's first line reveals the break");
}

// --- degenerate input must never invent a paragraph ---------------------------------

static void testBlankCharacterSet() {
  // The module cannot include lib/Utf8, so its blank set is a deliberate mirror; these
  // are the codepoints whose absence would hand the ragged test a space-only line.
  check(pdf_join::isBlankCp(' '), "space");
  check(pdf_join::isBlankCp(0x3000), "ideographic space (CJK indentation)");
  check(pdf_join::isBlankCp(0xA0), "NBSP");
  check(pdf_join::isBlankCp(0x200B), "ZWSP");
  check(pdf_join::isBlankCp(0xFEFF), "BOM");
  check(pdf_join::isBlankCp(-1), "MuPDF's negative sentinel is not ink");
  check(pdf_join::isBlankCp('\t'), "tab");
  check(!pdf_join::isBlankCp(0x672C), "a CJK character is ink");
  check(!pdf_join::isBlankCp('A'), "a letter is ink");
  check(!pdf_join::isBlankCp(0x2013), "an en dash is ink -- it prints");
}

// The cost of the column gate, pinned rather than wished away: two TWO-line paragraphs
// fused into one block show only 50% full lines, so the gate refuses and they stay glued.
// Lowering kMinFullLinePercent would split this case and shred the listings above -- the
// real pages measured 28 bogus splits against 7 correct ones -- so this miss is the price
// of the refusal, and it is the app-side reader (which sees two paragraphs on one line)
// rather than a silently fused sentence.
static void testShortFusedParagraphsAreTheKnownMiss() {
  const std::vector<pdf_join::LineRecord> lines{ln(100, 300, 0), ln(100, 180, 16),
                                               ln(100, 300, 32), ln(100, 190, 48)};
  const pdf_join::BlockStats stats = pdf_join::measureBlock(lines);
  check(stats.lines == 4 && stats.fullLines == 2, "two of four lines reach the margin");
  check(!pdf_join::looksLikeTextColumn(stats), "50% full is below the 60% gate");
  check(paragraphCount(lines) == 1, "KNOWN MISS: this pair stays fused (documented, not hidden)");
}

// The reader's 智能合并断行 switch reaches the engine through this flag, so the engine has
// to be able to do exactly what it did before this feature existed.
static void testSwitchOffRestoresLegacyGluing() {
  check(pdf_join::paragraphStarts(wrappedColumn(), false).size() == 1,
        "off: a fused column stays one paragraph");
  check(pdf_join::paragraphStarts(codeListing(), false).size() == 1, "off: a listing is one paragraph");
  check(pdf_join::paragraphStarts({ln(100, 300, 0), ln(100, 118, 16, false), ln(100, 300, 32)}, false)
            .size() == 1,
        "off: a blank line no longer forces a break either");
  check(pdf_join::paragraphStarts({ln(100, 300, 0), ln(100, 300, 52)}, false).size() == 1,
        "off: not even a paragraph-sized gap splits");
  // The default argument matters as much as the off path: a caller that forgets to thread
  // the setting must not silently disable the fix.
  check(pdf_join::paragraphStarts(wrappedColumn()).size() == 2, "on by default");
  check(pdf_join::paragraphStarts(wrappedColumn(), true).size() == 2, "on when asked");
  check(pdf_join::paragraphStarts({}, false).empty(), "off with nothing printable stays empty");
  check(pdf_join::paragraphStarts({ln(100, 118, 0, false), ln(100, 300, 16)}, false).size() == 1,
        "off: the first line WITH ink is the paragraph start");
}

static void testDegenerateInputGlues() {
  check(paragraphCount({}) == 0, "an empty block has no paragraphs");
  check(paragraphCount({ln(100, 118, 0, false)}) == 0, "a block of blanks has none either");
  check(paragraphCount({ln(100, 300, 0)}) == 1, "a single line is one paragraph");
  // A one-glyph column: width cannot say anything.
  check(paragraphCount({ln(100, 101, 0), ln(100, 101, 16), ln(100, 101, 32)}) == 1,
        "a hairline column is left alone");
  // The interesting hairline: lines that differ by half a point.  Scaled by a column
  // width that is itself one point, that is a 50% shortfall -- the ragged test would fire
  // on nothing but rounding, which is why a sub-glyph block is never a column.
  check(paragraphCount({ln(100, 101, 0), ln(100, 100.5f, 16), ln(100, 101, 32)}) == 1,
        "a block narrower than a glyph cannot describe a column");
  // Degenerate boxes (x1 <= x0) are skipped, not read as maximally short.
  check(paragraphCount({ln(100, 100, 0), ln(100, 300, 16), ln(100, 300, 32),
                        ln(100, 300, 48)}) == 1,
        "an empty box is not a paragraph end");
  check(pdf_join::measureBlock({ln(100, 100, 0)}).lines == 0, "and is not counted");
  // Two full-width lines at an ordinary pitch stay one paragraph (the pre-existing
  // behaviour: this rule may only ADD breaks it is sure about).
  check(paragraphCount({ln(100, 300, 0), ln(100, 300, 16), ln(100, 300, 32),
                        ln(100, 300, 48)}) == 1,
        "an even, full block is one paragraph");
}

int main() {
  testShortFusedParagraphsAreTheKnownMiss();
  testSwitchOffRestoresLegacyGluing();
  testBlankCharacterSet();
  testSplitsAWrappedColumn();
  testRaggedBoundary();
  testGapSplitsEvenJustifiedText();
  testCodeListingIsNotShredded();
  testBlankLinesBreakButAreNotText();
  testShortColumnStillSplitsOnGap();
  testDegenerateInputGlues();
  std::printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
  return failures == 0 ? 0 : 1;
}
