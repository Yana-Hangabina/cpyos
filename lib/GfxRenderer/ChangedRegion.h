#pragma once

#include <cstddef>
#include <cstdint>

// Bounding box of the pixels that differ between two equally-sized 1bpp
// framebuffers in PHYSICAL panel-memory layout: MSB-first rows of `strideBytes`
// bytes, `rows` of them. Extracted so the reader's per-minute status-bar clock
// tick (A2) can be unit-tested on the host without the display stack.
//
// Coordinates are framebuffer-native (x = byteIndex * 8 + bitIndex), which is
// what the display layer's windowed push wants. `valid` is false when the
// buffers are identical (nothing to push) or the arguments make no sense.
namespace readerutil {

struct PixelRegion {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool valid = false;
};

inline PixelRegion changedRegion(const uint8_t* before, const uint8_t* after, const int rows, const int strideBytes) {
  PixelRegion region;
  if (!before || !after || rows <= 0 || strideBytes <= 0) return region;

  int minX = strideBytes * 8;
  int minY = rows;
  int maxX = -1;
  int maxY = -1;
  for (int row = 0; row < rows; ++row) {
    const uint8_t* a = before + static_cast<size_t>(row) * strideBytes;
    const uint8_t* b = after + static_cast<size_t>(row) * strideBytes;
    bool rowDiffers = false;
    int first = -1;
    int last = -1;
    for (int i = 0; i < strideBytes; ++i) {
      if (a[i] == b[i]) continue;
      rowDiffers = true;
      if (first < 0) first = i;
      last = i;
    }
    if (!rowDiffers) continue;
    if (row < minY) minY = row;
    if (row > maxY) maxY = row;
    const int firstBit = first * 8;
    const int lastBit = last * 8 + 7;
    if (firstBit < minX) minX = firstBit;
    if (lastBit > maxX) maxX = lastBit;
  }

  if (maxX < 0) return region;  // identical
  region.x = minX;
  region.y = minY;
  region.width = maxX - minX + 1;
  region.height = maxY - minY + 1;
  region.valid = true;
  return region;
}

}  // namespace readerutil