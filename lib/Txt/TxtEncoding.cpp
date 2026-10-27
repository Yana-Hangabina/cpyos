#include "TxtEncoding.h"

#include <algorithm>
#include <cstring>

namespace txt_encoding {
namespace {

bool isGbkLead(const uint8_t value) { return value >= 0x81 && value <= 0xFE; }

bool isGbkTrail(const uint8_t value) { return value >= 0x40 && value <= 0xFE && value != 0x7F; }

size_t utf8SequenceLength(const uint8_t lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 0;
}

bool isStrictUtf8(const uint8_t* data, const size_t length, const bool finalChunk, bool& hasCompleteNonAscii) {
  size_t pos = 0;
  while (pos < length) {
    const uint8_t lead = data[pos];
    if (lead < 0x80) {
      pos++;
      continue;
    }
    const size_t sequenceLength = utf8SequenceLength(lead);
    if (sequenceLength == 0) return false;
    if (pos + sequenceLength > length) return !finalChunk;
    for (size_t i = 1; i < sequenceLength; i++) {
      if ((data[pos + i] & 0xC0) != 0x80) return false;
    }

    uint32_t codepoint = lead & ((1U << (7 - sequenceLength)) - 1);
    for (size_t i = 1; i < sequenceLength; i++) {
      codepoint = (codepoint << 6) | (data[pos + i] & 0x3F);
    }
    const bool overlong = (sequenceLength == 2 && codepoint < 0x80) || (sequenceLength == 3 && codepoint < 0x800) ||
                          (sequenceLength == 4 && codepoint < 0x10000);
    if (overlong || (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) return false;
    hasCompleteNonAscii = true;
    pos += sequenceLength;
  }
  return true;
}

#ifdef ENABLE_CHINESE_VERSION
#include "GbkToUnicodeTable.inc"

uint16_t gbkToUnicode(const uint8_t lead, const uint8_t trail) {
  if (!isGbkLead(lead) || !isGbkTrail(trail)) return 0;
  const size_t trailIndex = trail < 0x7F ? trail - 0x40 : trail - 0x41;
  return kGbkToUnicode[(static_cast<size_t>(lead) - 0x81) * 190 + trailIndex];
}

bool isStrictGbk(const uint8_t* data, const size_t length, const bool finalChunk, bool& hasCompleteCharacter) {
  size_t pos = 0;
  while (pos < length) {
    if (data[pos] < 0x80) {
      pos++;
      continue;
    }
    if (!isGbkLead(data[pos])) return false;
    if (pos + 1 >= length) return !finalChunk;
    if (gbkToUnicode(data[pos], data[pos + 1]) == 0) return false;
    hasCompleteCharacter = true;
    pos += 2;
  }
  return true;
}

#endif

// UTF-8 encoding helpers shared by the GBK and UTF-16 converters. Kept outside
// the ENABLE_CHINESE_VERSION guard so BOM-marked UTF-16 text still decodes in
// builds without the GBK table; 4-byte output covers surrogate pairs.
size_t utf8EncodedLength(const uint32_t codepoint) {
  if (codepoint < 0x80) return 1;
  if (codepoint < 0x800) return 2;
  if (codepoint < 0x10000) return 3;
  return 4;
}

size_t appendUtf8(const uint32_t codepoint, uint8_t* output) {
  const size_t length = utf8EncodedLength(codepoint);
  switch (length) {
    case 1:
      output[0] = static_cast<uint8_t>(codepoint);
      break;
    case 2:
      output[0] = static_cast<uint8_t>(0xC0 | (codepoint >> 6));
      output[1] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
      break;
    case 3:
      output[0] = static_cast<uint8_t>(0xE0 | (codepoint >> 12));
      output[1] = static_cast<uint8_t>(0x80 | ((codepoint >> 6) & 0x3F));
      output[2] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
      break;
    default:
      output[0] = static_cast<uint8_t>(0xF0 | (codepoint >> 18));
      output[1] = static_cast<uint8_t>(0x80 | ((codepoint >> 12) & 0x3F));
      output[2] = static_cast<uint8_t>(0x80 | ((codepoint >> 6) & 0x3F));
      output[3] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
      break;
  }
  return length;
}

uint16_t readUtf16Unit(const uint8_t* bytes, const bool bigEndian) {
  return bigEndian ? static_cast<uint16_t>((bytes[0] << 8) | bytes[1])
                   : static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

// Converts BOM-marked UTF-16 to UTF-8 in place. The write pass runs backwards
// from the end of the source region because CJK expands 2 -> 3 bytes, so a
// forward pass would overwrite bytes it has not read yet. A trailing lone high
// surrogate (split read) is left unconsumed so the caller can re-feed it.
TranscodeResult transcodeUtf16InPlace(uint8_t* buffer, const size_t rawLength, const size_t capacity,
                                      const bool finalChunk, const bool bigEndian) {
  (void)finalChunk;
  if (!buffer || rawLength < 2 || rawLength >= capacity) return {0, 0};
  size_t start = 0;
  if (readUtf16Unit(buffer, bigEndian) == 0xFEFF) {
    start = 2;
  }

  size_t pos = start;
  size_t utf8Length = 0;
  size_t lastComplete = start;
  while (pos + 1 < rawLength) {
    const uint16_t unit = readUtf16Unit(buffer + pos, bigEndian);
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      if (pos + 3 >= rawLength) break;  // low surrogate not buffered yet
      const uint16_t low = readUtf16Unit(buffer + pos + 2, bigEndian);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        const uint32_t codepoint =
            0x10000 + ((static_cast<uint32_t>(unit) - 0xD800) << 10) + (static_cast<uint32_t>(low) - 0xDC00);
        utf8Length += utf8EncodedLength(codepoint);
        pos += 4;
        lastComplete = pos;
        continue;
      }
    }
    utf8Length += utf8EncodedLength(unit);
    pos += 2;
    lastComplete = pos;
  }

  const size_t sourceLength = lastComplete;
  if (sourceLength == start) return {0, 0};
  if (sourceLength + utf8Length >= capacity) return {0, 0};

  size_t src = sourceLength;
  size_t out = sourceLength + utf8Length;
  while (src > start) {
    size_t unitStart = src - 2;
    uint32_t codepoint = readUtf16Unit(buffer + unitStart, bigEndian);
    if (codepoint >= 0xDC00 && codepoint <= 0xDFFF && unitStart >= start + 2) {
      const uint16_t high = readUtf16Unit(buffer + unitStart - 2, bigEndian);
      if (high >= 0xD800 && high <= 0xDBFF) {
        unitStart -= 2;
        codepoint = 0x10000 + ((static_cast<uint32_t>(high) - 0xD800) << 10) + (codepoint - 0xDC00);
      }
    }
    out -= utf8EncodedLength(codepoint);
    appendUtf8(codepoint, buffer + out);
    src = unitStart;
  }

  std::memmove(buffer, buffer + sourceLength, utf8Length);
  buffer[utf8Length] = '\0';
  return {sourceLength, utf8Length};
}

}  // namespace

bool isSerializedValueValid(const uint8_t value) {
  return value <= static_cast<uint8_t>(Encoding::Utf16Be);
}

bool isSupported(const Encoding encoding) {
  switch (encoding) {
    case Encoding::Unknown:
    case Encoding::Utf8:
      return true;
    // UTF-16 (BOM-marked) needs no lookup table, so every build can decode it.
    case Encoding::Utf16Le:
    case Encoding::Utf16Be:
      return true;
    case Encoding::Gbk:
#ifdef ENABLE_CHINESE_VERSION
      return true;
#else
      return false;
#endif
  }
  return false;
}

Encoding detect(const uint8_t* data, const size_t length, const bool finalChunk) {
  // UTF-16 is accepted only when a BOM is present. Without one there is no
  // reliable signature: CJK UTF-16LE text is e.g. `XX 4E`, which has no zero
  // bytes, so a heuristic would mis-detect GBK content and produce mojibake.
  // Every mainstream "save as Unicode" path (Windows Notepad included) writes
  // the BOM, so this covers the real-world cases without guessing.
  if (data && length >= 2) {
    if (data[0] == 0xFF && data[1] == 0xFE) return Encoding::Utf16Le;
    if (data[0] == 0xFE && data[1] == 0xFF) return Encoding::Utf16Be;
  }
#ifndef ENABLE_CHINESE_VERSION
  (void)data;
  (void)length;
  (void)finalChunk;
  return Encoding::Utf8;
#else
  bool hasCompleteUtf8 = false;
  if (isStrictUtf8(data, length, finalChunk, hasCompleteUtf8)) {
    return hasCompleteUtf8 ? Encoding::Utf8 : Encoding::Unknown;
  }
  bool hasCompleteGbk = false;
  if (isStrictGbk(data, length, finalChunk, hasCompleteGbk)) {
    return hasCompleteGbk ? Encoding::Gbk : Encoding::Unknown;
  }
  return Encoding::Utf8;
#endif
}

TranscodeResult transcodeGbkInPlace(uint8_t* buffer, size_t rawLength, const size_t capacity, const bool finalChunk) {
#ifndef ENABLE_CHINESE_VERSION
  (void)buffer;
  (void)capacity;
  (void)finalChunk;
  return {0, 0};
#else
  if (!buffer || rawLength == 0 || rawLength >= capacity) return {0, 0};

  size_t utf8Length = 0;
  size_t pos = 0;
  while (pos < rawLength) {
    if (buffer[pos] < 0x80) {
      utf8Length++;
      pos++;
      continue;
    }
    if (pos + 1 < rawLength) {
      const uint16_t mapped = gbkToUnicode(buffer[pos], buffer[pos + 1]);
      if (mapped != 0) {
        utf8Length += utf8EncodedLength(mapped);
        pos += 2;
        continue;
      }
    }
    if (!finalChunk && pos + 1 == rawLength && isGbkLead(buffer[pos])) {
      rawLength = pos;
      break;
    }
    utf8Length++;
    pos++;
  }
  if (rawLength + utf8Length >= capacity) return {0, 0};

  const size_t sourceLength = rawLength;
  size_t sourcePos = 0;
  size_t outputPos = sourceLength;
  while (sourcePos < sourceLength) {
    if (buffer[sourcePos] < 0x80) {
      buffer[outputPos++] = buffer[sourcePos++];
      continue;
    }

    uint32_t codepoint = '?';
    size_t consumed = 1;
    if (sourcePos + 1 < sourceLength) {
      const uint16_t mapped = gbkToUnicode(buffer[sourcePos], buffer[sourcePos + 1]);
      if (mapped != 0) {
        codepoint = mapped;
        consumed = 2;
      }
    }
    outputPos += appendUtf8(codepoint, buffer + outputPos);
    sourcePos += consumed;
  }

  std::memmove(buffer, buffer + sourceLength, utf8Length);
  buffer[utf8Length] = '\0';
  return {sourceLength, utf8Length};
#endif
}

TranscodeResult transcodeInPlace(uint8_t* buffer, const size_t rawLength, const size_t capacity,
                                 const bool finalChunk, const Encoding encoding) {
  switch (encoding) {
    case Encoding::Gbk:
      return transcodeGbkInPlace(buffer, rawLength, capacity, finalChunk);
    case Encoding::Utf16Le:
      return transcodeUtf16InPlace(buffer, rawLength, capacity, finalChunk, /*bigEndian=*/false);
    case Encoding::Utf16Be:
      return transcodeUtf16InPlace(buffer, rawLength, capacity, finalChunk, /*bigEndian=*/true);
    default:
      return {0, 0};
  }
}

size_t encodeUtf8(const uint32_t codepoint, uint8_t* output) { return appendUtf8(codepoint, output); }

size_t gbkSourceLength(const uint8_t* utf8, const size_t utf8Length) {  size_t sourceLength = 0;
  size_t pos = 0;
  while (pos < utf8Length) {
    const size_t sequenceLength = std::max<size_t>(utf8SequenceLength(utf8[pos]), 1);
    sourceLength += sequenceLength == 1 ? 1 : 2;
    pos += std::min(sequenceLength, utf8Length - pos);
  }
  return sourceLength;
}

}  // namespace txt_encoding
