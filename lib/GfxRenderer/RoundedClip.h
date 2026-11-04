#pragma once

// Corner geometry for rounded-rect scissoring. Shared by
// GfxRenderer::RoundedClipScope and the host test, so the two can never drift:
// a mirroring mistake here only shows up as content poking through a card
// corner on the panel, which is expensive to notice and easy to get wrong.
namespace rounded_clip {

// True for a pixel of the corner square that lies OUTSIDE the rounded outline
// (the "wedge" the scope saves and puts back). Mirrors the test that
// GfxRenderer::maskRoundedRectOutsideCorners() has always used.
inline constexpr bool wedgePixel(const int dx, const int dy, const int radius) {
  if (radius <= 0) return false;
  const int rr = radius - 1;
  const int tx = rr - dx;
  const int ty = rr - dy;
  return tx * tx + ty * ty > rr * rr;
}

// Corner 0 = top-left, 1 = top-right, 2 = bottom-left, 3 = bottom-right.
// dx/dy are measured from the rectangle's own top/left edge, exactly as
// maskRoundedRectOutsideCorners() draws them: the right and bottom corners
// MIRROR the offsets. Using the corner square's own left/top instead would
// restore the wrong wedge (the top-right would clip its inner half and leave
// the outer half painted).
inline constexpr void cornerPixel(const int corner, const int dx, const int dy, const int x, const int y,
                                  const int width, const int height, int* px, int* py) {
  switch (corner) {
    case 0:
      *px = x + dx;
      *py = y + dy;
      break;
    case 1:
      *px = x + width - 1 - dx;
      *py = y + dy;
      break;
    case 2:
      *px = x + dx;
      *py = y + height - 1 - dy;
      break;
    default:
      *px = x + width - 1 - dx;
      *py = y + height - 1 - dy;
      break;
  }
}

}  // namespace rounded_clip
