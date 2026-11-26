#pragma once

#include <algorithm>

// Vertical plan for the Find-books page (pure integer math, host-testable).
//
// The page is a fixed stack: an arrival row, then one source card per entry.
// The authored geometry fits a 100% body (the design's 184 px cards). Above
// 100% the UI viewport shrinks while the type keeps its scaled line heights,
// so the same stack no longer fits the body and would be drawn over the tab
// bar. planFor() reproduces the authored plan whenever it fits and otherwise
// shrinks only *flexible* blocks: page margins, the arrival row's height, the
// gaps, the card insets, the inner gap and the pill capsule. The title/status
// block is the floor -- type is never squeezed -- and the returned plan never
// draws outside `band`.
//
// One plan drives the drawing and the hit rects the page records, so there is
// no second copy of this arithmetic (see docs/engineering/ui-coordinate-contract.md,
// "Fixed-height screens").
namespace find_books_plan {

// Authored (design-space) values; the caller supplies them so this header stays
// free of the theme and font tables.
struct Design {
  int topMargin;    // body top margin, and the matching bottom margin
  int arrivalRow;   // the upload row
  int gap;          // arrival row -> first card, and between cards
  int cardHeight;   // design card height
  int seal;         // source glyph square
  int pillHeight;   // capsule height
  int padY;         // card top/bottom inset
  int innerGap;     // header block -> pill row
};

struct Plan {
  int topPad;
  int arrivalH;
  int gap;
  int cardH;
  int padY;
  int innerGap;
  int pillH;
  int seal;
};

// One gap follows the arrival row and one separates each pair of cards, so a
// page of `count` cards spends `count` gaps in total.
inline int stackHeight(const Plan& p, const int count) {
  const int n = std::max(1, count);
  return 2 * p.topPad + p.arrivalH + n * p.gap + n * p.cardH;
}

// The tallest block the card may not shrink below: the seal glyph or the
// title/status lines, whichever is taller.
inline int headerBlock(const Plan& p, const int typeBlock) { return std::max(p.seal, typeBlock); }

// Interior of one card: insets, header block, inner gap, capsule. Must never
// exceed the card, or the capsules would ride over the title block.
inline int interiorHeight(const Plan& p, const int typeBlock) {
  return 2 * p.padY + headerBlock(p, typeBlock) + p.innerGap + p.pillH;
}

inline Plan planFor(const Design& d, const int band, const int count, const int titleLine, const int detailLine) {
  const int n = std::max(1, count);
  const int typeBlock = titleLine + 3 + detailLine;

  // Authored: today's geometry, byte for byte. It is what every 100% page and
  // every body tall enough to hold four design cards gets.
  Plan authored{d.topMargin, d.arrivalRow, d.gap, 0, d.padY, d.innerGap, d.pillHeight, d.seal};
  const int authoredMin = d.padY * 2 + std::max(d.seal, typeBlock) + d.innerGap + d.pillHeight;
  const int authoredShare = (band - 2 * d.topMargin - d.arrivalRow - n * d.gap) / n;
  authored.cardH = std::min(d.cardHeight, std::max(authoredMin, authoredShare));
  if (stackHeight(authored, n) <= band) return authored;

  // Compressed: the page-level flexible blocks give way first, then the cards
  // share exactly what the band has left.
  Plan p{d.topMargin, d.arrivalRow, d.gap, 0, d.padY, d.innerGap, d.pillHeight, d.seal};
  p.topPad = std::min(d.topMargin, 8);
  p.gap = std::min(d.gap, 10);
  p.arrivalH = std::min(d.arrivalRow, 72);
  // A body too short even for those reserves at most one pixel per card and
  // takes the difference out of the fixed blocks, so nothing is drawn below the
  // body on a small panel either.
  const int fixedBlocks = 2 * p.topPad + p.arrivalH + n * p.gap;
  if (fixedBlocks > band - n) {
    p.topPad = 0;
    p.gap = std::max(0, std::min(p.gap, (band - n) / (2 * n)));
    p.arrivalH = std::max(0, std::min(p.arrivalH, band - n - n * p.gap));
  }
  p.cardH = std::max(1, std::min(d.cardHeight, (band - 2 * p.topPad - p.arrivalH - n * p.gap) / n));

  // Split the slack above (header block + minimum capsule) between the insets,
  // the inner gap and the capsule, in that order.
  const int minPill = std::clamp(detailLine + 8, 20, d.pillHeight);
  const int slack = std::max(0, p.cardH - (std::max(p.seal, typeBlock) + minPill));
  p.padY = std::clamp(slack / 4, 0, std::min(d.padY, 6));
  p.innerGap = std::clamp(slack - 2 * p.padY, 0, std::min(d.innerGap, 4));
  p.pillH = std::clamp(p.cardH - 2 * p.padY - headerBlock(p, typeBlock) - p.innerGap, minPill, d.pillHeight);
  // A card too short for seal + capsule drops the glyph first; the type block
  // and the capsule are what the card is for.
  const int sealRoom = p.cardH - 2 * p.padY - p.innerGap - p.pillH;
  if (sealRoom < p.seal) p.seal = std::clamp(sealRoom, 16, p.seal);
  return p;
}

}  // namespace find_books_plan
