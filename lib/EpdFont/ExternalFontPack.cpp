#include "ExternalFontPack.h"

#include <cstring>

namespace {

constexpr uint16_t kVersion = 1;

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

bool ExternalFontPack::parse(const uint8_t* data, size_t size) {
  data_ = nullptr;
  size_ = 0;
  count_ = 0;
  if (!data || size < kHeaderSize) return false;
  if (std::memcmp(data, "CPFN", 4) != 0) return false;
  if (rd16(data + 4) != kVersion) return false;
  const uint16_t count = rd16(data + 6);
  if (count == 0 || count > kMaxFonts) return false;
  if (rd32(data + 8) != size) return false;
  if (kHeaderSize + size_t(count) * kEntrySize > size) return false;

  EpdFontData parsed[kMaxFonts]{};
  char names[kMaxFonts][kNameLen]{};

  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* e = data + kHeaderSize + size_t(i) * kEntrySize;
    std::memcpy(names[i], e, kNameLen);
    names[i][kNameLen - 1] = '\0';
    if (names[i][0] == '\0') return false;

    const uint32_t bitmapOffset = rd32(e + 12);
    const uint32_t bitmapSize = rd32(e + 16);
    const uint32_t glyphOffset = rd32(e + 20);
    const uint32_t glyphCount = rd32(e + 24);
    const uint32_t intervalOffset = rd32(e + 28);
    const uint32_t intervalCount = rd32(e + 32);
    const uint16_t advanceY = rd16(e + 36);
    const uint8_t is2Bit = e[38];
    const int16_t ascender = static_cast<int16_t>(rd16(e + 40));
    const int16_t descender = static_cast<int16_t>(rd16(e + 42));
    const uint32_t storedCrc = rd32(e + 44);

    if (glyphCount == 0 || intervalCount == 0) return false;
    if (bitmapOffset > size || bitmapSize > size - bitmapOffset) return false;
    if ((glyphOffset & 3u) != 0 || glyphOffset > size) return false;
    const size_t glyphBytes = size_t(glyphCount) * sizeof(EpdGlyph);
    if (glyphBytes / sizeof(EpdGlyph) != glyphCount) return false;
    if (glyphBytes > size - glyphOffset) return false;
    if ((intervalOffset & 3u) != 0 || intervalOffset > size) return false;
    const size_t intervalBytes = size_t(intervalCount) * sizeof(EpdUnicodeInterval);
    if (intervalBytes / sizeof(EpdUnicodeInterval) != intervalCount) return false;
    if (intervalBytes > size - intervalOffset) return false;

    // CRC covers bitmaps, glyphs and intervals in pack order.
    uint32_t c = 0xFFFFFFFFu;
    auto feed = [&c](const uint8_t* p, size_t n) {
      for (size_t k = 0; k < n; ++k) {
        c ^= p[k];
        for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
      }
    };
    feed(data + bitmapOffset, bitmapSize);
    feed(data + glyphOffset, glyphBytes);
    feed(data + intervalOffset, intervalBytes);
    if (~c != storedCrc) return false;

    EpdFontData& d = parsed[i];
    d.bitmap = data + bitmapOffset;
    d.glyph = reinterpret_cast<const EpdGlyph*>(data + glyphOffset);
    d.intervals = reinterpret_cast<const EpdUnicodeInterval*>(data + intervalOffset);
    d.intervalCount = intervalCount;
    d.advanceY = static_cast<uint8_t>(advanceY);
    d.ascender = ascender;
    d.descender = descender;
    d.is2Bit = is2Bit != 0;
  }

  data_ = data;
  size_ = size;
  count_ = static_cast<uint8_t>(count);
  for (uint8_t i = 0; i < count_; ++i) {
    fonts_[i] = parsed[i];
    std::memcpy(names_[i], names[i], kNameLen);
  }
  return true;
}

const EpdFontData* ExternalFontPack::get(const char* name) const {
  if (!name) return nullptr;
  for (uint8_t i = 0; i < count_; ++i) {
    if (std::strncmp(names_[i], name, kNameLen) == 0) return &fonts_[i];
  }
  return nullptr;
}
