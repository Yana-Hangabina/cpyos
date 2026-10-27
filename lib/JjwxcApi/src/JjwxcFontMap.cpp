#include "JjwxcFontMap.h"

#include <string.h>

#include "JjwxcCrypto.h"

namespace jjwxc {

// The outline table is generated offline by tools/gen_jjwxc_font_table.py and
// reviewed before it is copied next to this file. A missing copy must not break
// the build, so it degrades to an empty table: every PUA codepoint then reports
// unresolved and FontMapResult::tableMissing says why, which is the loud failure
// this feature is required to have. Shipping wrong characters silently would be
// far worse than refusing the chapter.
#if defined(__has_include)
#if __has_include("jjwxc_font_table.inc")
#include "jjwxc_font_table.inc"
#define JJWXC_FONT_TABLE_PRESENT 1
#endif
#endif

#ifndef JJWXC_FONT_TABLE_PRESENT
#warning \
    "JjwxcFontMap: jjwxc_font_table.inc not found - PUA watermark decoding is disabled. Run tools/gen_jjwxc_font_table.py and copy the result next to JjwxcFontMap.cpp."
// One row so the array is not zero-length (ill-formed in C++), with a count of
// zero so no lookup ever reaches it.
constexpr JjwxcGlyphEntry kJjwxcGlyphTable[1] = {};
constexpr uint32_t kJjwxcGlyphCount = 0u;
#endif

namespace {

constexpr uint32_t kPuaFirst = 0xE000u;
constexpr uint32_t kPuaLast = 0xF8FFu;
constexpr uint32_t kHashLen = 8u;

constexpr size_t kSfntHeaderLen = 12u;
constexpr size_t kTableRecordLen = 16u;
constexpr size_t kCmapEncodingRecordLen = 8u;
// head.indexToLocFormat sits at byte 50, so 54 bytes is the shortest head that
// can be interpreted at all.
constexpr size_t kHeadLen = 54u;

// The generator quotes its Flash cost as rows * 13, which only holds if the row
// struct really packs; every member is byte-sized, so it must.
static_assert(sizeof(JjwxcGlyphEntry) == 13u, "JjwxcGlyphEntry grew; update gen_jjwxc_font_table.py");

// ------------------------------------------------------------- byte readers --
// Assembled a byte at a time rather than through a cast: the shared reader code
// also runs on ESP32-C3, where a reinterpret_cast of an unaligned uint8_t* to a
// wider type faults. Table offsets inside an sfnt are only 4-aligned by luck.
inline uint16_t be16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

inline int16_t beI16(const uint8_t* p) { return static_cast<int16_t>(be16(p)); }

inline uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// `off + n` would overflow on a hostile offset, so the subtraction form is used
// everywhere instead of the natural comparison.
inline bool inRange(size_t len, size_t off, size_t n) { return off <= len && n <= len - off; }

struct SfntLayout {
  const uint8_t* data = nullptr;
  size_t len = 0;
  size_t headOff = 0;
  size_t headLen = 0;
  size_t maxpOff = 0;
  size_t maxpLen = 0;
  size_t locaOff = 0;
  size_t locaLen = 0;
  size_t glyfOff = 0;
  size_t glyfLen = 0;
  size_t cmapOff = 0;
  size_t cmapLen = 0;
  uint16_t numGlyphs = 0;
  int16_t indexToLocFormat = 0;
};

bool parseSfnt(const uint8_t* ttf, size_t ttfLen, SfntLayout& out, FontStatus& status) {
  status = FontStatus::Ok;
  if (ttf == nullptr || ttfLen < kSfntHeaderLen) {
    status = FontStatus::TooShort;
    return false;
  }
  const uint32_t version = be32(ttf);
  // 0x00010000 is plain TrueType; 'true' and 'typ1' are the legacy Apple
  // spellings of the same layout. 'OTTO' carries CFF outlines with no glyf/loca,
  // and 'ttcf' is a collection needing an extra indirection - neither has been
  // observed from JJWXC, so both are rejected instead of guessed at.
  if (version != 0x00010000u && version != 0x74727565u && version != 0x74797031u) {
    status = FontStatus::BadSfntVersion;
    return false;
  }

  out.data = ttf;
  out.len = ttfLen;

  const uint16_t numTables = be16(ttf + 4);
  bool haveHead = false, haveMaxp = false, haveLoca = false, haveGlyf = false, haveCmap = false;
  for (uint16_t i = 0; i < numTables; ++i) {
    const size_t rec = kSfntHeaderLen + static_cast<size_t>(i) * kTableRecordLen;
    if (!inRange(ttfLen, rec, kTableRecordLen)) {
      status = FontStatus::TruncatedTable;
      return false;
    }
    const uint8_t* tag = ttf + rec;
    const size_t off = be32(ttf + rec + 8);
    const size_t len = be32(ttf + rec + 12);
    if (!inRange(ttfLen, off, len)) {
      status = FontStatus::TruncatedTable;
      return false;
    }
    if (memcmp(tag, "head", 4) == 0) {
      out.headOff = off;
      out.headLen = len;
      haveHead = true;
    } else if (memcmp(tag, "maxp", 4) == 0) {
      out.maxpOff = off;
      out.maxpLen = len;
      haveMaxp = true;
    } else if (memcmp(tag, "loca", 4) == 0) {
      out.locaOff = off;
      out.locaLen = len;
      haveLoca = true;
    } else if (memcmp(tag, "glyf", 4) == 0) {
      out.glyfOff = off;
      out.glyfLen = len;
      haveGlyf = true;
    } else if (memcmp(tag, "cmap", 4) == 0) {
      out.cmapOff = off;
      out.cmapLen = len;
      haveCmap = true;
    }
  }
  if (!haveHead || !haveMaxp || !haveLoca || !haveGlyf || !haveCmap) {
    status = FontStatus::MissingTable;
    return false;
  }
  if (out.headLen < kHeadLen || out.maxpLen < 6u) {
    status = FontStatus::TruncatedTable;
    return false;
  }

  out.indexToLocFormat = beI16(ttf + out.headOff + 50);
  if (out.indexToLocFormat != 0 && out.indexToLocFormat != 1) {
    status = FontStatus::BadLocaFormat;
    return false;
  }
  out.numGlyphs = be16(ttf + out.maxpOff + 4);

  // Validating every loca entry up front makes the per-glyph reads below
  // infallible, so a corrupt offset surfaces as a parse error naming the table
  // instead of as an unexplainable empty glyph.
  const size_t entryLen = (out.indexToLocFormat == 0) ? 2u : 4u;
  const size_t entries = static_cast<size_t>(out.numGlyphs) + 1u;
  if (out.locaLen / entryLen < entries) {
    status = FontStatus::BadLocaOffsets;
    return false;
  }
  for (size_t i = 0; i < entries; ++i) {
    const size_t at = out.locaOff + i * entryLen;
    const uint32_t raw = (entryLen == 2u) ? be16(ttf + at) : be32(ttf + at);
    // Short format stores offsets halved, which caps the addressable glyf table
    // at 128 KB - well above the ~36 KB these watermark fonts use.
    const uint32_t scaled = (entryLen == 2u) ? static_cast<uint32_t>(raw * 2u) : raw;
    if (scaled > out.glyfLen) {
      status = FontStatus::BadLocaOffsets;
      return false;
    }
  }
  return true;
}

// loca offsets are pre-validated by parseSfnt, so this cannot fail.
uint32_t locaAt(const SfntLayout& f, uint32_t index) {
  const size_t entryLen = (f.indexToLocFormat == 0) ? 2u : 4u;
  const size_t at = f.locaOff + static_cast<size_t>(index) * entryLen;
  const uint32_t raw = (entryLen == 2u) ? be16(f.data + at) : be32(f.data + at);
  return (entryLen == 2u) ? static_cast<uint32_t>(raw * 2u) : raw;
}

// ------------------------------------------------------------------- cmap ----
struct CmapChoice {
  size_t off = 0;
  uint16_t format = 0;
  bool found = false;
};

CmapChoice chooseCmap(const SfntLayout& f) {
  CmapChoice best;
  if (f.cmapLen < 4u) return best;
  const uint16_t numSub = be16(f.data + f.cmapOff + 2);
  CmapChoice firstFmt4;
  CmapChoice firstFmt12;
  for (uint16_t i = 0; i < numSub; ++i) {
    const size_t rec = f.cmapOff + 4u + static_cast<size_t>(i) * kCmapEncodingRecordLen;
    if (!inRange(f.len, rec, kCmapEncodingRecordLen)) break;
    const uint32_t rel = be32(f.data + rec + 4);
    // A bogus subtable offset is skipped rather than fatal: the samples carry
    // five subtables and only one of them is needed.
    if (rel > f.cmapLen) continue;
    const size_t sub = f.cmapOff + rel;
    if (!inRange(f.len, sub, 4u)) continue;
    const uint16_t platform = be16(f.data + rec);
    const uint16_t encoding = be16(f.data + rec + 2);
    const uint16_t fmt = be16(f.data + sub);
    // (3,1) is the Windows Unicode BMP spelling every sampled font carries, and
    // the PUA block is BMP-only, so format 4 is both sufficient and cheaper to
    // walk than the segmented format 12 groups.
    if (fmt == 4u && platform == 3u && encoding == 1u) {
      best.off = sub;
      best.format = 4u;
      best.found = true;
      return best;
    }
    if (fmt == 4u && !firstFmt4.found) {
      firstFmt4.off = sub;
      firstFmt4.format = 4u;
      firstFmt4.found = true;
    } else if (fmt == 12u && !firstFmt12.found) {
      firstFmt12.off = sub;
      firstFmt12.format = 12u;
      firstFmt12.found = true;
    }
  }
  if (firstFmt4.found) return firstFmt4;
  return firstFmt12;
}

bool cmapFormat4(const SfntLayout& f, size_t sub, uint32_t cp, uint32_t& gidOut, FontStatus& status) {
  gidOut = 0;
  if (cp > 0xFFFFu) return true;  // not representable in a BMP-only subtable
  if (!inRange(f.len, sub, 14u)) {
    status = FontStatus::BadCmap;
    return false;
  }
  const uint16_t segX2 = be16(f.data + sub + 6);
  if ((segX2 & 1u) != 0u) {
    status = FontStatus::BadCmap;
    return false;
  }
  const size_t segCount = static_cast<size_t>(segX2) / 2u;
  const size_t ends = sub + 14u;
  const size_t starts = ends + static_cast<size_t>(segX2) + 2u;  // +2 skips reservedPad
  const size_t deltas = starts + segX2;
  const size_t ranges = deltas + segX2;
  // Validating the idRangeOffset span covers endCode/startCode/idDelta too,
  // since they all sit before it.
  if (!inRange(f.len, ends, segX2) || !inRange(f.len, ranges, segX2)) {
    status = FontStatus::BadCmap;
    return false;
  }
  if (segCount == 0) return true;

  // endCode[] is ascending, so the first segment whose endCode >= cp is the only
  // one that can contain cp.
  size_t lo = 0;
  size_t hi = segCount - 1u;
  size_t found = segCount;  // sentinel: no candidate segment
  while (lo <= hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    if (be16(f.data + ends + mid * 2u) >= cp) {
      found = mid;
      if (mid == 0) break;
      hi = mid - 1u;
    } else {
      lo = mid + 1u;
    }
  }
  if (found == segCount) return true;

  const uint16_t start = be16(f.data + starts + found * 2u);
  if (start > cp) return true;
  const int16_t delta = beI16(f.data + deltas + found * 2u);
  const uint16_t rangeOff = be16(f.data + ranges + found * 2u);
  if (rangeOff == 0u) {
    gidOut = static_cast<uint32_t>((static_cast<int32_t>(cp) + delta) & 0xFFFF);
    return true;
  }
  // The spec's address arithmetic: glyphIdArray is reached relative to the
  // address of this segment's own idRangeOffset cell, not to the table start.
  const size_t cell = ranges + found * 2u;
  const size_t at = cell + rangeOff + (static_cast<size_t>(cp) - start) * 2u;
  if (!inRange(f.len, at, 2u)) {
    status = FontStatus::BadCmap;
    return false;
  }
  const uint16_t glyph = be16(f.data + at);
  gidOut = (glyph == 0u) ? 0u : static_cast<uint32_t>((static_cast<int32_t>(glyph) + delta) & 0xFFFF);
  return true;
}

bool cmapFormat12(const SfntLayout& f, size_t sub, uint32_t cp, uint32_t& gidOut, FontStatus& status) {
  gidOut = 0;
  if (!inRange(f.len, sub, 16u)) {
    status = FontStatus::BadCmap;
    return false;
  }
  const size_t base = sub + 16u;
  const uint32_t groups = be32(f.data + sub + 12);
  if (groups == 0u) return true;
  // Division rather than multiplication: groups comes from the file and 12 *
  // groups can exceed 32 bits.
  if (groups > (f.len - base) / 12u) {
    status = FontStatus::BadCmap;
    return false;
  }
  size_t lo = 0;
  size_t hi = static_cast<size_t>(groups) - 1u;
  while (lo <= hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    const size_t at = base + mid * 12u;
    const uint32_t first = be32(f.data + at);
    const uint32_t last = be32(f.data + at + 4u);
    if (cp < first) {
      if (mid == 0) break;
      hi = mid - 1u;
    } else if (cp > last) {
      lo = mid + 1u;
    } else {
      gidOut = be32(f.data + at + 8u) + (cp - first);
      return true;
    }
  }
  return true;
}

// ------------------------------------------------------------------- hash ----
int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// Md5 exposes only the hex form, so the key is its first 16 digits folded back
// down to bytes. Routing through hex keeps JjwxcCrypto.h untouched, and the
// generator uses hashlib.md5(...).digest()[:8], which is the same 8 bytes.
void outlineHash(const uint8_t* p, size_t n, uint8_t out[kHashLen]) {
  char hex[Md5::kHexLen + 1];
  Md5::hex(p, n, hex);
  for (uint32_t i = 0; i < kHashLen; ++i) {
    out[i] = static_cast<uint8_t>((hexVal(hex[i * 2u]) << 4) | hexVal(hex[i * 2u + 1u]));
  }
}

// The generated rows are sorted by hash, so this is a plain binary search over
// 200 entries: ~8 memcmp calls per codepoint, no index structure to keep in RAM.
const JjwxcGlyphEntry* lookupHash(const uint8_t hash[kHashLen]) {
  size_t lo = 0;
  size_t hi = static_cast<size_t>(kJjwxcGlyphCount);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    const int cmp = memcmp(kJjwxcGlyphTable[mid].hash, hash, kHashLen);
    if (cmp == 0) return &kJjwxcGlyphTable[mid];
    if (cmp < 0) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return nullptr;
}

}  // namespace

uint32_t glyphTableSize() { return kJjwxcGlyphCount; }

bool PuaMap::add(uint32_t codepoint, const char* utf8, uint8_t len) {
  if (utf8 == nullptr || len == 0u || len >= sizeof(PuaEntry::utf8) || count >= kCapacity) {
    return false;
  }
  size_t at = count;
  // Walk back past everything that sorts after the new codepoint, stopping at the
  // first entry that is >= it, so an equal codepoint lands on `at` and is caught
  // by the duplicate test below rather than being appended behind its twin.
  while (at > 0 && entries[at - 1].codepoint >= codepoint) --at;
  if (at < count && entries[at].codepoint == codepoint) return false;
  if (at < count) {
    memmove(&entries[at + 1], &entries[at], static_cast<size_t>(count - at) * sizeof(PuaEntry));
  }
  entries[at].codepoint = codepoint;
  memset(entries[at].utf8, 0, sizeof(entries[at].utf8));
  memcpy(entries[at].utf8, utf8, len);
  entries[at].utf8Len = len;
  ++count;
  return true;
}

const PuaEntry* PuaMap::find(uint32_t codepoint) const {
  size_t lo = 0;
  size_t hi = count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    const uint32_t here = entries[mid].codepoint;
    if (here == codepoint) return &entries[mid];
    if (here < codepoint) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return nullptr;
}

bool buildPuaMap(const uint8_t* ttf, size_t ttfLen, PuaMap& map, FontMapResult& out) {
  out = FontMapResult{};
  map.clear();
  out.tableMissing = (kJjwxcGlyphCount == 0u);

  SfntLayout font;
  FontStatus status = FontStatus::Ok;
  if (!parseSfnt(ttf, ttfLen, font, status)) {
    out.status = status;
    return false;
  }
  out.numGlyphs = font.numGlyphs;
  out.indexToLocFormat = font.indexToLocFormat;

  const CmapChoice cmap = chooseCmap(font);
  if (!cmap.found) {
    out.status = FontStatus::NoUsableCmap;
    return false;
  }

  // Walking the whole PUA block costs 6400 binary searches, which is cheaper
  // than decoding the segment arrays to find which codepoints exist, and it
  // keeps puaCount an honest count of what the font maps rather than what the
  // caller expected it to map.
  for (uint32_t cp = kPuaFirst; cp <= kPuaLast; ++cp) {
    uint32_t gid = 0;
    FontStatus st = FontStatus::Ok;
    const bool ok = (cmap.format == 4u) ? cmapFormat4(font, cmap.off, cp, gid, st)
                                        : cmapFormat12(font, cmap.off, cp, gid, st);
    if (!ok) {
      out.status = st;
      return false;
    }
    if (gid == 0) continue;  // unmapped: this font does not watermark that slot
    ++out.puaCount;

    if (gid >= font.numGlyphs) {
      ++out.badGlyphId;
      ++out.unresolved;
      continue;
    }
    const uint32_t start = locaAt(font, gid);
    const uint32_t end = locaAt(font, gid + 1u);
    if (end <= start) {
      ++out.emptyGlyph;
      ++out.unresolved;
      continue;
    }
    const size_t recLen = static_cast<size_t>(end - start);
    const size_t at = font.glyfOff + start;
    // Unreachable given parseSfnt's validation; kept because the alternative is
    // an out-of-bounds read on a font that was edited between the two passes.
    if (!inRange(font.len, at, recLen)) {
      out.status = FontStatus::TruncatedTable;
      return false;
    }

    // The key is the whole glyf record, header and any trailing padding
    // included. That is the slice reachable from loca without decoding contour
    // data, and it was verified byte-identical across three real fonts whose
    // glyf/loca/cmap tables all hash differently.
    uint8_t hash[kHashLen];
    outlineHash(font.data + at, recLen, hash);
    const JjwxcGlyphEntry* row = lookupHash(hash);
    if (row == nullptr) {
      ++out.unknownHash;
      ++out.unresolved;
      continue;
    }
    if (!map.add(cp, row->utf8, row->utf8Len)) {
      out.overflowed = true;
      ++out.unresolved;
      continue;
    }
    ++out.resolved;
  }

  out.status = FontStatus::Ok;
  return out.unresolved == 0u;
}

}  // namespace jjwxc
