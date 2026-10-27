#include "PinyinTables.h"

#include <cstring>

namespace pinyin {

namespace {

constexpr size_t kMaxTextBytes = 255;
constexpr size_t kMaxQueryBytes = 64;

unsigned char lowerAscii(const unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
bool isAsciiLetter(const uint32_t c) { return c >= 'a' && c <= 'z'; }
bool isAsciiDigit(const uint32_t c) { return c >= '0' && c <= '9'; }
bool isSpace(const uint32_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Bounded UTF-8 decode. The shared decoder stops on NUL and reads ahead for
// continuation bytes, which is wrong here: index records and queries are
// length-carrying views that may end mid-sequence, and reading the byte after
// `length` would be reading whatever follows in the record.
bool nextCodepoint(const char* s, const size_t length, size_t* index, uint32_t* out) {
  if (*index >= length) return false;
  unsigned char c = static_cast<unsigned char>(s[(*index)++]);
  if (c < 128) {
    *out = c;
    return true;
  }
  unsigned following;
  uint32_t value, minimum;
  if (c >= 0xc2 && c <= 0xdf) {
    following = 1;
    value = c & 31;
    minimum = 128;
  } else if (c >= 0xe0 && c <= 0xef) {
    following = 2;
    value = c & 15;
    minimum = 2048;
  } else if (c >= 0xf0 && c <= 0xf4) {
    following = 3;
    value = c & 7;
    minimum = 65536;
  } else {
    return false;
  }
  while (following--) {
    if (*index >= length) return false;
    c = static_cast<unsigned char>(s[(*index)++]);
    if ((c & 0xc0) != 0x80) return false;
    value = (value << 6) | (c & 63);
  }
  if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  *out = value;
  return true;
}

bool validUtf8(const char* s, const size_t length) {
  size_t i = 0;
  uint32_t cp;
  while (i < length) {
    if (!nextCodepoint(s, length, &i, &cp)) return false;
  }
  return true;
}

bool equalAsciiLower(const char* a, const char* b, const size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (lowerAscii(static_cast<unsigned char>(a[i])) != lowerAscii(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

// Bit j of the state set means the first j query units matched; every step can
// start a new substring. The set folds every polyphonic branch, so the match
// never enumerates pronunciation combinations.
bool advance(uint64_t* state, const unsigned char c, const uint64_t masks[128], const uint64_t finish) {
  const uint64_t matched = *state & masks[c];
  *state = (matched << 1) | 1;
  return (matched & finish) != 0;
}

}  // namespace

PydbValidity PinyinTables::adopt(const uint8_t* data, const size_t size) {
  const PydbValidity validity = validatePydb(data, size);
  if (validity != PydbValidity::Ok) return validity;
  PydbHeader header;
  memcpy(&header, data, sizeof(header));
  image_ = data;
  size_ = size;
  header_ = header;
  return PydbValidity::Ok;
}

uint16_t PinyinTables::readU16(const uint32_t offset) const {
  uint16_t value;
  memcpy(&value, image_ + offset, sizeof(value));
  return value;
}

uint32_t PinyinTables::readU32(const uint32_t offset) const {
  uint32_t value;
  memcpy(&value, image_ + offset, sizeof(value));
  return value;
}

uint32_t PinyinTables::groupFor(const uint32_t cp) const {
  size_t low = 0;
  size_t high = header_.codepointCount;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (readU32(header_.codepointsOff + static_cast<uint32_t>(mid) * 4) < cp) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  if (low >= header_.codepointCount ||
      readU32(header_.codepointsOff + static_cast<uint32_t>(low) * 4) != cp)
    return UINT32_MAX;
  // Clamped, not trusted: a forged group id drawn from a card the user can
  // write must not index past the offsets table.
  const uint16_t group = readU16(header_.groupsOff + static_cast<uint32_t>(low) * 2);
  return group < header_.groupCount ? group : UINT32_MAX;
}

bool PinyinTables::match(const char* text, const size_t textLength, const char* query,
                         const size_t queryLength) const {
  if (!valid() || text == nullptr || query == nullptr) return false;
  if (textLength > kMaxTextBytes || queryLength > kMaxQueryBytes) return false;
  if (!validUtf8(text, textLength) || !validUtf8(query, queryLength)) return false;
  if (queryLength == 0) return true;

  // Literal substring first: Chinese and symbols outside the table stay
  // searchable, and ASCII matches do not depend on the Pinyin walk at all.
  if (queryLength <= textLength) {
    for (size_t i = 0; i + queryLength <= textLength; i++) {
      if (equalAsciiLower(text + i, query, queryLength)) return true;
    }
  }

  uint64_t masks[128] = {};
  size_t i = 0;
  size_t n = 0;
  while (i < queryLength) {
    uint32_t cp;
    if (!nextCodepoint(query, queryLength, &i, &cp)) return false;
    if (isSpace(cp)) continue;
    if (cp == 0xfc || cp == 0xdc) cp = 'v';  // ü and Ü mean v in a Pinyin query
    if (cp < 128) cp = lowerAscii(static_cast<unsigned char>(cp));
    if (!isAsciiLetter(cp) && !isAsciiDigit(cp)) return false;
    masks[cp] |= UINT64_C(1) << n++;
  }
  if (n == 0) return true;
  const uint64_t finish = UINT64_C(1) << (n - 1);

  uint64_t full = 1;
  uint64_t initials = 1;
  bool englishWord = false;
  i = 0;
  while (i < textLength) {
    uint32_t cp;
    if (!nextCodepoint(text, textLength, &i, &cp)) return false;
    if (cp < 128) {
      cp = lowerAscii(static_cast<unsigned char>(cp));
      if (isAsciiLetter(cp) || isAsciiDigit(cp)) {
        if (advance(&full, static_cast<unsigned char>(cp), masks, finish)) return true;
        // English initials follow word boundaries; digits always start one.
        if ((!englishWord || isAsciiDigit(cp)) && advance(&initials, static_cast<unsigned char>(cp), masks, finish))
          return true;
        englishWord = isAsciiLetter(cp);
      } else {
        englishWord = false;
      }
      continue;
    }
    englishWord = false;
    const uint32_t group = groupFor(cp);
    if (group == UINT32_MAX) {
      full = initials = 1;
      continue;
    }
    uint64_t nextFull = 1;
    uint64_t nextInitials = 1;
    const uint32_t begin = readU16(header_.groupOffsetsOff + group * 2);
    const uint32_t end = readU16(header_.groupOffsetsOff + (group + 1) * 2);
    for (uint32_t j = begin; j < end && j < header_.readingCount; j++) {
      const char* reading = syllable(readU16(header_.readingsOff + j * 2));
      if (reading == nullptr) continue;
      uint64_t branch = full;
      uint64_t initialBranch = initials;
      if (advance(&initialBranch, static_cast<unsigned char>(reading[0]), masks, finish)) return true;
      nextInitials |= initialBranch;
      for (const char* at = reading; *at; ++at) {
        if (advance(&branch, static_cast<unsigned char>(*at), masks, finish)) return true;
      }
      nextFull |= branch;
    }
    full = nextFull;
    initials = nextInitials;
  }
  return false;
}

size_t PinyinTables::syllableCount() const { return valid() ? header_.syllableCount : 0; }

const char* PinyinTables::syllable(const size_t index) const {
  if (!valid() || index >= header_.syllableCount) return nullptr;
  const uint32_t offset = readU16(header_.syllableOffsetsOff + static_cast<uint32_t>(index) * 2);
  if (offset >= header_.syllablesLen) return nullptr;
  return reinterpret_cast<const char*>(image_ + header_.syllablesOff + offset);
}

int PinyinTables::syllableIndexFor(const char* syllableText) const {
  size_t low = 0;
  size_t high = header_.syllableCount;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    const char* at = syllable(mid);
    const int order = at ? strcmp(at, syllableText) : -1;
    if (order < 0) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  if (low >= header_.syllableCount) return -1;
  const char* found = syllable(low);
  return (found && strcmp(found, syllableText) == 0) ? static_cast<int>(low) : -1;
}

size_t PinyinTables::candidates(const char* syllableText, uint32_t* out, const size_t cap,
                                const size_t skip) const {
  if (!valid() || syllableText == nullptr || out == nullptr || cap == 0) return 0;
  char normalized[9];
  const size_t n = strnlen(syllableText, sizeof(normalized));
  if (n == 0 || n >= sizeof(normalized)) return 0;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = lowerAscii(static_cast<unsigned char>(syllableText[i]));
    if (!isAsciiLetter(c)) return 0;
    normalized[i] = static_cast<char>(c);
  }
  normalized[n] = 0;
  const int index = syllableIndexFor(normalized);
  if (index < 0) return 0;

  const uint32_t begin = readU32(header_.imeOffsetsOff + static_cast<uint32_t>(index) * 4);
  const uint32_t end = readU32(header_.imeOffsetsOff + (static_cast<uint32_t>(index) + 1) * 4);
  if (begin > end || end > header_.imeCharCount) return 0;
  uint32_t count = end - begin;
  if (skip >= count) return 0;
  count -= static_cast<uint32_t>(skip);
  if (count > cap) count = static_cast<uint32_t>(cap);
  const uint32_t first = begin + static_cast<uint32_t>(skip);
  for (uint32_t i = 0; i < count; i++) {
    out[i] = readU16(header_.imeCharsOff + (first + i) * 2);
  }
  return count;
}

size_t PinyinTables::phraseCount() const { return valid() ? header_.phraseCount : 0; }

bool PinyinTables::phrase(const size_t index, const char** roman, const char** text) const {
  if (!valid() || index >= header_.phraseCount || roman == nullptr || text == nullptr) return false;
  const uint32_t begin = readU32(header_.phraseOffsetsOff + static_cast<uint32_t>(index) * 4);
  const uint32_t end = readU32(header_.phraseOffsetsOff + (static_cast<uint32_t>(index) + 1) * 4);
  if (begin > end || end > header_.phraseBlobLen) return false;
  const char* base = reinterpret_cast<const char*>(image_ + header_.phraseBlobOff);
  const char* romanAt = base + begin;
  const void* romanEnd = memchr(romanAt, 0, end - begin);
  if (romanEnd == nullptr) return false;
  const char* textAt = static_cast<const char*>(romanEnd) + 1;
  const size_t remain = end - static_cast<uint32_t>(textAt - base);
  if (remain == 0 || memchr(textAt, 0, remain) == nullptr) return false;
  *roman = romanAt;
  *text = textAt;
  return true;
}

}  // namespace pinyin
