#pragma once

#include <cstdint>
#include <vector>

// Should the next line of one MuPDF text block be glued onto the current paragraph, or
// does it begin a new one -- and therefore: where do the paragraphs of a block start?
//
// Why this exists.  MuPDF groups hard-wrapped lines into a single `fz_stext_block`
// whenever they are evenly spaced -- measured with test/pdf_line_join/
// make_cjk_block_probe.py: two Chinese paragraphs, each wrapped at 16 characters on a
// uniform 16 pt pitch, come back as ONE block of SIX lines.  The extractor glued every
// line of a block together unconditionally, so those two paragraphs reached the reader
// as one run, and once the newline is gone no app-side rule (`pdf_merge`) can put it
// back -- that half of the problem is only ever visible through geometry, and geometry
// lives here, inside the module.
//
// What the first attempt got wrong, and why `looksLikeTextColumn` exists.  Applied
// naively to a block, "a line that ends short of the column is a paragraph end" split
// four real pages of a shipped manual from 73 paragraphs into 89: source-code listings
// are ragged *by nature*, and a space-only span has a tiny box that reads as an
// enormous shortfall.  Requiring the block to look like a text column first -- most of
// its lines really do reach the right edge -- takes the same pages to 74, and the one
// remaining split is a genuine boundary (a function signature followed by the prose
// describing it).
//
// The rule can therefore only ever SPLIT a glue it is confident about; anything it
// cannot read as a text column keeps the behaviour this file shipped with before.  The
// asymmetry is the one the whole feature uses: an extra paragraph break looks like the
// source did before any of this ran, while a missing one silently fuses two thoughts.
//
// `paragraphStarts()` is exported rather than an inline loop in the caller so that the
// host test and the real-page measurement (test/pdf_line_join/join_real_pages.py driving
// PdfLineJoinProbe.cpp) run the SAME walk the device runs.  A Python restatement of this
// rule would have been a second implementation, and the two readers of a page would have
// drifted apart exactly like the two gesture machines did.
//
// Pure and free of MuPDF headers (plain floats, one bool and ints in; indices out) so a
// host test can pin every threshold.
namespace pdf_join {

struct LineBox {
  // A MuPDF `fz_rect`, already in the page's own units.
  float x0 = 0.f;
  float y0 = 0.f;
  float x1 = 0.f;
  float y1 = 0.f;
};

// One visual line of a block.  `printable` is whether the line carries a character that
// is not blank: PDF emits space-only lines, and they must not reach the page as text --
// but they do mean something, so they are read as a paragraph break.
struct LineRecord {
  LineBox box;
  bool printable = true;
};

// Everything the decision needs about one block.
struct BlockStats {
  float columnLeft = 0.f;
  float columnRight = 0.f;
  float avgHeight = 0.f;
  int lines = 0;      // printable lines
  int fullLines = 0;  // lines reaching the right edge (within kFullTolerancePercent)
};

enum class Decision { Glue, NewParagraph };

// A line ending this far short of the block's widest right edge is the last line of its
// paragraph.  Measured against the column rather than the line's own length, because a
// wrapped column is full width on every line but the last.
constexpr int kRaggedPercentOfColumn = 25;

// Vertical whitespace this many times the mean line height is a paragraph gap, not a
// line gap.  This is the only signal that works on justified text, where the ragged test
// sees nothing because every line reaches the margin.
constexpr int kGapMultiple = 3;

// A line whose right edge is within this percent of the block's widest line counts as
// "full", i.e. as evidence that the block is a text column.
constexpr int kFullTolerancePercent = 5;

// Below this share of full lines the block is not a text column, and the ragged test is
// switched off: code listings, indented quotations, tables and verse are ragged
// everywhere and would otherwise be shredded one line at a time.
constexpr int kMinFullLinePercent = 60;

// This ratio is the ONLY gate on the ragged test, deliberately.  An earlier version also
// required three lines, on the theory that a two-line caption cannot describe a column --
// but the ratio already covers that shape, and the extra check was untestable: a
// two-line block either has a line that misses the margin (then at most 50% is full, and
// the ratio refuses) or has only full lines (then no line has the 25%-of-column shortfall
// the ragged test looks for).  A guard no input can reach is not a safety property, so it
// is gone rather than kept-and-documented.

constexpr bool usable(const LineBox& box) {
  return box.x1 > box.x0 && box.y1 >= box.y0;
}

// A character that puts no ink on the page.  This mirrors
// lib/Utf8/Utf8.h::utf8IsBlankCodepoint on purpose -- this header is compiled into the
// flash module, where that include path does not exist -- and additionally treats MuPDF's
// negative sentinels (`FZ_NEWLINE`-style placeholders arrive as -1) as blank, which the
// `cp <= 0x20` half already does.  test/pdf_line_join pins both halves, because a space
// counted as content would give a blank line a small box and the ragged test would read
// that as a paragraph end.
constexpr bool isBlankCp(const int cp) {
  return cp <= 0x20                      // controls, space, and MuPDF's negative sentinels
         || cp == 0xA0 || cp == 0xAD     // NBSP, soft hyphen
         || (cp >= 0x2000 && cp <= 0x200F)  // thin spaces, ZWSP, ZWNJ, ZWJ, marks
         || cp == 0x202F || cp == 0x205F || cp == 0x2060 || cp == 0xFEFF
         || cp == 0x3000 || cp == 0x3164;  // ideographic space, Hangul filler
}

// Is this block a column of wrapped prose?  `lines` guards against reading a two-line
// caption as a column; the ratio is what keeps a listing out.
constexpr bool looksLikeTextColumn(const BlockStats& stats) {
  // The width test lives where the decision is made (`between`) and where the count is
  // taken (`measureBlock`); repeating it here only made this function harder to read and
  // gave a mutation harness three places to patch instead of two.
  return static_cast<int64_t>(stats.fullLines) * 100 >=
         static_cast<int64_t>(stats.lines) * kMinFullLinePercent;
}

inline BlockStats measureBlock(const std::vector<LineRecord>& lines) {
  BlockStats stats;
  float heightSum = 0.f;
  for (const auto& line : lines) {
    if (!line.printable || !usable(line.box)) continue;
    if (stats.lines == 0) {
      stats.columnLeft = line.box.x0;
      stats.columnRight = line.box.x1;
    } else {
      if (line.box.x0 < stats.columnLeft) stats.columnLeft = line.box.x0;
      if (line.box.x1 > stats.columnRight) stats.columnRight = line.box.x1;
    }
    heightSum += line.box.y1 - line.box.y0;
    ++stats.lines;
  }
  if (stats.lines == 0) return stats;
  stats.avgHeight = heightSum / static_cast<float>(stats.lines);
  const float column = stats.columnRight - stats.columnLeft;
  if (column > 1.f) {
    for (const auto& line : lines) {
      if (!line.printable || !usable(line.box)) continue;
      if ((stats.columnRight - line.box.x1) * 100.f <=
          column * static_cast<float>(kFullTolerancePercent)) {
        ++stats.fullLines;
      }
    }
  }
  return stats;
}

inline Decision between(const LineBox& current, const LineBox& next, const BlockStats& stats) {
  if (!usable(current) || !usable(next)) return Decision::Glue;
  const float column = stats.columnRight - stats.columnLeft;
  if (column <= 1.f) return Decision::Glue;  // one-glyph column: width says nothing
  // 1. ragged right edge -> paragraph end, but only where the block really is a column.
  if (looksLikeTextColumn(stats)) {
    const float shortfall = stats.columnRight - current.x1;
    if (shortfall * 100.f >= column * static_cast<float>(kRaggedPercentOfColumn)) {
      return Decision::NewParagraph;
    }
  }
  // 2. an outsized vertical gap before `next` -> paragraph break, regardless of width.
  if (stats.avgHeight > 0.5f) {
    const float gap = next.y0 - current.y1;
    if (gap >= stats.avgHeight * static_cast<float>(kGapMultiple)) return Decision::NewParagraph;
  }
  return Decision::Glue;
}

// The index of the first line of every paragraph of this block, always including the
// first printable line.  Non-printable lines are not paragraph starts (they carry no
// text) but they do force the next printable line to start one.  Degenerate blocks --
// nothing printable, one line -- come back as one paragraph, which is the pre-existing
// behaviour: this function can only ever ADD a break.
// `enabled` is the reader's 智能合并断行 switch, carried into the engine so that ONE
// option governs both directions of the paragraphing fix (see `Options` in PdfLineMerge.h
// for the app-side half).  When off, this returns exactly the pre-existing behaviour --
// one paragraph per block -- and computes nothing.  Blank lines are still not emitted as
// text even when off: a space is not a character the reader asked for, and gluing it in was
// a defect rather than a behaviour.
inline std::vector<int> paragraphStarts(const std::vector<LineRecord>& lines,
                                        const bool enabled = true) {
  std::vector<int> starts;
  if (!enabled) {
    for (size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].printable && usable(lines[i].box)) {
        starts.push_back(static_cast<int>(i));
        break;
      }
    }
    return starts;
  }
  const BlockStats stats = measureBlock(lines);
  if (stats.lines == 0) return starts;
  int previous = -1;
  bool breakPending = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (!lines[i].printable || !usable(lines[i].box)) {
      if (previous >= 0) breakPending = true;  // a blank line inside the block IS a break
      continue;
    }
    if (starts.empty()) {
      starts.push_back(static_cast<int>(i));
    } else if (breakPending ||
               (previous >= 0 && between(lines[previous].box, lines[i].box, stats) ==
                                    Decision::NewParagraph)) {
      starts.push_back(static_cast<int>(i));
    }
    breakPending = false;
    previous = static_cast<int>(i);
  }
  return starts;
}

}  // namespace pdf_join
