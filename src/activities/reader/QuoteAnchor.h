#pragma once

// Locating an annotation's quoted text inside the CURRENT reflow lines.
//
// A PDF highlight stores the selected quote, not a character offset, because a
// reflowed PDF page has no stable text index across font/margin/size changes.  So
// "which lines does this annotation cover now?" is a matching question, and it has
// two traps that a naive substring search gets wrong:
//
//  * Lines have to be joined before searching.  MuPDF commonly ends a line inside a
//    sentence, and the reader's own reflow splits paragraphs differently again after
//    a re-layout, so the quote is almost never a single line.
//  * The join must not be trusted to be the same join the quote was captured under.
//    Turning 智能合并断行 on merges hard-wrapped CJK lines WITHOUT a space, so a
//    quote taken before the toggle contains a space that the page no longer has.
//    The exact pass then fails, and the caller's only fallback is the line numbers
//    recorded at creation -- which describe the OLD layout, and merging two lines
//    removes one.  The result is a highlight that silently drifts or disappears.
//
// Hence two passes: exact (whitespace-collapsed) first, so Latin word boundaries keep
// their precision whenever that is possible, and a whitespace-insensitive retry after
// it fails.  Pure and header-only so a host test can pin both passes and the ordering
// between them (test/quote_anchor/).
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace quote_anchor {

// Where a quote landed.  Lines are a half-open range; columns are byte offsets into the
// ORIGINAL source line and are -1 when the position was a synthesised separator.
struct Span {
  int firstLine = -1;
  int lastLine = -1;  // exclusive
  int firstCol = -1;
  int lastCol = -1;  // exclusive
  bool relaxed = false;
};

inline bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// ASCII letters/digits form words; CJK does not (a Chinese sentence is one unbroken
// run with no separators).
inline bool isLatinWordChar(const char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// True when joining `left` to `right` would cut through the middle of a Latin word.
// This is why a bare substring search is not good enough for re-anchoring: the quote
// "and extra" occurs verbatim inside the unrelated line "band extraordinary results",
// and a matched quote underlines its WHOLE line -- so the reader would see a highlight
// on a paragraph they never selected.  Guarding both ends catches it, while staying
// silent on CJK, where a quote legitimately starts mid-run because there is nothing to
// start "at".
inline bool cutsLatinWord(const char left, const char right) {
  return isLatinWordChar(left) && isLatinWordChar(right);
}

// Next whole-word occurrence of `needle` at or after `from`, or npos.
inline size_t nextWholeWord(const std::string& haystack, const std::string& needle, const size_t from) {
  if (needle.empty()) return std::string::npos;
  for (size_t at = haystack.find(needle, from); at != std::string::npos;
       at = haystack.find(needle, at + 1)) {
    const char before = at == 0 ? '\0' : haystack[at - 1];
    const char after = at + needle.size() >= haystack.size() ? '\0' : haystack[at + needle.size()];
    if (!cutsLatinWord(before, needle.front()) && !cutsLatinWord(needle.back(), after)) return at;
  }
  return std::string::npos;
}

// Which occurrence to use when the same sentence appears on the page more than once.
//
// A quoted highlight has to land on the sentence the reader ACTUALLY selected, and a page
// of dialogue or a repeated refrain contains the same line twice all the time -- taking the
// first match silently underlines a different passage, which is the exact failure the quote
// was stored to avoid. So occurrences are ranked by distance from the line recorded when the
// annotation was made, ties going to the earlier one (stable, and it keeps behaviour
// identical for the single-match case). No hint (< 0) or no usable hint means first match.
inline size_t bestOccurrence(const std::string& haystack, const std::string& needle,
                             const std::vector<int>& lineOfChar, const size_t needleLen, const int hintLine) {
  size_t best = std::string::npos;
  long bestDistance = 0;
  for (size_t at = nextWholeWord(haystack, needle, 0); at != std::string::npos;
       at = nextWholeWord(haystack, needle, at + 1)) {
    if (at + needleLen > lineOfChar.size()) break;  // cannot map it back to a line
    if (hintLine < 0) return at;                    // no preference: first match wins
    const long distance = std::labs(static_cast<long>(lineOfChar[at]) - hintLine);
    if (best == std::string::npos || distance < bestDistance) {
      best = at;
      bestDistance = distance;
    }
  }
  return best;
}

// First occurrence of `needle` in `haystack` that does not split a Latin word.
inline size_t findWholeWord(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return std::string::npos;
  for (size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + 1)) {
    const char before = at == 0 ? '\0' : haystack[at - 1];
    const char after = at + needle.size() >= haystack.size() ? '\0' : haystack[at + needle.size()];
    if (!cutsLatinWord(before, needle.front()) && !cutsLatinWord(needle.back(), after)) {
      return at;
    }
  }
  return std::string::npos;
}

// Collapse runs of whitespace to one space and trim, so a quote taken from joined
// lines still matches text joined here a different way.  A leading space is never
// emitted: `lineOfChar` must stay aligned with `page`, one entry per character.
inline std::string normalizeSpaces(const std::string& in) {
  std::string result;
  result.reserve(in.size());
  bool space = false;
  for (const unsigned char c : in) {
    if (isSpace(static_cast<char>(c))) {
      space = !result.empty();
      continue;
    }
    if (space) {
      result.push_back(' ');
      space = false;
    }
    result.push_back(static_cast<char>(c));
  }
  return result;
}

// Flatten the page's lines into one searchable string, remembering which line each
// character came from.  A separator space is credited to the line it precedes, which
// is arbitrary but harmless: both passes below only read the line of a non-space
// character.
inline void buildPage(const std::vector<std::string>& lines, std::string& page,
                      std::vector<int>& lineOfChar, std::vector<int>& colOfChar) {
  page.clear();
  lineOfChar.clear();
  colOfChar.clear();
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (!page.empty() && page.back() != ' ') {
      page.push_back(' ');
      lineOfChar.push_back(static_cast<int>(i));
      colOfChar.push_back(-1);  // the separator is not a character of the source line
    }
    for (size_t j = 0; j < line.size(); ++j) {
      const char c = line[j];
      if (isSpace(c)) {
        if (!page.empty() && page.back() != ' ') {
          page.push_back(' ');
          lineOfChar.push_back(static_cast<int>(i));
          colOfChar.push_back(-1);
        }
      } else {
        page.push_back(c);
        lineOfChar.push_back(static_cast<int>(i));
        colOfChar.push_back(static_cast<int>(j));
      }
    }
  }
}

