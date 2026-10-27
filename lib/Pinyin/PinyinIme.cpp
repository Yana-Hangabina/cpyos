#include "PinyinIme.h"

#include <cstdio>
#include <cstring>

namespace pinyin {

namespace {

constexpr int kNegativeScore = -1000000;
// Segment scores keep a phrase (×100+40) ahead of any single-character
// composition (×100-10) of the same length, so the lexicon wins ties.
constexpr int kPhraseBonus = 40;
constexpr int kGlyphPenalty = -10;
// Only the first candidate of a syllable feeds the composition path; the full
// list is paged straight from the tables.
constexpr size_t kGlyphCap = 1;
// Continuation suggestions look back at most this many bytes of committed text.
constexpr size_t kContinuationBackMax = 24;

char t9Digit(const char c) {
  static const char kMap[] = "22233344455566677778889999";
  return c >= 'a' && c <= 'z' ? kMap[c - 'a'] : 0;
}

bool prefixMatch(const char* raw, const char* roman, const bool nine) {
  const size_t length = strlen(roman);
  if (strlen(raw) < length) return false;
  for (size_t i = 0; i < length; i++) {
    const char expected = nine ? t9Digit(roman[i]) : roman[i];
    if (!expected || raw[i] != expected) return false;
  }
  return true;
}

bool preferredOk(const char* roman, const char* preferred, const size_t at) {
  return at != 0 || preferred == nullptr || preferred[0] == 0 ||
         strncmp(roman, preferred, strlen(preferred)) == 0;
}

// Codepoint to UTF-8 into a 5-byte buffer.
void glyphUtf8(const uint32_t cp, char text[5]) {
  if (cp < 128) {
    text[0] = static_cast<char>(cp);
    text[1] = 0;
  } else if (cp < 2048) {
    text[0] = static_cast<char>(0xc0 | (cp >> 6));
    text[1] = static_cast<char>(0x80 | (cp & 63));
    text[2] = 0;
  } else {
    text[0] = static_cast<char>(0xe0 | (cp >> 12));
    text[1] = static_cast<char>(0x80 | ((cp >> 6) & 63));
    text[2] = static_cast<char>(0x80 | (cp & 63));
    text[3] = 0;
  }
}

}  // namespace

struct PinyinIme::Path {
  int score;
  char text[kImeTextMax];
  char roman[kImeRawMax + 1];
};

struct PinyinIme::GlyphCache {
  uint32_t first;
  uint8_t count;
  uint8_t filled;
  uint8_t reserved[2];
};

// The trailing glyph cache is over-allocated by one entry per syllable, the
// classic flexible-array layout: sessionBytes() sizes the storage and
// attach() zeroes exactly `syllableCount` entries.
struct PinyinIme::Session {
  ImeResult result;
  Path paths[kImeRawMax + 1];
  uint16_t matches[512];
  uint8_t seenChars[(0x9FFF - 0x4E00 + 8) / 8];
  GlyphCache glyphs[1];
};

bool PinyinIme::addCandidate(ImeResult& out, size_t& seenCount, const size_t skip, const char* text, const char* roman,
                             const size_t consume) {
  if (strlen(text) >= kImeTextMax) return false;
  if (seenCount++ < skip) return false;
  if (out.count == kImeCandidateCap) {
    out.hasMore = true;
    return true;
  }
  ImeCandidate& item = out.items[out.count++];
  snprintf(item.text, sizeof(item.text), "%s", text);
  snprintf(item.roman, sizeof(item.roman), "%s", roman);
  item.consume = consume;
  return false;
}

void PinyinIme::relax(Path* paths, const size_t at, const size_t length, const char* text, const char* roman,
                      const int score) {
  Path& from = paths[at];
  Path& next = paths[at + length];
  const size_t used = strlen(from.text);
  const size_t more = strlen(text);
  if (used + more >= sizeof(next.text) || from.score + score <= next.score) return;
  next.score = from.score + score;
  memcpy(next.text, from.text, used);
  memcpy(next.text + used, text, more + 1);
  const size_t romanUsed = strlen(from.roman);
  memcpy(next.roman, from.roman, romanUsed);
  memcpy(next.roman + romanUsed, roman, strlen(roman) + 1);
}

PinyinIme::~PinyinIme() { end(); }

size_t PinyinIme::sessionBytes(const PinyinTables& tables) {
  const size_t count = tables.syllableCount();
  if (count == 0 || count > 512) return 0;
  return offsetof(Session, glyphs) + count * sizeof(GlyphCache);
}

bool PinyinIme::begin(const PinyinTables& tables) {
  const size_t bytes = sessionBytes(tables);
  if (bytes == 0) return false;
  memory::ByteBuffer storage = memory::makePsramByteBufferUninitializedNoThrow(bytes);
  if (!storage) return false;
  end();
  owned_ = std::move(storage);
  return attach(tables, owned_.get(), bytes);
}

bool PinyinIme::beginWithStorage(const PinyinTables& tables, uint8_t* storage, const size_t size) {
  if (sessionBytes(tables) == 0 || storage == nullptr || size < sessionBytes(tables)) return false;
  if (reinterpret_cast<uintptr_t>(storage) % alignof(Session) != 0) return false;
  end();
  return attach(tables, storage, size);
}

bool PinyinIme::attach(const PinyinTables& tables, uint8_t* storage, const size_t size) {
  Session& ws = *reinterpret_cast<Session*>(storage);
  memset(&ws.result, 0, sizeof(ws.result));
  memset(ws.glyphs, 0, tables.syllableCount() * sizeof(GlyphCache));
  tables_ = &tables;
  storage_ = storage;
  storageSize_ = size;
  result_ = &ws.result;
  syllableCount_ = tables.syllableCount();
  return true;
}

void PinyinIme::end() {
  owned_.reset();
  storage_ = nullptr;
  storageSize_ = 0;
  tables_ = nullptr;
  result_ = nullptr;
  syllableCount_ = 0;
}

bool PinyinIme::firstGlyph(const size_t syllableIndex, uint32_t& cp) {
  GlyphCache& entry = session().glyphs[syllableIndex];
  if (!entry.filled) {
    uint32_t points[kGlyphCap];
    entry.count =
        static_cast<uint8_t>(tables_->candidates(tables_->syllable(syllableIndex), points, kGlyphCap, 0));
    entry.first = entry.count ? points[0] : 0;
    entry.filled = 1;
  }
  cp = entry.first;
  return entry.count != 0;
}

bool PinyinIme::digits(const char* roman, char* out, const size_t cap) {
  if (roman == nullptr || out == nullptr || cap == 0) return false;
  const size_t n = strlen(roman);
  if (n >= cap) return false;
  for (size_t i = 0; i < n; i++) {
    const char mapped = t9Digit(roman[i]);
    if (!mapped) return false;
    out[i] = mapped;
  }
  out[n] = 0;
  return true;
}

// Suggestions after committed text: the longest phrase suffix that continues
// the text, never a guess at unconfirmed Pinyin.
bool PinyinIme::continuations(const char* preceding, const size_t skip) {
  ImeResult& out = *result_;
  size_t seenCount = 0;
  const size_t bytes = preceding ? strlen(preceding) : 0;
  const size_t phraseCount = tables_->phraseCount();
  for (size_t back = bytes < kContinuationBackMax ? bytes : kContinuationBackMax; back > 0; back--) {
    const char* suffix = preceding + bytes - back;
    if ((static_cast<unsigned char>(*suffix) & 192) == 128) continue;  // mid-codepoint tail
    bool matched = false;
    for (size_t i = 0; i < phraseCount; i++) {
      const char* roman;
      const char* text;
      if (!tables_->phrase(i, &roman, &text)) continue;
      if (strncmp(text, suffix, back) != 0 || strlen(text) <= back) continue;
      matched = true;
      bool duplicate = false;
      for (size_t j = 0; j < i && !duplicate; j++) {
        const char* otherRoman;
        const char* otherText;
        if (!tables_->phrase(j, &otherRoman, &otherText)) continue;
        duplicate = strncmp(otherText, suffix, back) == 0 && strcmp(text + back, otherText + back) == 0;
      }
      if (!duplicate && addCandidate(out, seenCount, skip, text + back, "", 0)) return true;
    }
    if (matched) break;
  }
  return true;
}

bool PinyinIme::candidatesPage(const char* raw, const bool nine, const char* preferred, const char* preceding,
                               const size_t skip) {
  if (!ready() || raw == nullptr) return false;
  ImeResult& out = *result_;
  out.count = 0;
  out.hasMore = false;

  const size_t n = strnlen(raw, kImeRawMax + 1);
  if (n > kImeRawMax) return false;
  for (size_t i = 0; i < n; i++) {
    if (nine ? (raw[i] < '2' || raw[i] > '9') : (raw[i] < 'a' || raw[i] > 'z')) return false;
  }
  if (n == 0) return continuations(preceding, skip);

  Session& ws = session();
  Path* paths = ws.paths;
  memset(paths, 0, (n + 1) * sizeof(Path));
  for (size_t i = 1; i <= n; i++) paths[i].score = kNegativeScore;

  const size_t phraseCount = tables_->phraseCount();
  for (size_t at = 0; at < n; at++) {
    if (paths[at].score < 0) continue;
    for (size_t i = 0; i < phraseCount; i++) {
      const char* roman;
      const char* text;
      if (!tables_->phrase(i, &roman, &text)) continue;
      if (preferredOk(roman, preferred, at) && prefixMatch(raw + at, roman, nine))
        relax(paths, at, strlen(roman), text, roman, static_cast<int>(strlen(roman)) * 100 + kPhraseBonus);
    }
    for (size_t i = 0; i < syllableCount_; i++) {
      const char* roman = tables_->syllable(i);
      if (roman == nullptr) break;
      if (!preferredOk(roman, preferred, at) || !prefixMatch(raw + at, roman, nine)) continue;
      uint32_t cp;
      if (firstGlyph(i, cp)) {
        char text[5];
        glyphUtf8(cp, text);
        relax(paths, at, strlen(roman), text, roman, static_cast<int>(strlen(roman)) * 100 + kGlyphPenalty);
      }
    }
  }

  size_t seenCount = 0;
  bool bestKnown = false;
  for (size_t i = 0; i < phraseCount; i++) {
    const char* roman;
    const char* text;
    if (!tables_->phrase(i, &roman, &text)) continue;
    if (strlen(roman) != n || !preferredOk(roman, preferred, 0) || !prefixMatch(raw, roman, nine)) continue;
    if (strcmp(paths[n].text, text) == 0) bestKnown = true;
    if (addCandidate(out, seenCount, skip, text, roman, n)) return true;
  }
  if (paths[n].score >= 0 && !bestKnown &&
      addCandidate(out, seenCount, skip, paths[n].text, paths[n].roman, n))
    return true;

  for (size_t length = n; length > 0; length--) {
    if (length != n) {
      for (size_t i = 0; i < phraseCount; i++) {
        const char* roman;
        const char* text;
        if (!tables_->phrase(i, &roman, &text)) continue;
        if (strlen(roman) == length && preferredOk(roman, preferred, 0) && prefixMatch(raw, roman, nine))
          if (addCandidate(out, seenCount, skip, text, roman, length)) return true;
      }
    }
    memset(ws.seenChars, 0, sizeof(ws.seenChars));
    size_t matches = 0;
    for (size_t i = 0; i < syllableCount_; i++) {
      const char* roman = tables_->syllable(i);
      if (roman == nullptr) break;
      if (strlen(roman) == length && preferredOk(roman, preferred, 0) && prefixMatch(raw, roman, nine))
        ws.matches[matches++] = static_cast<uint16_t>(i);
    }
    // Homophones of every matching reading alternate by rank, so one reading's
    // long list can never hide another reading behind it.
    for (size_t rank = 0; matches != 0; rank++) {
      bool found = false;
      for (size_t m = 0; m < matches; m++) {
        const char* roman = tables_->syllable(ws.matches[m]);
        uint32_t cp;
        if (roman == nullptr || tables_->candidates(roman, &cp, 1, rank) != 1) continue;
        found = true;
        if (cp < 0x4e00 || cp > 0x9fff) continue;
        const unsigned index = cp - 0x4e00;
        const unsigned mask = 1u << (index & 7);
        if (ws.seenChars[index / 8] & mask) continue;
        ws.seenChars[index / 8] |= static_cast<uint8_t>(mask);
        char text[5];
        glyphUtf8(cp, text);
        if (length == n && paths[n].score >= 0 && strcmp(text, paths[n].text) == 0) continue;
        if (addCandidate(out, seenCount, skip, text, roman, length)) return true;
      }
      if (!found) break;
    }
  }
  return true;
}

}  // namespace pinyin
