#pragma once

// Upright 1-bpp drawing for the CPY mascot assets. The freeink::Icon format is
// NOT pre-rotated (row-major, MSB-first, bit==0 = ink), while the legacy
// GfxRenderer::drawIcon() applies a 90-degree mapping for old callers — the
// duck bits must be plotted directly through drawPixel instead.
//
// Consumers: the CPY theme (title band, splash, progress bar, empty state) and
// the DuckSleep standby face.

#include <GfxRenderer.h>

#include "cpy_duck_assets.h"

namespace cpy_duck {

inline void draw(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y) {
  const int size = icon.w;  // duck assets are square
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    for (int column = 0; column < size; ++column) {
      const uint8_t byte = icon.bits[row * rowBytes + column / 8];
      if (((byte >> (7 - column % 8)) & 1U) == 0) renderer.drawPixel(x + column, y + row, true);
    }
  }
}

// Same bits, flipped left-to-right: the swimming duck's art faces left, the
// progress bar swims right.
inline void drawMirroredX(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y) {
  const int size = icon.w;
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    for (int column = 0; column < size; ++column) {
      const int sourceColumn = size - 1 - column;
      const uint8_t byte = icon.bits[row * rowBytes + sourceColumn / 8];
      if (((byte >> (7 - sourceColumn % 8)) & 1U) == 0) renderer.drawPixel(x + column, y + row, true);
    }
  }
}

// Quarter-density plot for backdrops: only every second row and column gets
// ink, which reads as a light grey pattern on the 1-bpp panel.
inline void drawSparse(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y) {
  const int size = icon.w;
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row += 2) {
    for (int column = 0; column < size; column += 2) {
      const uint8_t byte = icon.bits[row * rowBytes + column / 8];
      if (((byte >> (7 - column % 8)) & 1U) == 0) renderer.drawPixel(x + column, y + row, true);
    }
  }
}

}  // namespace cpy_duck
