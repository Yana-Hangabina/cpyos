// Encode the 1-bit CPYOS framebuffer as a top-down BMP for remote bring-up.
//
// The framebuffer stores bit 1 = paper (white), rows MSB-first, tightly packed
// at `stride` bytes. BMP 1bpp expects the same row order but bottom-up by
// default; a negative height selects top-down, which keeps the byte order.
// Palette entry 0 is black and 1 is white, matching the framebuffer's meaning.
// ESP-IDF-free so the header/row math has host tests.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace bmp_export {

inline size_t rowBytes(int width) { return (static_cast<size_t>(width) + 7) / 8; }

inline size_t paddedRowBytes(int width) { return (rowBytes(width) + 3) & ~static_cast<size_t>(3); }

inline size_t fileSize(int width, int height) {
  return 14 + 40 + 8 + paddedRowBytes(width) * static_cast<size_t>(height);
}

inline void put16(uint8_t* out, size_t at, uint16_t value) {
  out[at] = static_cast<uint8_t>(value & 0xFF);
  out[at + 1] = static_cast<uint8_t>(value >> 8);
}

inline void put32(uint8_t* out, size_t at, uint32_t value) {
  out[at] = static_cast<uint8_t>(value & 0xFF);
  out[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[at + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[at + 3] = static_cast<uint8_t>(value >> 24);
}

// `stride` is the source row pitch; `invert` flips the polarity for panels that
// are mounted inverted. Returns false when the arguments are unusable.
inline bool writeMonoBmpTo(uint8_t* out, size_t out_size, const uint8_t* source, int stride,
                           int width, int height, bool invert) {
  if (out == nullptr || source == nullptr || width <= 0 || height <= 0 || stride <= 0) return false;
  const size_t row = paddedRowBytes(width);
  const size_t size = fileSize(width, height);
  if (out_size < size) return false;
  std::memset(out, 0, size);
  out[0] = 'B';
  out[1] = 'M';
  put32(out, 2, static_cast<uint32_t>(size));
  put32(out, 10, 14 + 40 + 8);  // pixel data offset
  put32(out, 14, 40);           // BITMAPINFOHEADER
  put32(out, 18, static_cast<uint32_t>(width));
  put32(out, 22, static_cast<uint32_t>(-height));  // negative => top-down
  put16(out, 26, 1);                              // planes
  put16(out, 28, 1);                              // bits per pixel
  put32(out, 34, static_cast<uint32_t>(row * static_cast<size_t>(height)));
  put32(out, 38, 2835);  // 72 dpi
  put32(out, 42, 2835);
  put32(out, 46, 2);  // palette colours
  // Palette: 0 = black, 1 = white (BGRA).
  out[54 + 4] = 255;
  out[54 + 5] = 255;
  out[54 + 6] = 255;
  out[54 + 7] = 0;
  const size_t base = 14 + 40 + 8;
  for (int y = 0; y < height; ++y) {
    const uint8_t* src = source + static_cast<size_t>(y) * static_cast<size_t>(stride);
    uint8_t* dst = out + base + static_cast<size_t>(y) * row;
    std::memcpy(dst, src, rowBytes(width));
    if (invert) {
      for (size_t i = 0; i < rowBytes(width); ++i) dst[i] = static_cast<uint8_t>(~dst[i]);
    }
  }
  return true;
}

inline bool writeMonoBmp(const uint8_t* source, int stride, int width, int height, bool invert,
                         std::vector<uint8_t>& out) {
  if (source == nullptr || width <= 0 || height <= 0 || stride <= 0) return false;
  out.assign(fileSize(width, height), 0);
  return writeMonoBmpTo(out.data(), out.size(), source, stride, width, height, invert);
}

}  // namespace bmp_export