// Returns the half-open line range [firstLine, lastLine) covering `quote`, or false
// when the quote is not on this page at all.  `relaxed` reports which pass matched, so
// a caller can log it: pass two firing at all means the layout changed under a
// previously exact annotation.
inline bool findSpan(const std::vector<std::string>& lines, const std::string& quote, Span& out,
                     int hintLine = -1);

inline bool findLineRange(const std::vector<std::string>& lines, const std::string& quote, int& firstLine,
                          int& lastLine, bool* relaxed = nullptr, int hintLine = -1) {
  Span span;
  span.relaxed = false;
  if (!findSpan(lines, quote, span, hintLine)) {
    if (relaxed) *relaxed = false;
    return false;
  }
  firstLine = span.firstLine;
  lastLine = span.lastLine;
  if (relaxed) *relaxed = span.relaxed;
  return true;
}

// The whole resolution, including where the quote starts and stops INSIDE the first and
// last line.  Kept as one function because underline precision is exactly as good as the
// match that produced it: two implementations would drift, and a highlight drawn from a
// different matcher than the one that found it is the bug pattern this file exists to
// avoid.
inline bool findSpan(const std::vector<std::string>& lines, const std::string& quote, Span& out,
                     const int hintLine) {
  out = Span{};
  const std::string needle = normalizeSpaces(quote);
  if (needle.empty()) return false;

  std::string page;
  std::vector<int> lineOfChar;
  std::vector<int> colOfChar;
  buildPage(lines, page, lineOfChar, colOfChar);
  if (page.empty()) return false;

  // Whole-word match, not a bare substring: see cutsLatinWord().
  const size_t at = bestOccurrence(page, needle, lineOfChar, needle.size(), hintLine);
  if (at != std::string::npos && at + needle.size() <= lineOfChar.size()) {
    out.firstLine = lineOfChar[at];
    out.lastLine = lineOfChar[at + needle.size() - 1] + 1;
    out.firstCol = colOfChar[at];
    out.lastCol = colOfChar[at + needle.size() - 1] + 1;  // exclusive
    return true;
  }

  // Pass two: compare with all whitespace removed on both sides.
  std::string dense;
  std::vector<int> denseLine;
  std::vector<int> denseCol;
  dense.reserve(page.size());
  denseLine.reserve(page.size());
  denseCol.reserve(page.size());
  for (size_t i = 0; i < page.size(); ++i) {
    if (isSpace(page[i])) continue;
    dense.push_back(page[i]);
    denseLine.push_back(lineOfChar[i]);
    denseCol.push_back(colOfChar[i]);
  }
  std::string denseNeedle;
  denseNeedle.reserve(needle.size());
  for (const char c : needle) {
    if (isSpace(c)) continue;
    denseNeedle.push_back(c);
  }
  if (dense.empty() || denseNeedle.empty()) return false;

  const size_t hit = bestOccurrence(dense, denseNeedle, denseLine, denseNeedle.size(), hintLine);
  if (hit == std::string::npos || hit + denseNeedle.size() > denseLine.size()) return false;
  out.firstLine = denseLine[hit];
  out.lastLine = denseLine[hit + denseNeedle.size() - 1] + 1;
  out.firstCol = denseCol[hit];
  out.lastCol = denseCol[hit + denseNeedle.size() - 1] + 1;  // exclusive
  out.relaxed = true;
  return true;
}

