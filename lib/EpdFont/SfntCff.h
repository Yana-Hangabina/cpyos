#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// True when an sfnt file stores outlines as CFF/CFF2 instead of glyf.
//
// The Metalio E-INK4 Plus port renders CFF glyphs at roughly twice their
// nominal ink size while line metrics stay nominal (measured on the glass for
// both embedded EPUB faces and SD font families), so those surfaces skip CFF
// at discovery time rather than drawing overlapping text. Read Pico's
// FreeType build renders CFF correctly and does not use this guard.
inline bool sfntHasCffOutlines(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 12) return false;
  const size_t tables = (static_cast<size_t>(data[4]) << 8) | data[5];
  if (tables == 0 || tables > 64 || 12 + tables * 16 > len) return false;
  for (size_t i = 0; i < tables; ++i) {
    const uint8_t* rec = data + 12 + i * 16;
    if (memcmp(rec, "CFF ", 4) == 0 || memcmp(rec, "CFF2", 4) == 0) return true;
  }
  return false;
}
