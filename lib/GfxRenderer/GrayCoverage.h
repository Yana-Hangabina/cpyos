#pragma once

// Coverage helpers for the 16-level (4bpp) canvas.
//
// The 1-bit path stays Bresenham/integer by design: on a 1bpp panel there is
// nothing between ink and paper. The 16-level path has 16 tones, so geometry
// that is not axis-aligned (diagonal lines, arcs, icon outlines) can carry a
// fractional pixel. These are pure functions with no HAL dependency so the
// host test can check the areas they produce.
//
// Pixel centres are at integer coordinates, matching GfxRenderer::drawPixel.
// A 1 px stroke therefore has halfWidth 0.5 and covers exactly one pixel row
// when it is axis-aligned.

#include <cmath>
#include <cstdint>

namespace gray_aa {

inline float clamp01(const float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Box-filter approximation of how much of the pixel at (px,py) is inside a
// segment of half-width `halfWidth` centred on (x1,y1)-(x2,y2).
inline float lineCoverage(const float px, const float py, const float x1, const float y1, const float x2,
                          const float y2, const float halfWidth) {
  const float vx = x2 - x1;
  const float vy = y2 - y1;
  const float wx = px - x1;
  const float wy = py - y1;
  const float len2 = vx * vx + vy * vy;
  float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  const float dx = px - (x1 + t * vx);
  const float dy = py - (y1 + t * vy);
  return clamp01(halfWidth + 0.5f - std::sqrt(dx * dx + dy * dy));
}

// Area of the pixel at (px,py) inside a disk of radius r. The +0.5 puts the
// shape's edge half a pixel outside the outermost covered centre, so a disk of
// radius r has the same extent as the integer-drawn version.
inline float diskCoverage(const float px, const float py, const float cx, const float cy, const float r) {
  const float dx = px - cx;
  const float dy = py - cy;
  return clamp01(r + 0.5f - std::sqrt(dx * dx + dy * dy));
}

// Annulus used by drawArc(): outer edge at rOuter, inner edge at rInner.
inline float bandCoverage(const float px, const float py, const float cx, const float cy, const float rOuter,
                          const float rInner) {
  return clamp01(diskCoverage(px, py, cx, cy, rOuter) - diskCoverage(px, py, cx, cy, rInner));
}

// One mask texel as 0/1. `inverted` matches the freeink::Icon convention
// (bit 0 = ink); clamping keeps bilinear samples at the border well defined.
inline float maskTexel(const uint8_t* bits, const int rowBytes, const int w, const int h, int x, int y,
                       const bool inverted) {
  if (x < 0) x = 0;
  else if (x >= w) x = w - 1;
  if (y < 0) y = 0;
  else if (y >= h) y = h - 1;
  const uint8_t byte = bits[y * rowBytes + (x >> 3)];
  const bool bit = ((byte >> (7 - (x & 7))) & 1u) != 0;
  return (inverted ? !bit : bit) ? 1.0f : 0.0f;
}

// Bilinear reconstruction of the thresholded mask. At 1:1 it reproduces the
// texel; between texels it turns the hard staircase into a one-pixel ramp.
inline float maskBilinear(const uint8_t* bits, const int rowBytes, const int w, const int h, const float u,
                          const float v, const bool inverted) {
  const float x = u - 0.5f;
  const float y = v - 0.5f;
  // u,v are never negative here, so x,y >= -0.5: truncation is floor except
  // for the one possible negative value. Avoids a libm floorf per tap.
  int x0 = static_cast<int>(x);
  int y0 = static_cast<int>(y);
  if (static_cast<float>(x0) > x) --x0;
  if (static_cast<float>(y0) > y) --y0;
  const float fx = x - static_cast<float>(x0);
  const float fy = y - static_cast<float>(y0);
  const float a = maskTexel(bits, rowBytes, w, h, x0, y0, inverted);
  const float b = maskTexel(bits, rowBytes, w, h, x0 + 1, y0, inverted);
  const float c = maskTexel(bits, rowBytes, w, h, x0, y0 + 1, inverted);
  const float d = maskTexel(bits, rowBytes, w, h, x0 + 1, y0 + 1, inverted);
  return (a * (1.0f - fx) + b * fx) * (1.0f - fy) + (c * (1.0f - fx) + d * fx) * fy;
}

// Box-filtered coverage of a destination pixel whose footprint in source
// coordinates is [u0,u0+du] x [v0,v0+dv]. 4x4 samples are enough for the
// 18..56 px icon sizes this firmware uses and stay well inside the frame
// budget.
inline float maskBoxCoverage(const uint8_t* bits, const int rowBytes, const int w, const int h, const float u0,
                             const float v0, const float du, const float dv, const bool inverted) {
  constexpr int kSamples = 4;
  float sum = 0.0f;
  for (int sy = 0; sy < kSamples; ++sy) {
    const float v = v0 + (sy + 0.5f) * dv / kSamples;
    for (int sx = 0; sx < kSamples; ++sx) {
      const float u = u0 + (sx + 0.5f) * du / kSamples;
      sum += maskBilinear(bits, rowBytes, w, h, u, v, inverted);
    }
  }
  return sum / (kSamples * kSamples);
}

// Coverage for icon masks. Bilinear reconstruction treats each texel as a
// sample at its centre, which pulls every outlined stroke in by half a texel
// on each side; a gentle S-curve recovers a little of the inked weight (a
// solid texel stays 1, a 0.75 edge becomes 0.80) without turning the
// fractional corners back into hard steps.
inline float maskCurve(const float coverage) {
  const float c = clamp01(coverage);
  return c * (1.25f - 0.25f * c);
}

// Exact area average of the thresholded mask over a destination footprint.
// Used whenever a destination pixel spans more than one texel (native is left
// to the bilinear path so it can anti-alias the outline): a 2:1 icon reads a
// 2x2 block instead of 4x4 bilinear taps per supersample, which is what keeps
// a dashboard repaint inside the frame budget.
inline float maskAreaCoverage(const uint8_t* bits, const int rowBytes, const int w, const int h, const float u0,
                              const float v0, const float du, const float dv, const bool inverted) {
  const float u1 = u0 + du;
  const float v1 = v0 + dv;
  // u0/v0 are >= 0, so truncation is floor. ceil is the integer part plus one
  // when the fractional part is non-zero; avoids four libm calls per pixel.
  int x0 = static_cast<int>(u0);
  int x1 = static_cast<int>(u1);
  int y0 = static_cast<int>(v0);
  int y1 = static_cast<int>(v1);
  if (static_cast<float>(x1) < u1) ++x1;
  if (static_cast<float>(y1) < v1) ++y1;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > w) x1 = w;
  if (y1 > h) y1 = h;
  if (x0 >= x1 || y0 >= y1) return 0.0f;
  float sum = 0.0f;
  for (int y = y0; y < y1; ++y) {
    const float oy = std::min(v1, static_cast<float>(y + 1)) - std::max(v0, static_cast<float>(y));
    if (oy <= 0.0f) continue;
    for (int x = x0; x < x1; ++x) {
      const float ox = std::min(u1, static_cast<float>(x + 1)) - std::max(u0, static_cast<float>(x));
      if (ox <= 0.0f) continue;
      if (maskTexel(bits, rowBytes, w, h, x, y, inverted) > 0.5f) sum += ox * oy;
    }
  }
  const float area = du * dv;
  return area > 0.0f ? maskCurve(sum / area) : 0.0f;
}

inline float maskCoverage(const uint8_t* bits, const int rowBytes, const int w, const int h, const float u0,
                          const float v0, const float du, const float dv, const bool inverted) {
  if (du > 1.0f || dv > 1.0f) return maskAreaCoverage(bits, rowBytes, w, h, u0, v0, du, dv, inverted);
  return maskCurve(maskBoxCoverage(bits, rowBytes, w, h, u0, v0, du, dv, inverted));
}

// True when every texel in the 3x3 neighbourhood has the same value. A native
// resolution icon can then skip supersampling: interior/solid areas are
// already 0 or 1, and only the outline pays for the 4x4 taps.
inline bool maskNeighbourhoodUniform(const uint8_t* bits, const int rowBytes, const int w, const int h, int x,
                                     int y, const bool inverted) {
  const float c = maskTexel(bits, rowBytes, w, h, x, y, inverted);
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if ((dx != 0 || dy != 0) && maskTexel(bits, rowBytes, w, h, x + dx, y + dy, inverted) != c) return false;
    }
  }
  return true;
}

// Composite `coverage` of an ink tone over the current level. Ink may be black
// (0) or white (15), so the same helper handles inverted UI rows.
inline uint8_t blendLevel(const uint8_t oldLevel, const float coverage, const uint8_t inkLevel) {
  // 0..15 fixed point; integer only so the per-pixel path never calls libm.
  const int c = static_cast<int>(clamp01(coverage) * 15.0f + 0.5f);
  const int delta = static_cast<int>(inkLevel) - static_cast<int>(oldLevel);
  int merged = static_cast<int>(oldLevel) + (delta * c) / 15;
  if (merged < 0) merged = 0;
  if (merged > 15) merged = 15;
  return static_cast<uint8_t>(merged);
}

}  // namespace gray_aa