// Which bytes of `line` the annotation underlines, or false when the quote is not here.
// Boundary lines get the partial span -- EPUB underlines exactly what was selected, and
// a multi-line PDF highlight used to paint its first and last line end to end.  Interior
// lines of the quote are covered in full because the quote covers them in full.
// Returns -1/-1 columns when the source column is unknown (should not happen: a match
// never starts or ends on a separator), in which case the caller keeps the line's width.
// Returns kAbsent when the quote is not on this page at all (the caller may still fall
// back to stored line offsets), kElsewhere when it matched but not on this line (the
// caller must NOT underline here -- falling through would paint a stale offset range onto
// a page whose layout has since changed, which is the exact drift the quote exists to
// prevent), and kHere with the byte span set.
enum LineMatch { kAbsent = -1, kElsewhere = 0, kHere = 1 };

inline int spanForLine(const std::vector<std::string>& lines, const std::string& quote, const int line,
                       int& startByte, int& endByte, const int hintLine = -1) {
  startByte = -1;
  endByte = -1;
  if (line < 0 || line >= static_cast<int>(lines.size())) return kAbsent;
  Span span;
  if (!findSpan(lines, quote, span, hintLine)) return kAbsent;
  if (line < span.firstLine || line >= span.lastLine) return kElsewhere;

  const std::string& text = lines[static_cast<size_t>(line)];
  const int width = static_cast<int>(text.size());
  // A column of -1 means the position was a synthesised separator rather than a real
  // character of this line; out-of-range is possible only if the line changed under us.
  // Either way the safe answer is the whole line, never a clipped word.
  auto clampCol = [width](const int col, const int fallback) {
    return (col < 0 || col > width) ? fallback : col;
  };

  if (span.firstLine == span.lastLine - 1) {  // one line holds the whole quote
    startByte = clampCol(span.firstCol, 0);
    endByte = clampCol(span.lastCol, width);
    return kHere;
  }
  if (line == span.firstLine) {
    startByte = clampCol(span.firstCol, 0);
    endByte = width;
    return kHere;
  }
  if (line == span.lastLine - 1) {
    startByte = 0;
    endByte = clampCol(span.lastCol, width);
    return kHere;
  }
  startByte = 0;  // an interior line is covered in full by the quote
  endByte = width;
  return kHere;
}

}  // namespace quote_anchor
