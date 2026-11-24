#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

/**
 * Page-break decisions for the block flow (layout milestone M3, upstream a6fd569f).
 *
 * Free of Page/TextBlock types so the host test can drive the shipped logic: the
 * parser supplies only `canCarry()` (are the page's trailing n elements text lines
 * that may move?) and applies the returned carry.
 */
namespace page_break_policy {

// A paragraph never leaves fewer than this many of its lines at a page bottom...
constexpr uint8_t kOrphanLines = 2;
// ...and never pushes fewer than this many to the top of the next page.
constexpr uint8_t kWidowLines = 2;
// Both a paragraph tail and a heading may move; bounded so a page can never empty.
constexpr size_t kMaxCarriedLines = 6;

struct State {
  // Lines of the block being laid out that are already on the page.
  uint8_t paragraphLinesOnPage = 0;
  // Lines that block still has after the one being placed; -1 = not known yet.
  int linesAfterCurrent = -1;
  // Lines of the heading(s) right before this block, waiting for its first line.
  uint8_t keepWithNextLines = 0;
};

struct Carry {
  size_t lines = 0;            // trailing page elements to move to the next page
  uint8_t paragraphLines = 0;  // how many of those are the block's own lines
};

// A heading holds its lines back until the following block places one of its own.
[[nodiscard]] inline uint8_t armKeepWithNext(const bool blockKeepsWithNext, const uint8_t pending,
                                             const uint8_t linesLaidOut) {
  if (!blockKeepsWithNext) return 0;
  return static_cast<uint8_t>(std::min<size_t>(kMaxCarriedLines, pending + linesLaidOut));
}

// Decide how many trailing text lines the page hands over when the line being
// placed does not fit. `canCarry(n)` answers whether the page's trailing n
// elements may move (they must be text lines, and never all of them).
template <typename CanCarry>
[[nodiscard]] Carry decide(const State state, const CanCarry& canCarry) {
  Carry carry;
  if (state.paragraphLinesOnPage > 0 && state.paragraphLinesOnPage < kOrphanLines) {
    // Fewer than kOrphanLines of the block's lines would sit at the page bottom.
    carry.paragraphLines = state.paragraphLinesOnPage;
  } else if (state.paragraphLinesOnPage >= kWidowLines && state.linesAfterCurrent == 0) {
    // The line being placed is the block's last, so it would open the next page
    // alone; carry one line with it, or both when the block only has two here.
    carry.paragraphLines = state.paragraphLinesOnPage >= 3 ? 1 : 2;
  }

  size_t lines = carry.paragraphLines;
  if (state.keepWithNextLines > 0 && carry.paragraphLines == state.paragraphLinesOnPage) {
    lines += state.keepWithNextLines;
  }
  if (lines <= kMaxCarriedLines && canCarry(lines)) {
    carry.lines = lines;
    return carry;
  }
  // Oversized combined carry (a heading plus a paragraph tail): keep the heading
  // where it is and only move the paragraph lines.
  if (carry.paragraphLines <= kMaxCarriedLines && canCarry(carry.paragraphLines)) {
    carry.lines = carry.paragraphLines;
    return carry;
  }
  return {};
}

}  // namespace page_break_policy
