#pragma once

// Which words belong to an existing annotation.
//
// The PDF reader cannot store text offsets the way the EPUB reader does: a PDF
// page re-extracted after a font/margin change has no stable character index, so
// a PDF highlight carries the selected QUOTE and is re-anchored by finding that
// quote in the current reflow lines.  That makes "press inside an existing
// highlight and re-select its whole range" a question about intervals on lines,
// not about the renderer -- so it lives here, pure and header-only, where a host
// test can pin it (test/annotation_anchor/).
//
// The reader supplies `spanOf(line, startOut, endOut)`, which is exactly what it
// already computes to draw the underline.  Drawing and hit-testing therefore
// share one matcher and cannot drift apart.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace anno_anchor {

struct Word {
  uint16_t line = 0;
  uint32_t start = 0;  // byte offset within that line's text
  uint32_t end = 0;    // exclusive
};

constexpr uint32_t kPageMarkerStart = 0;
constexpr uint32_t kPageMarkerEnd = 1;

// A PDF annotation whose range is exactly line 0 (exclusive end 1) is the
// whole-page marker written by the page-note path: "a note about this page", not
// "a note about this text".  Every test that separates the two must go through
// this.  Treating any annotation on a page as the page marker let the page-note
// editor prefill from an unrelated selection note -- and the old save path then
// deleted every annotation on the page, highlights included.
inline bool isWholePageMarker(const uint32_t startOffset, const uint32_t endOffset) {
  return startOffset == kPageMarkerStart && endOffset == kPageMarkerEnd;
}

// Half-open overlap: a word that begins exactly where a span ends is NOT inside
// it.  Getting this wrong makes a press just past a highlight re-select it, and
// a press on the first word of the next paragraph do nothing.
inline bool overlaps(const uint32_t wordStart, const uint32_t wordEnd, const int spanStart,
                     const int spanEnd) {
  if (spanStart < 0 || spanEnd <= spanStart) return false;
  return static_cast<int>(wordStart) < spanEnd && static_cast<int>(wordEnd) > spanStart;
}

// Given the tapped word index, return the annotation index that covers it (as
// reported by `coverOf`, -1 for none) and widen to every word of this screen that
// the same annotation covers, in reading order.  `coverOf` is expected to be
// stable across calls for the same annotation.
template <class CoverOf>
int rangeForWord(const std::vector<Word>& words, const size_t tapped, CoverOf coverOf, int& firstOut,
                 int& lastOut) {
  firstOut = -1;
  lastOut = -1;
  if (tapped >= words.size()) return -1;
  const int annotation = coverOf(tapped);
  if (annotation < 0) return -1;
  for (size_t i = 0; i < words.size(); ++i) {
    if (coverOf(i) != annotation) continue;
    if (firstOut < 0) firstOut = static_cast<int>(i);
    lastOut = static_cast<int>(i);
  }
  return firstOut < 0 ? -1 : annotation;
}

}  // namespace anno_anchor
