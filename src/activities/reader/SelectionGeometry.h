#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// Pure selection geometry shared by the EPUB reader and the PDF reflow reader.
// A long-press selection is the same machine in both readers: hit the nearest
// word, grow a (line, word) range that never crosses its own ends, and place
// two drag handles at the range endpoints. Keeping that arithmetic here -- not
// copy-pasted per reader -- is what makes the PDF selection behave exactly like
// the EPUB one instead of being a second, drifting implementation.
//
// No renderer, no settings, no page model: every function is a pure function of
// its arguments so a host test can pin the geometry (see
// src/selection_geometry/).
namespace selection_geom {

// One selectable word's rectangle plus its position in the page's (line, word)
// grid. `start`/`end` are the word's byte range in the joined source text; the
// readers that need them (EPUB annotations) fill them in, and the ones that do
// not can leave them zero.
struct WordRect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  uint16_t line = 0;
  uint16_t word = 0;
  uint32_t start = 0;
  uint32_t end = 0;
};

// The half-open (start, end) word range, stored in the order the gesture
// produced it (start may be after end while the user drags leftward).
struct Range {
  uint16_t startLine = 0;
  uint16_t startWord = 0;
  uint16_t endLine = 0;
  uint16_t endWord = 0;
};

// Two endpoint handles, in page coordinates. Index 0 = start, 1 = end.
struct Handles {
  int16_t x[2] = {0, 0};
  int16_t y[2] = {0, 0};
  int16_t w[2] = {0, 0};
  int16_t h[2] = {0, 0};
};

inline constexpr int kHandleSize = 24;
inline constexpr int kHandleSlop = 12;

// Lexicographic (line, word) order.
inline bool before(uint16_t l1, uint16_t w1, uint16_t l2, uint16_t w2) {
  return l1 < l2 || (l1 == l2 && w1 < w2);
}

// True when `line`/`word` falls inside the range, regardless of which end the
// user is dragging. The endpoints are normalized first so a leftward drag that
// leaves start > end still highlights the whole span.
inline bool covers(const Range& r, uint16_t line, uint16_t word) {
  uint16_t aLine = r.startLine, aWord = r.startWord;
  uint16_t bLine = r.endLine, bWord = r.endWord;
  if (before(bLine, bWord, aLine, aWord)) {
    // cppcheck-suppress swapLocal
    std::swap(aLine, bLine);
    // cppcheck-suppress swapLocal
    std::swap(aWord, bWord);
  }
  if (line < aLine || line > bLine) return false;
  if (line == aLine && word < aWord) return false;
  if (line == bLine && word > bWord) return false;
  return true;
}

// Index of the word whose rectangle contains (x, y), else the nearest one by
// Manhattan distance to its box, else -1 for an empty list. Snapping to the
// nearest word is what makes a press on the gap between characters still do
// something instead of silently failing.
template <class Words>
int nearest(const Words& words, int x, int y) {
  int best = -1;
  int bestDist = 0x7FFFFFFF;
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& w = words[i];
    if (x >= w.x && x < w.x + w.w && y >= w.y && y < w.y + w.h) return static_cast<int>(i);
    const int dx = x < w.x ? w.x - x : (x > w.x + w.w ? x - (w.x + w.w) : 0);
    const int dy = y < w.y ? w.y - y : (y > w.y + w.h ? y - (w.y + w.h) : 0);
    if (dx + dy < bestDist) {
      bestDist = dx + dy;
      best = static_cast<int>(i);
    }
  }
  return best;
}

// Move the START end of the range to `word`, clamped so it never crosses the
// end (a crossing would flip the range under the finger). The start may sit on
// or before the end, never after it. Returns true only if the start moved.
inline bool moveStartTo(Range& r, uint16_t line, uint16_t word) {
  if (before(r.endLine, r.endWord, line, word)) return false;
  if (line == r.startLine && word == r.startWord) return false;
  r.startLine = line;
  r.startWord = word;
  return true;
}

// Move the END end of the range to `word`, clamped so it never crosses the
// start. The end may sit on or after the start, never before it. Returns true
// only if the end moved.
inline bool moveEndTo(Range& r, uint16_t line, uint16_t word) {
  if (before(line, word, r.startLine, r.startWord)) return false;
  if (line == r.endLine && word == r.endWord) return false;
  r.endLine = line;
  r.endWord = word;
  return true;
}

// Tap-extend: grow whichever end is away from the anchor toward `word`, leaving
// the anchored end fixed. This is what makes a tap left of the anchor extend
// left rather than translating the whole range.
inline void extendTo(Range& r, uint16_t anchorLine, uint16_t anchorWord, uint16_t line, uint16_t word) {
  if (before(line, word, anchorLine, anchorWord)) {
    r.startLine = line;
    r.startWord = word;
  } else {
    r.endLine = line;
    r.endWord = word;
  }
}

// Key-driven widening of a selection. The bar-closed state of a selection is
// widened with the d-pad/capacitive keys after a dictionary lookup (see
// "extend, look up again" in the readers), and both readers used to own that
// arithmetic. `anchorX` is the column a vertical step tries to keep.
enum class KeyStep { Left, Right, Up, Down };

