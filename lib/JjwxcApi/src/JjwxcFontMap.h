// Reverse-maps the per-reader PUA watermark that JJWXC's WAP VIP pages carry.
//
// The WAP font is regenerated per session: glyph order in `glyf` is shuffled and
// the PUA codepoints are re-randomised, so no codepoint -> character map can be
// shipped. What stays fixed is the *bytes* of each outline record, so the device
// walks cmap -> loca -> glyf and keys a small constant table on a truncated MD5
// of that record. That replaces the reference implementation's rasterise-and-IoU
// match (fontTools + Pillow + numpy) with one pass over a 45 KB buffer.
//
// The library allocates nothing: the TTF bytes and the PuaMap both belong to the
// caller, which on firmware puts them in PSRAM. Internal RAM is down to ~50 KB
// and the display driver needs 45 KB of it contiguous, so any heap use here
// would be a blackout risk rather than a style issue.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

// ------------------------------------------------------------------ result ---

// Why a build failed, or why an individual codepoint could not be resolved.
// Kept granular because each value implies a different remedy: an unknown hash
// means the shipped table is stale, a parse failure means the download is bad.
enum class FontStatus : uint8_t {
  Ok = 0,
  TooShort,            // buffer cannot hold an sfnt header
  BadSfntVersion,      // CFF ('OTTO') or a TrueType collection ('ttcf')
  MissingTable,        // no head/maxp/loca/glyf/cmap
  TruncatedTable,      // a table record runs past the end of the buffer
  BadLocaFormat,       // head.indexToLocFormat is neither 0 nor 1
  BadLocaOffsets,      // loca points outside glyf, or is too short for numGlyphs
  NoUsableCmap,        // every subtable is a format this parser does not know
  BadCmap,             // chosen subtable is internally inconsistent
};

struct FontMapResult {
  FontStatus status = FontStatus::Ok;

  uint32_t puaCount = 0;    // PUA codepoints the cmap actually maps
  uint32_t resolved = 0;    // ...and whose outline hash is in the table
  uint32_t unresolved = 0;  // puaCount - resolved, always reported, never folded away

  // Breakdown of `unresolved`. A silent U+FFFD or a raw PUA codepoint reaching
  // the reader would look like a font bug in the e-ink renderer; these counters
  // let the caller reject the chapter and ask for a firmware update instead.
  uint32_t unknownHash = 0;  // outline not in the shipped table
  uint32_t emptyGlyph = 0;   // zero-length glyf record
  uint32_t badGlyphId = 0;   // cmap pointed at or past numGlyphs

  bool overflowed = false;  // more PUA codepoints than PuaMap::kCapacity
  bool tableMissing = false;  // built without jjwxc_font_table.inc; nothing can resolve

  uint16_t numGlyphs = 0;         // diagnostics for the serial log
  int16_t indexToLocFormat = -1;  // 0 = short loca, 1 = long
};

// --------------------------------------------------------------------- map ---

struct PuaEntry {
  uint32_t codepoint = 0;  // 0xE000..0xF8FF
  char utf8[4] = {0, 0, 0, 0};  // NUL-terminated; 3 bytes covers all BMP CJK
  uint8_t utf8Len = 0;
};

// Caller-owned fixed-capacity container. 256 rows leave headroom over the 200
// codepoints every sampled font carries, at ~3 KB - acceptable in PSRAM, and the
// alternative (a std::vector) is banned in this library.
struct PuaMap {
  static constexpr uint32_t kCapacity = 256;

  PuaEntry entries[kCapacity];
  uint32_t count = 0;

  void clear() { count = 0; }
  bool empty() const { return count == 0; }

  // Inserts in codepoint order so find() can binary-search. Returns false when
  // full or when `len` does not fit PuaEntry::utf8; the caller must treat a
  // false as an unresolved codepoint rather than dropping it on the floor.
  bool add(uint32_t codepoint, const char* utf8, uint8_t len);

  // nullptr when the codepoint is absent, which is how callers tell "not a
  // watermark character" apart from "watermark we could not decode".
  const PuaEntry* find(uint32_t codepoint) const;
};

// --------------------------------------------------------------- glyph row ---

// One row of the generated table. The layout must match what
// tools/gen_jjwxc_font_table.py emits; all members are byte-sized so the struct
// packs to 13 bytes with no padding, and 200 rows cost 2600 bytes of Flash.
struct JjwxcGlyphEntry {
  uint8_t hash[8];
  char utf8[4];
  uint8_t utf8Len;
};

// Rows in the shipped outline table. Zero means the generated
// jjwxc_font_table.inc was absent at compile time, in which case every PUA
// codepoint reports unresolved and FontMapResult::tableMissing is set.
uint32_t glyphTableSize();

// Builds a PUA -> UTF-8 map from an in-memory TTF.
//
// Returns false when the font could not be parsed at all (see out.status), or
// when any codepoint failed to resolve - a partial map is never presented as
// success, because the caller has no way to know which characters are missing
// once they are interleaved into chapter text. `out` is filled in either way.
bool buildPuaMap(const uint8_t* ttf, size_t ttfLen, PuaMap& map, FontMapResult& out);

}  // namespace jjwxc
