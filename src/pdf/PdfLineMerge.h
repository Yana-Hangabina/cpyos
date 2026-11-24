#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include <Utf8.h>
#include <vector>

#include "util/SelectionText.h"

// OCR'd and hard-wrapped PDFs expose every visual line either as its own text
// block or as a '\n' inside one block, so a reflow that trusts the extraction
// shows a page of ragged one-line "paragraphs" (本书如何帮助 / 你成为 ...) and
// runs of blank ones.  merge() therefore starts by flattening both shapes into
// one line per entry, so a line-final hyphen and a hard wrap are handled the
// same way whoever produced them.
//
// This is the mature, minimal fix used by calibre's "Heuristic processing" and
// post-OCR correctors: re-join consecutive lines that are really one paragraph,
// de-hyphenate line-final hyphens, and drop blank runs -- with NO machine
// learning, only the text itself.  "Short line means a paragraph end" is judged
// against the page's median line length (calibre's --html-unwrap-factor), not a
// fixed character count, because a full line is a different length in every
// script.  It is deliberately conservative so a real paragraph break (a line
// ending in sentence-final punctuation, an indented start, a blank run, or a
// short final line) is never swallowed.
//
// See docs/engineering/pdf-xip.md "Smart line merge" for what was read, what
// was adopted, and the two established rules that were declined with reasons.
//
// Pure and header-only so a host test can pin every rule
// (test/pdf_line_merge/run_pdf_line_merge_test.sh).
namespace pdf_merge {

// calibre's default --html-unwrap-factor, as an integer percent so no float is
// needed to compare against a median length.
constexpr size_t kUnwrapPercent = 40;
// Below this many codepoints a line is short in any script, whatever the
// median says.
constexpr size_t kAbsoluteShortCp = 3;

// Codepoints that end a sentence, and therefore almost always end a paragraph
// in an already-punctuated book. A line ending here is NOT merged forward.
inline bool isTerminalCp(const uint32_t cp) {
  switch (cp) {
    case 0x3002:  // 。
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
    case 0x2026:  // …
    case 0x2025:  // ‥
    case '.':
    case '!':
    case '?':
      return true;
    default:
      return false;
  }
}

// A closing bracket/quote right after the terminal still counts as the end
// (。」, !"), so scan back over the non-letter closers.
inline bool endsSentence(const std::string& s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const unsigned char* end = p + s.size();
  const unsigned char* q = end;
  while (q > p) {
    // start of the codepoint that ends just before q
    const unsigned char* r = q - 1;
    while (r > p && (*r & 0xC0) == 0x80) --r;
    const unsigned char* cursor = r;
    const uint32_t cp = utf8NextCodepoint(&cursor);
    const bool closer = cp == 0x300D /*」*/ || cp == 0x300F /*』*/ || cp == 0xFF09 /*）*/ ||
                        cp == 0x201D /*”*/ || cp == 0x2019 /*’*/ || cp == ')' || cp == ']' || cp == '"' ||
                        cp == '\'';
    if (isTerminalCp(cp)) return true;
    if (!closer) return false;
    q = r;  // step back to this codepoint's start and look at the one before it
  }
  return false;
}

// Count codepoints (cheap length proxy for "is this a short heading?").
inline size_t codepoints(const std::string& s) {
  size_t n = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  const unsigned char* end = p + s.size();
  while (p < end) {
    const unsigned char* before = p;
    utf8NextCodepoint(&p);
    if (p == before) break;  // malformed: stop rather than loop
    ++n;
  }
  return n;
}

inline std::string trimRight(const std::string& s) {
  size_t e = s.size();
  while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(0, e);
}

// Drop leading blank material. Reflow draws a paragraph's first-line indent
// itself (SETTINGS-driven), so keeping the source's own indentation would
// double it -- and in CJK text the marker is U+3000, which no ASCII trim sees.
inline std::string trimLeft(std::string s) {
  size_t n = 0;
  while (n < s.size()) {
    if (s[n] == ' ' || s[n] == '\t' || s[n] == '\r' || s[n] == '\n') {
      ++n;
    } else if (s.size() - n >= 3 && static_cast<unsigned char>(s[n]) == 0xE3 &&
               static_cast<unsigned char>(s[n + 1]) == 0x80 && static_cast<unsigned char>(s[n + 2]) == 0x80) {
      n += 3;
    } else {
      break;
    }
  }
  return s.substr(n);
}

// One row of a table of contents, an index, or an entry list.  Both signals below
// are things a wrapped sentence does not do:
//  * a filler run -- three or more '.' or '·', or two '…'.  Leaders exist to bridge
//    a title to its page number; prose emits them only as an ellipsis, and a line
//    that ENDS in an ellipsis already stops a merge (it reads as terminal);
//  * a bare number as the LAST codepoint.  A wrap that fell inside "1234" would
//    leave the fragment, not the finished number, so a digit at the end of a line
//    means the line ends where an aligned column does.  A prose line can end on a
//    number ("总计 12"), and the cost there is one missed merge -- never a wrong one.
inline bool hasLeaderRun(const std::string& s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  const unsigned char* end = p + s.size();
  int dots = 0;
  int middots = 0;
  int ellipses = 0;
  while (p < end) {
    const unsigned char* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (p == before) break;  // malformed: stop rather than loop
    dots = (cp == '.') ? dots + 1 : 0;
    middots = (cp == 0xB7 /* · */ || cp == 0x2027) ? middots + 1 : 0;
    ellipses = (cp == 0x2026 /* … */) ? ellipses + 1 : 0;
    if (dots >= 3 || middots >= 3 || ellipses >= 2) return true;
  }
  return false;
}

inline bool endsWithDigit(const std::string& s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  const unsigned char* end = p + s.size();
  const unsigned char* q = end;
  while (q > p) {
    const unsigned char* r = q - 1;
    while (r > p && (*r & 0xC0) == 0x80) --r;
    const unsigned char* cursor = r;
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (utf8IsBlankCodepoint(cp)) {  // trailing spaces are not the end of the row
      q = r;
      continue;
    }
    return (cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19);  // ASCII + full width
  }
  return false;
}

inline bool isEntryRow(const std::string& s) { return hasLeaderRun(s) || endsWithDigit(s); }

// An entry's first row announces itself: `第一章 …`, `3. …`, `12、…`, or a bullet.
// Used to start a paragraph, so a table of contents without leaders still survives.
inline bool startsEnumerated(const std::string& s) {
  if (s.empty()) return false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  const unsigned char* end = p + s.size();
  uint32_t first = 0;
  const unsigned char* start = p;
  first = utf8NextCodepoint(&p);
  if (first == 0x7B2C /* 第 */) {
    // 第 + up to four codepoints (a numeral) + a section word.
    static constexpr uint32_t kSectionCps[] = {0x7AE0 /* 章 */, 0x8282 /* 节 */, 0x7BC0 /* 節 */,
                                               0x56DE /* 回 */, 0x5377 /* 卷 */, 0x7BC7 /* 篇 */,
                                               0x90E8 /* 部 */, 0x5371 /* 卷(变体) */, 0x8B1B /* 讲 */};
    for (int step = 0; step < 4 && p < end; ++step) {
      const unsigned char* before = p;
      const uint32_t cp = utf8NextCodepoint(&p);
      if (p == before) break;
      for (const uint32_t section : kSectionCps) {
        if (cp == section) return true;
      }
    }
    return false;
  }
  // Only unambiguous bullet glyphs count.  A leading '*' or '>' is markdown-ish
  // emphasis or a quote marker more often than it is a list bullet -- measured on a
  // real PDF page it wrongly fired on `*None*` and cost three legitimate merges -- and
  // a bare '-' is what a hyphenated wrap fragment looks like.  A dash therefore has to
  // be followed by a space to read as a bullet.
  static constexpr uint32_t kBullets[] = {0x2022 /* • */, 0x2027 /* ‧ */, 0x00B7 /* · */,
                                          0x25CF /* ● */, 0x25CB /* ○ */, 0x25AA /* ▪ */,
                                          0x2013 /* – */, 0x2014 /* — */};
  for (const uint32_t bullet : kBullets) {
    if (first == bullet) return true;
  }
  if (first == '-' && s.size() > 1 && (s[1] == ' ' || s[1] == '\t')) return true;
  if ((first >= '0' && first <= '9') || (first >= 0xFF10 && first <= 0xFF19)) {
    // digits then a separator: `12.` `3、` `4)`
    for (int step = 0; step < 5 && p < end; ++step) {
      const unsigned char* before = p;
      const uint32_t cp = utf8NextCodepoint(&p);
      if (p == before) break;
      if (cp == '.' || cp == 0x3001 /* 、 */ || cp == ')' || cp == 0xFF09 /* ） */ || cp == ':') return true;
      if (!((cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19))) break;
    }
  }
  (void)start;
  return false;
}

// calibre's `--html-unwrap-factor` is a *fraction of the median line length*,
// not an absolute character count, and that is the right shape: a full CJK line
// is ~28 codepoints while a Latin one is ~70, so any fixed threshold is far too
// eager on one script and far too lazy on the other.  The median is used rather
// than the mean because a page of hard-wrapped lines is mostly full width plus
// a few short last lines, which pulls a mean around but barely moves the
// median.
//
// The percent is an integer so the comparison stays exact without floating
// point.  An absolute floor still applies: three codepoints is never a wrap
// whatever the median says.
//
// ★ The floor is NOT enough for a page whose lines are all of a kind -- a table of
// contents or a verse column.  There the median equals the line length, so nothing
// looks short and the percentage rule merges the whole page into one run-on
// paragraph (measured: 6 TOC rows -> 1 paragraph, 4 poem lines -> 「床前明月光疑是
// 地上霜…」).  That is the same defect the option exists to fix, pointed the other
// way, and the option is on by default -- so the entry-row rules below exist to
// catch it.
struct PageShape {
  size_t median = 0;
  size_t lines = 0;     // non-blank
  size_t entryRows = 0; // rows that read as one list/TOC entry (leader or number tail)
};

inline PageShape measurePage(const std::vector<std::string>& lines) {
  PageShape shape;
  std::vector<size_t> lengths;
  lengths.reserve(lines.size());
  for (const auto& line : lines) {
    const std::string trimmed = trimRight(trimLeft(line));
    if (utf8IsBlankText(trimmed)) continue;
    lengths.push_back(codepoints(trimmed));
    if (isEntryRow(trimmed)) ++shape.entryRows;
  }
  shape.lines = lengths.size();
  if (lengths.empty()) return shape;
  std::sort(lengths.begin(), lengths.end());
  shape.median = lengths[lengths.size() / 2];
  return shape;
}

// Half the page reading like list entries is enough to call the page a table: the
// cost of being wrong is asymmetric.  Merging prose that could have been left alone
// invents text; leaving a wrapped page as the source wrote it is exactly what the
// option OFF does, which the reader can always ask for.
inline bool pageIsAnEntryList(const PageShape& shape) {
  return shape.lines >= 3 && shape.entryRows * 2 >= shape.lines;
}

inline bool shortEnoughToBeABreak(const size_t length, const size_t median, const size_t unwrapPercent) {
  if (length <= kAbsoluteShortCp) return true;
  if (median <= kAbsoluteShortCp) return false;  // nothing meaningful to compare against
  return length * 100 <= median * unwrapPercent;
}

inline bool startsIndented(const std::string& s) {
  // Two or more leading spaces is the source telling us this is a new paragraph.
  // Chinese/Japanese books indent with U+3000 IDEOGRAPHIC SPACE, which most
  // extractors hand through untranslated, so it counts as two ordinary spaces.
  int width = 0;
  size_t n = 0;
  while (n < s.size()) {
    if (s[n] == ' ' || s[n] == '\t') {
      width += (s[n] == ' ') ? 1 : 2;
      ++n;
    } else if (s.size() - n >= 3 && static_cast<unsigned char>(s[n]) == 0xE3 &&
               static_cast<unsigned char>(s[n + 1]) == 0x80 && static_cast<unsigned char>(s[n + 2]) == 0x80) {
      width += 2;
      n += 3;
    } else {
      break;
    }
  }
  return width >= 2;
}

inline uint32_t firstCp(const std::string& s) {
  if (s.empty()) return 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  return utf8NextCodepoint(&p);
}

inline uint32_t lastCp(const std::string& s) {
  if (s.empty()) return 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  const unsigned char* end = p + s.size();
  const unsigned char* startOfLast = p;
  uint32_t cp = 0;
  while (p < end) {
    const unsigned char* before = p;
    cp = utf8NextCodepoint(&p);
    if (p == before) break;
    startOfLast = before;
  }
  (void)startOfLast;
  return cp;
}

// Join `a` and `b` as one paragraph.  `deHyphenate` controls ONE thing: whether a
// line-final hyphen is dropped.  It must never change the spacing rule -- the
// first version of the off-path did (`prev + " " + para`) and it inserted a space
// between two CJK lines, which is a new defect in the very text the option exists
// to repair.
inline std::string joinParagraphs(std::string a, const std::string& b, const bool deHyphenate) {
  a = trimRight(a);
  const std::string bTrim = trimRight(b);
  if (bTrim.empty()) return a;
  if (deHyphenate && !a.empty() && a.back() == '-') {
    a.pop_back();  // de-hyphenate: "counterpro-" + "ductive"
    return a + bTrim;
  }
  const uint32_t last = lastCp(a);
  const uint32_t first = firstCp(bTrim);
  const bool glue = selection_text::gluesToNeighbour(last) && selection_text::gluesToNeighbour(first);
  if (!a.empty() && !glue && a.back() != ' ') a.push_back(' ');
  return a + bTrim;
}

struct Options {
  // Off by default, on purpose.  De-hyphenating a line-final hyphen cannot be
  // made safe without a dictionary: measured on device, the English fixture's
  // `...identifiers, and-` + `extraordinarily...` came back as
  // `andextraordinarily` -- a fabricated non-word.  This project does not
  // silently emit wrong words (the same rule that makes a stale watermark font a
  // hard failure rather than a fallback), and a wrapped `com-` + `puter` reading
  // `com puter` is a visible cosmetic wart instead.  calibre gates this on a
  // document-derived dictionary; the blocker here is the per-page extract_text API
  // (no whole-book pass), not Flash budget.  testCompoundHyphenGluesByDesign pins
  // both directions, so turning this on stays a deliberate decision.
  bool deHyphenate = false;
  bool dropBlanks = true;
  // Percent of the page's median line length under which a line counts as a
  // real break.  0 restores the plain "three codepoints is short" rule.
  size_t unwrapPercent = kUnwrapPercent;
};

// The merge itself. Returns paragraphs with hard-wrapped lines rejoined and
// blank runs collapsed. When nothing qualifies for merging it returns the input
// unchanged (minus blanks), so turning this on never corrupts a well-formed
// extraction.
// One entry per visual line. An empty entry means the source had a blank line
// there, which the loop below treats as a real paragraph break.
inline std::vector<std::string> splitHardBreaks(const std::vector<std::string>& in) {
  std::vector<std::string> lines;
  for (const auto& para : in) {
    size_t start = 0;
    while (start <= para.size()) {
      const size_t nl = para.find('\n', start);
      const size_t stop = (nl == std::string::npos) ? para.size() : nl;
      lines.push_back(para.substr(start, stop - start));
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
    // A block's trailing newline carries no information; an interior blank line
    // does (it is the paragraph break), so only the final piece is dropped.
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();
  }
  return lines;
}

inline std::vector<std::string> merge(const std::vector<std::string>& in, const Options& opt) {
  const std::vector<std::string> lines = splitHardBreaks(in);
  const PageShape shape = measurePage(lines);
  const size_t median = shape.median;
  // A page that is mostly table-of-contents rows is not prose with hard wraps, and
  // merging it would invent text ("第一章 … 1 第二章 … 7").  Blank runs still collapse,
  // because that half is not a judgement about paragraphing.
  const bool entryList = pageIsAnEntryList(shape);
  std::vector<std::string> out;
  // A blank line is a real paragraph separator in an OCR'd book, not noise: it
  // must force the next non-empty line to start a new paragraph. Consecutive
  // blanks still collapse to that one break.
  bool pendingBreak = false;
  // The line just consumed. A short one is the ragged right edge of a
  // paragraph, so the line AFTER it belongs to a new one -- the break is only
  // knowable after reading it, which is why this is state and not a lookahead.
  bool lastLineShort = false;
  for (const auto& raw : lines) {
    // utf8IsBlankText, not para.empty(): OCR'd Chinese fills paragraphs with
    // U+3000/NBSP/ZWSP, which are invisible on paper but non-empty as bytes.  Called a
    // blank line here means it collapses to ONE paragraph break instead of becoming a
    // text line that then gets joined to a real paragraph.
    const std::string para = trimRight(trimLeft(raw));
    if (utf8IsBlankText(para)) {
      if (!opt.dropBlanks) out.push_back(para);
      pendingBreak = true;
      continue;
    }
    const bool newPara = entryList || startsIndented(raw) || pendingBreak || lastLineShort ||
                         startsEnumerated(para);
    pendingBreak = false;
    const size_t length = codepoints(para);
    // A line that ends in a hyphen is a continuation by definition, however
    // short it is: it must never read as the end of its paragraph, or the
    // de-hyphenation below loses its second half to a phantom break.
    const bool endsHyphen = !para.empty() && para.back() == '-';
    lastLineShort = !endsHyphen && shortEnoughToBeABreak(length, median, opt.unwrapPercent);
    if (!out.empty() && !newPara) {
      std::string& prev = out.back();
      // Never merge across a sentence end, and never append to a paragraph that
      // is nothing but one short line (a heading, a caption): that line has no
      // continuation to be the right edge of.
      const bool prevTerminal = endsSentence(prev);
      const bool prevAloneIsShort = codepoints(prev) <= kAbsoluteShortCp;
      // A row that carries a leader or a page number, or that announces itself
      // (第三章 …, 2. …), is one entry, finished: the next line is the NEXT entry or a
      // new paragraph, never this one's continuation. Without the enumerated half, a
      // contents page whose rows have no leaders merged in pairs (measured 3 -> 2).
      const bool prevIsEntryRow = isEntryRow(prev) || startsEnumerated(prev);
      const bool canMerge = !prevTerminal && !prevAloneIsShort && !prevIsEntryRow;
      if (canMerge) {
        prev = joinParagraphs(prev, para, opt.deHyphenate);
        continue;
      }
    }
    out.push_back(para);
  }
  return out;
}

}  // namespace pdf_merge
