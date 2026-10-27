#pragma once

// Offline Chinese composition: continuous Pinyin, T9 digits, phrase-first word
// building with per-character fallback and homophone paging. Nothing here
// touches the network, and every buffer is caller-owned — the session lives in
// PSRAM (about 19 KB) so a keystroke never allocates.
//
// Ported from read-pico-reader (kiikoread, Apache-2.0) main/ui/ui_ime.c. The
// candidate order is part of the port: exact phrases first, then the best
// composition, then remaining phrases of the consumed prefix, then every
// homophone of the first reading, deduplicated across batches without ever
// truncating a reading's character list.

#include <cstddef>
#include <cstdint>

#include "Memory.h"
#include "PinyinTables.h"

namespace pinyin {

inline constexpr size_t kImeRawMax = 48;
inline constexpr size_t kImeCandidateCap = 24;
inline constexpr size_t kImeTextMax = 96;

struct ImeCandidate {
  char text[kImeTextMax];
  char roman[kImeRawMax + 1];
  // How many Pinyin units the candidate consumes from the raw input.
  size_t consume;
};

struct ImeResult {
  ImeCandidate items[kImeCandidateCap];
  size_t count = 0;
  bool hasMore = false;
};

class PinyinIme {
 public:
  PinyinIme() = default;
  ~PinyinIme();
  PinyinIme(const PinyinIme&) = delete;
  PinyinIme& operator=(const PinyinIme&) = delete;

  // Bytes a session needs; the IME is only available while the tables are.
  static size_t sessionBytes(const PinyinTables& tables);

  // One session at a time, main task only: the workspace caches syllable
  // lookups and is reused across keystrokes. The device path allocates the
  // session in PSRAM; beginWithStorage serves hosts and tests.
  bool begin(const PinyinTables& tables);
  bool beginWithStorage(const PinyinTables& tables, uint8_t* storage, size_t size);
  void end();
  bool ready() const { return tables_ != nullptr && storage_ != nullptr; }

  // Rebuild the candidate page for `raw`. `nine` reads T9 digits instead of
  // letters. `preferred` pins the leading syllable (T9 disambiguation), and
  // `preceding` is the committed text before the caret, used for continuation
  // suggestions when `raw` is empty. `skip` pages into the enumeration and
  // never truncates a reading. Returns false on malformed input; an empty page
  // is a success with `hasMore` false.
  bool candidatesPage(const char* raw, bool nine, const char* preferred, const char* preceding, size_t skip);
  const ImeResult& result() const { return *result_; }

  // Letters to T9 digits, rejecting anything else.
  static bool digits(const char* roman, char* out, size_t cap);

 private:
  struct Path;
  struct GlyphCache;
  struct Session;

  // Appends one candidate honouring the page skip; true means the page is
  // full and the enumeration must stop.
  static bool addCandidate(ImeResult& out, size_t& seenCount, size_t skip, const char* text, const char* roman,
                           size_t consume);
  // Relaxation into paths[at + length]; the first writer wins a tie.
  static void relax(Path* paths, size_t at, size_t length, const char* text, const char* roman, int score);

  bool attach(const PinyinTables& tables, uint8_t* storage, size_t size);
  bool continuations(const char* preceding, size_t skip);
  bool firstGlyph(size_t syllableIndex, uint32_t& cp);
  Session& session() const { return *reinterpret_cast<Session*>(storage_); }

  memory::ByteBuffer owned_;
  uint8_t* storage_ = nullptr;
  size_t storageSize_ = 0;
  const PinyinTables* tables_ = nullptr;
  ImeResult* result_ = nullptr;
  size_t syllableCount_ = 0;
};

}  // namespace pinyin
