// Generate build/font_pack.dat from the four large built-in CJK/title faces.
//
// The faces are simple: each EpdFontData points at three arrays and carries
// four scalars; kerning/ligatures/groups are all null.  The pack keeps those
// arrays byte-for-byte, so the runtime loader can point an EpdFontData straight
// into the loaded buffer instead of re-rasterising anything.
//
// Usage: generate_font_pack <output.dat>
// Build: g++ -std=c++20 -O2 -Ilib/EpdFont tools/generate_font_pack.cpp

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "EpdFontData.h"
#include "builtinFonts/notosans_cjk_12.h"
#include "builtinFonts/notosans_cjk_14.h"
#include "builtinFonts/notosans_cjk_16.h"
#include "builtinFonts/notoserif_title_23.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph layout is part of the pack format");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval layout is part of the pack format");

namespace {

constexpr uint16_t kVersion = 1;
constexpr size_t kNameLen = 12;
constexpr size_t kEntrySize = 48;
constexpr size_t kHeaderSize = 16;

struct FontSrc {
  const char* name;
  const EpdFontData* data;
  const uint8_t* bitmaps;
  uint32_t bitmapSize;
  const EpdGlyph* glyphs;
  uint32_t glyphCount;
  const EpdUnicodeInterval* intervals;
  uint32_t intervalCount;
  uint32_t crc;
};

template <typename T, size_t N>
constexpr uint32_t countOf(const T (&)[N]) {
  return static_cast<uint32_t>(N);
}

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x));
  v.push_back(static_cast<uint8_t>(x >> 8));
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 24));
}

void pad4(std::vector<uint8_t>& v) {
  while (v.size() % 4) v.push_back(0);
}

void putName(std::vector<uint8_t>& v, const char* name) {
  char tmp[kNameLen] = {};
  std::strncpy(tmp, name, kNameLen - 1);
  v.insert(v.end(), tmp, tmp + kNameLen);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <output.dat>\n", argv[0]);
    return 2;
  }

  FontSrc fonts[] = {
      {"cjk12", &notosans_cjk_12, notosans_cjk_12Bitmaps, sizeof(notosans_cjk_12Bitmaps), notosans_cjk_12Glyphs,
       countOf(notosans_cjk_12Glyphs), notosans_cjk_12Intervals, countOf(notosans_cjk_12Intervals), 0},
      {"title", &notoserif_title, notoserif_titleBitmaps, sizeof(notoserif_titleBitmaps), notoserif_titleGlyphs,
       countOf(notoserif_titleGlyphs), notoserif_titleIntervals, countOf(notoserif_titleIntervals), 0},
  };
  const uint16_t fontCount = static_cast<uint16_t>(sizeof(fonts) / sizeof(fonts[0]));

  // Data area begins after the directory, 4-byte aligned.
  std::vector<uint8_t> data;
  struct Layout {
    uint32_t bitmapOffset, glyphOffset, intervalOffset;
  } layout[4] = {};

  for (uint16_t i = 0; i < fontCount; ++i) {
    FontSrc& f = fonts[i];
    pad4(data);
    layout[i].bitmapOffset = static_cast<uint32_t>(data.size());
    data.insert(data.end(), f.bitmaps, f.bitmaps + f.bitmapSize);
    pad4(data);
    layout[i].glyphOffset = static_cast<uint32_t>(data.size());
    const uint8_t* g = reinterpret_cast<const uint8_t*>(f.glyphs);
    data.insert(data.end(), g, g + size_t(f.glyphCount) * sizeof(EpdGlyph));
    pad4(data);
    layout[i].intervalOffset = static_cast<uint32_t>(data.size());
    const uint8_t* iv = reinterpret_cast<const uint8_t*>(f.intervals);
    data.insert(data.end(), iv, iv + size_t(f.intervalCount) * sizeof(EpdUnicodeInterval));
    pad4(data);
    // CRC covers the three arrays in pack order (padding excluded).
    const size_t glyphBytes = size_t(f.glyphCount) * sizeof(EpdGlyph);
    const size_t intervalBytes = size_t(f.intervalCount) * sizeof(EpdUnicodeInterval);
    std::vector<uint8_t> crcBuf;
    crcBuf.reserve(f.bitmapSize + glyphBytes + intervalBytes);
    crcBuf.insert(crcBuf.end(), f.bitmaps, f.bitmaps + f.bitmapSize);
    crcBuf.insert(crcBuf.end(), g, g + glyphBytes);
    crcBuf.insert(crcBuf.end(), iv, iv + intervalBytes);
    f.crc = crc32(crcBuf.data(), crcBuf.size());
  }

  std::vector<uint8_t> out;
  out.reserve(kHeaderSize + size_t(fontCount) * kEntrySize + data.size());
  out.insert(out.end(), {'C', 'P', 'F', 'N'});
  put16(out, kVersion);
  put16(out, fontCount);
  put32(out, static_cast<uint32_t>(kHeaderSize + size_t(fontCount) * kEntrySize + data.size()));
  put32(out, 0);

  for (uint16_t i = 0; i < fontCount; ++i) {
    const FontSrc& f = fonts[i];
    const uint32_t base = static_cast<uint32_t>(kHeaderSize + size_t(fontCount) * kEntrySize);
    putName(out, f.name);
    put32(out, base + layout[i].bitmapOffset);
    put32(out, f.bitmapSize);
    put32(out, base + layout[i].glyphOffset);
    put32(out, f.glyphCount);
    put32(out, base + layout[i].intervalOffset);
    put32(out, f.intervalCount);
    put16(out, static_cast<uint16_t>(f.data->advanceY));
    out.push_back(f.data->is2Bit ? 1 : 0);
    out.push_back(0);
    put16(out, static_cast<uint16_t>(static_cast<int16_t>(f.data->ascender)));
    put16(out, static_cast<uint16_t>(static_cast<int16_t>(f.data->descender)));
    put32(out, f.crc);
  }

  out.insert(out.end(), data.begin(), data.end());

  std::FILE* fp = std::fopen(argv[1], "wb");
  if (!fp) {
    std::perror(argv[1]);
    return 1;
  }
  const size_t written = std::fwrite(out.data(), 1, out.size(), fp);
  std::fclose(fp);
  if (written != out.size()) return 1;

  std::printf("font_pack: %u fonts, %zu bytes -> %s\n", unsigned(fontCount), out.size(), argv[1]);
  for (uint16_t i = 0; i < fontCount; ++i) {
    std::printf("  %-6s bitmap=%u glyphs=%u intervals=%u crc=%08x\n", fonts[i].name, fonts[i].bitmapSize,
                fonts[i].glyphCount, fonts[i].intervalCount, fonts[i].crc);
  }
  return 0;
}
