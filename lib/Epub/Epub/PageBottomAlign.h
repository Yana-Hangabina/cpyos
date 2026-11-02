#pragma once

#include <algorithm>
#include <cstdint>

// Vertical page bottom alignment (upstream 8418166c + 9a7e6b35).
//
// The layout engine emits pages top-aligned: the last text line can stop well
// above the content bottom on pages that break early. With the reader setting
// on, every non-final text-only page redistributes its leftover vertical space
// between the lines so the first line stays put and the last one reaches the
// content bottom.
//
// Kept as a pure function (no Page/parser types) so the distribution and its
// safety cap are host-testable; the parser only applies the returned shifts.
namespace pageBottomAlign {

struct Plan {
  // Extra pixels added to every inter-line gap.
  int16_t extraPerGap = 0;
  // The first `remainder` gaps get one extra pixel (integer division remainder).
  int16_t remainder = 0;
  bool distribute = false;
};

// viewportHeight   content box height the page is laid out in
// contentBottom    y just past the last text line pushed onto the page (the
//                  layout cursor without trailing paragraph spacing)
// lineCount        number of text lines on the page (>= 2 to be distributed)
// lineHeight       height of the most recent line, the cap reference
//
// Returns distribute=false when the page is full (nothing to spread), has fewer
// than two lines, or the math would not move anything.
inline Plan plan(const int viewportHeight, const int contentBottom, const int lineCount, const int lineHeight) {
  Plan out;
  if (lineCount < 2) return out;
  const int leftover = viewportHeight - contentBottom;
  if (leftover <= 0) return out;
  const int gapCount = lineCount - 1;
  // Safety cap: no single gap may grow by more than half a line height. A
  // sparse page would otherwise get one enormous gap in the middle; instead the
  // stretch stops and the rest of the space stays at the bottom. Ordinary pages
  // have a small leftover and never reach the cap, so their last line still
  // sits flush with the content bottom.
  const int maxExtraPerGap = std::max(lineHeight / 2, 1);
  const int rawExtraPerGap = leftover / gapCount;
  if (rawExtraPerGap <= 0) return out;
  if (rawExtraPerGap <= maxExtraPerGap) {
    out.extraPerGap = static_cast<int16_t>(rawExtraPerGap);
    out.remainder = static_cast<int16_t>(leftover % gapCount);
  } else {
    out.extraPerGap = static_cast<int16_t>(maxExtraPerGap);
    out.remainder = 0;
  }
  out.distribute = true;
  return out;
}

// Downward shift, in pixels, for the line at `lineIndex` (0-based).
inline int shiftFor(const Plan& p, const int lineIndex) {
  if (!p.distribute || lineIndex <= 0) return 0;
  return lineIndex * p.extraPerGap + std::min(lineIndex, static_cast<int>(p.remainder));
}

}  // namespace pageBottomAlign