// Index of the word at (line, word), or -1 when the word list no longer contains
// it. Callers must treat -1 as "do not move": defaulting to index 0 would yank the
// selection to the first word of the page whenever a rebuild renumbered the lines.
// Indexed and moved over any word-shaped element (`line`, `word`, `x`, `w`): the
// EPUB reader's rectangle carries extra fields (text, paragraph, gapBefore), so
// pinning these to WordRect would force a per-reader copy of the rule again --
// which is the whole thing this header exists to prevent.
template <class W>
inline int indexOf(const std::vector<W>& words, const uint16_t line, const uint16_t word) {
  for (size_t i = 0; i < words.size(); ++i) {
    if (words[i].line == line && words[i].word == word) return static_cast<int>(i);
  }
  return -1;
}

// The next focused word, or -1 when there is no move (either page edge, or a
// neighbouring line that does not exist). Left/Right walk reading order; Up/Down
// jump a line and land on the nearest column, which is what keeps a multi-line
// selection from drifting to the line start on every press.
template <class W>
inline int stepWord(const std::vector<W>& words, const int from, const KeyStep step, const int anchorX) {
  if (from < 0 || static_cast<size_t>(from) >= words.size()) return -1;
  if (step == KeyStep::Left) return from == 0 ? -1 : from - 1;
  if (step == KeyStep::Right) {
    const size_t next = static_cast<size_t>(from) + 1;
    return next >= words.size() ? -1 : static_cast<int>(next);
  }
  const int targetLine = static_cast<int>(words[static_cast<size_t>(from)].line) +
                         (step == KeyStep::Up ? -1 : 1);
  if (targetLine < 0) return -1;
  int best = -1;
  int bestDistance = 0x7FFFFFFF;
  for (size_t i = 0; i < words.size(); ++i) {
    if (words[i].line != static_cast<uint16_t>(targetLine)) continue;
    const int centre = words[i].x + words[i].w / 2;
    const int distance = centre > anchorX ? centre - anchorX : anchorX - centre;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = static_cast<int>(i);
    }
  }
  return best;
}

// First/last covered rectangles, so the caller can place handles and the action
// bar without rescanning. `found` is false when the range covers no word.
struct Ends {
  bool found = false;
  int firstX = 0, firstY = 0, firstW = 0, firstH = 0;
  int lastX = 0, lastY = 0, lastW = 0, lastH = 0;
};

template <class Words>
Ends coveredEnds(const Words& words, const Range& r) {
  Ends e;
  for (const auto& w : words) {
    if (!covers(r, w.line, w.word)) continue;
    if (!e.found) {
      e.found = true;
      e.firstX = w.x;
      e.firstY = w.y;
      e.firstW = w.w;
      e.firstH = w.h;
    }
    e.lastX = w.x;
    e.lastY = w.y;
    e.lastW = w.w;
    e.lastH = w.h;
  }
  return e;
}

// The two endpoint handle rectangles. Horizontal writing puts them to the left
// of the first word and right of the last word, vertically centred on the line;
// vertical writing runs the range down a column, so they sit above and below.
// Handles are clamped into [2, screenW-2] so a range at either edge still shows
// a grabbable handle instead of pushing it off-screen.
inline Handles handleGeometry(const Ends& ends, bool vertical, int screenW, int lineHeight) {
  Handles h;
  if (!ends.found) return h;
  const int k = kHandleSize;
  int hx[2];
  int hy[2];
  if (vertical) {
    hx[0] = ends.firstX + (ends.firstW - k) / 2;
    hx[1] = ends.lastX + (ends.lastW - k) / 2;
    hy[0] = ends.firstY - k - 4;
    hy[1] = ends.lastY + ends.lastH + 4;
  } else {
    hx[0] = ends.firstX - k - 4;
    hx[1] = ends.lastX + ends.lastW + 4;
    hy[0] = ends.firstY + (lineHeight - k) / 2;
    hy[1] = ends.lastY + (lineHeight - k) / 2;
  }
  for (int i = 0; i < 2; ++i) {
    int x = hx[i];
    if (x < 2) x = 2;
    if (x + k > screenW - 2) x = screenW - 2 - k;
    h.x[i] = static_cast<int16_t>(x);
    h.y[i] = static_cast<int16_t>(hy[i]);
    h.w[i] = static_cast<int16_t>(k);
    h.h[i] = static_cast<int16_t>(k);
  }
  return h;
}

// Which handle (0 or 1) the point (x, y) is on, or -1. The hit box is the
// handle grown by `slop` on every side -- the handle is only 24 px, far below
// the comfortable touch target on this panel.
inline int hitHandle(const Handles& h, int x, int y, int slop = kHandleSlop) {
  for (int i = 0; i < 2; ++i) {
    if (h.w[i] <= 0) continue;
    if (x >= h.x[i] - slop && x < h.x[i] + h.w[i] + slop && y >= h.y[i] - slop && y < h.y[i] + h.h[i] + slop) {
      return i;
    }
  }
  return -1;
}

}  // namespace selection_geom
