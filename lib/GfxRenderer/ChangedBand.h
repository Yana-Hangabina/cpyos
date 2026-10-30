#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Bounding box of the byte-level differences between two 1-bpp framebuffers.
//
// The band-update path pushes exactly this rect through the windowed scan, so a
// rect that is too small leaves stale pixels on the glass and one that is too
// large only costs power. Kept dependency-free (no GfxRenderer, no HAL) so the
// host test can drive it with synthetic frames.
//
// x/w come back rounded out to whole bytes because a byte holds 8 pixels; the
// caller compares bytes, so the bbox is exact at byte granularity and never
// smaller than the real change.
inline bool changedBandBoundingBox(const uint8_t* current, const uint8_t* reference, const int widthBytes,
                                   const int height, int* x, int* y, int* w, int* h) {
  if (current == nullptr || reference == nullptr || widthBytes <= 0 || height <= 0) return false;
  int minRow = -1;
  int maxRow = -1;
  int minByte = widthBytes;
  int maxByte = -1;
  for (int row = 0; row < height; ++row) {
    const uint8_t* cur = current + static_cast<size_t>(row) * static_cast<size_t>(widthBytes);
    const uint8_t* ref = reference + static_cast<size_t>(row) * static_cast<size_t>(widthBytes);
    if (std::memcmp(cur, ref, static_cast<size_t>(widthBytes)) == 0) continue;
    if (minRow < 0) minRow = row;
    maxRow = row;
    for (int i = 0; i < widthBytes; ++i) {
      if (cur[i] == ref[i]) continue;
      if (i < minByte) minByte = i;
      if (i > maxByte) maxByte = i;
    }
  }
  if (minRow < 0) return false;
  *x = minByte * 8;
  *y = minRow;
  *w = (maxByte - minByte + 1) * 8;
  *h = maxRow - minRow + 1;
  return true;
}
