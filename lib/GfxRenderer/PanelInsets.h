#pragma once

// Panel-edge geometry: the per-row half of the safe-area rules.
//
// A board's *viewable* aperture is smaller than its glass. Two different
// things eat the edge, and they are not the same shape:
//
//  * the printed bezel, which overlaps the panel by a fixed number of pixels
//    on each side — board data (`BoardConfig::ViewableInsets`, read through
//    `GfxRenderer::getOrientedViewableTRBL`), constant down the whole edge;
//  * the aperture's rounded corners, where the seam between bezel and glass is
//    an arc. The Read Pico's inner corner is rounded (see
//    `docs/engineering/read-pico.md`); a square panel has radius 0.
//
// The straight-edge inset folds into a rectangle; the corner cannot, because
// what it steals depends on how deep the ink is. `cornerCut` answers that for
// one row, so chrome that hugs a corner keeps its own geometry and only nudges
// the end coordinate that would otherwise land in the arc.
//
// Both functions are `constexpr` and libm-free (integer isqrt), so a host test
// can pin them without a toolchain. Both return exactly 0 at `radius == 0`,
// which is what lets every caller be written as `max(existing, ...)`: on a
// square panel the expression is provably the value it replaced, so the
// shipped X3/X4/Read Pico builds stay byte-identical.
namespace panel_insets {

// Floor of the square root of a non-negative integer, without <cmath>: the
// corner maths runs on whole pixels, and the host test pins exact squares.
constexpr int isqrt(const int value) {
  if (value <= 0) return 0;
  int root = 0;
  int rest = value;
  int bit = 1 << 30;  // highest power of four <= value
  while (bit > rest) bit >>= 2;
  while (bit != 0) {
    if (rest >= root + bit) {
      rest -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

// Horizontal space the rounded corner steals on a row `depth` px in from the
// edge the corner belongs to (0 = the outermost visible row).
//
// The corner is a quarter circle of `radius`; at depth d the visible width
// starts at `radius - sqrt(radius^2 - (radius - d)^2)` from the side edge.
//   depth <= 0      -> the whole radius (ink sits on the edge)
//   depth >= radius -> 0 (past the arc, a straight edge again)
//   radius <= 0     -> 0 (square panel: nothing is stolen)
constexpr int cornerCut(const int radius, const int depth) {
  if (radius <= 0) return 0;
  if (depth <= 0) return radius;
  if (depth >= radius) return 0;
  const int r = radius;
  const int d = depth;
  return r - isqrt(r * r - (r - d) * (r - d));
}

// The clearance a side coordinate needs: the straight-edge inset, or the
// corner's cut when that is bigger. Identical to `inset` at radius 0.
constexpr int sideClearance(const int inset, const int radius, const int depth) {
  const int cut = cornerCut(radius, depth);
  return cut > inset ? cut : inset;
}

}  // namespace panel_insets
