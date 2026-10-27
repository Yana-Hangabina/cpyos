#pragma once

// Read-only view over a validated pinyin.dat image: the offline filename
// matcher (literal / full Pinyin / initials channels) and the lookups the IME
// composer needs. Everything operates on a caller-owned buffer and allocates
// nothing; the class is reentrant across threads as long as the image lives.
//
// Ported from read-pico-reader (kiikoread, Apache-2.0)
// components/read_pico_search/read_pico_search.c. The matching rules are kept
// verbatim (including the polyphonic handling: every reading of a character is
// tried, no semantic guessing) because the table was generated from the same
// pinned dictionary.

#include <cstddef>
#include <cstdint>

#include "PinyinFormat.h"

namespace pinyin {

class PinyinTables {
 public:
  // Validate and adopt an image; on failure the instance stays unusable and
  // the reason is returned. The buffer must outlive the instance and must not
  // be written while matching.
  PydbValidity adopt(const uint8_t* data, size_t size);

  bool valid() const { return image_ != nullptr; }

  // True when `query` matches `text` literally, as toneless full Pinyin, or as
  // initials (Chinese and English). Text is capped at 255 bytes, the query at
  // 64; over-long or invalid UTF-8 inputs return false, an empty or
  // whitespace-only query matches everything.
  bool match(const char* text, size_t textLength, const char* query, size_t queryLength) const;

  // --- IME lookups -----------------------------------------------------------
  size_t syllableCount() const;
  // The toneless reading at `index`, or nullptr when out of range.
  const char* syllable(size_t index) const;
  // Up to `cap` candidate characters for an exact syllable, starting `skip`
  // entries in (common glyphs first). Returns how many were written.
  size_t candidates(const char* syllable, uint32_t* out, size_t cap, size_t skip) const;

  size_t phraseCount() const;
  // Roman and text of phrase `index`, both NUL-terminated inside the image.
  bool phrase(size_t index, const char** roman, const char** text) const;

 private:
  uint16_t readU16(uint32_t offset) const;
  uint32_t readU32(uint32_t offset) const;
  // Reading-group id of one character, or UINT32_MAX when the table has none.
  uint32_t groupFor(uint32_t cp) const;
  // Exact-match lookup of a lowercase syllable, or -1.
  int syllableIndexFor(const char* syllable) const;

  const uint8_t* image_ = nullptr;
  size_t size_ = 0;
  PydbHeader header_{};
};

}  // namespace pinyin
