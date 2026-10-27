#pragma once

#include <cstdint>

// Rebuilds a language's uint16 offset table from a "same as English" bitmap.
//
// PSRAM targets store only the bitmap plus the language's own strings in flash;
// the 2 bytes per key per language are rebuilt once at boot into PSRAM. This
// header is deliberately free of ESP/Arduino dependencies so the exact same
// code can run in a host test.
//
// Bitmap layout: one bit per StrId, LSB-first within each byte. 1 = the string
// is byte-identical to English and the offset points into the English blob
// (bit 15 set); 0 = the string lives in this language's own blob.
namespace i18n_offsets {

constexpr uint16_t kEnglishBit = 0x8000;
constexpr uint16_t kOffsetMask = 0x7FFF;

// Fills out[0..keyCount) from the bitmap, the English offsets and this
// language's blob. Returns false (and leaves out in an unspecified state) if
// an offset would not fit in 15 bits, a string has no terminator inside the
// blob, or the blob length does not exactly match the strings consumed.
inline bool buildOffsets(const uint8_t* sameAsEn, uint32_t keyCount, const uint16_t* enOffsets,
                         const char* blob, uint32_t blobSize, uint16_t* out) {
  if (sameAsEn == nullptr || enOffsets == nullptr || out == nullptr) {
    return false;
  }
  if (blobSize > 0 && blob == nullptr) {
    return false;
  }

  uint32_t pos = 0;
  for (uint32_t i = 0; i < keyCount; ++i) {
    const bool same = (sameAsEn[i >> 3] & (1u << (i & 7u))) != 0;
    if (same) {
      const uint16_t en = enOffsets[i];
      // The English table holds plain offsets; a bit-15 entry means the caller
      // mixed tables up.
      if ((en & kEnglishBit) != 0) {
        return false;
      }
      out[i] = static_cast<uint16_t>(en | kEnglishBit);
      continue;
    }

    if (pos > kOffsetMask) {
      return false;
    }
    out[i] = static_cast<uint16_t>(pos);

    uint32_t len = 0;
    while (pos + len < blobSize && blob[pos + len] != '\0') {
      ++len;
    }
    if (pos + len >= blobSize) {
      return false;  // missing terminator: truncated or over-long blob
    }
    pos += len + 1;
  }

  // Every blob byte must belong to a string; trailing data means the bitmap
  // and the blob disagree.
  return pos == blobSize;
}

// Mirrors I18n::get(): bit 15 selects the English blob.
inline const char* resolve(const char* langData, const uint16_t* offsets, uint32_t index,
                           const char* enData) {
  const uint16_t off = offsets[index];
  if ((off & kEnglishBit) != 0) {
    return enData + (off & kOffsetMask);
  }
  return langData + off;
}

}  // namespace i18n_offsets
