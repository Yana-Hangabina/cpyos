#pragma once

// Lazy loader for the hyphenation tries that no longer ride in the firmware image.
//
// See HyphenationEmbedding.h for why: 351,458 bytes of .rodata against an app
// partition that was 98.5% full. Two sources, in the same order PinyinDb uses:
//
//   1. `/hyphen.dat` on the SD card -- the copy a user can replace, or upload
//      through the web file manager, without reflashing;
//   2. the copy burnt into the data/spiffs partition at HYPN_PARTITION_OFFSET by
//      the multi-image flash command, so a freshly flashed device has working
//      hyphenation before it ever sees a card.
//
// Lifetime rules, and the reasoning behind them, because both are load-bearing:
//
//   - One language at a time, loaded on first use. Reading an English book never
//     pays for the 206 KB German trie.
//   - A loaded trie is kept for the whole session and is NEVER evicted. The
//     registry installs the returned pointer into a descriptor that
//     LanguageHyphenator holds by reference, so freeing a buffer behind a live
//     descriptor would leave layout reading freed PSRAM. Keeping them costs at
//     most 351 KB of PSRAM in the pathological case of a session that reads books
//     in all eleven languages, against 3.5-6 MB free.
//   - There is no release(). Deep sleep reboots the chip, so the session is the
//     natural lifetime, and an explicit release would need to reach back into the
//     registry to un-install every descriptor first.
//   - The container's header and index are cached, and re-read if the card file's
//     size or modification time changes. That makes "the user replaced the file,
//     then opened a book in another language" pick up the new data, while a
//     language that is already installed keeps the bytes it was installed with --
//     which is safe precisely because those bytes are never freed.
//
// Every failure path returns false and leaves the caller's descriptor untouched,
// which lands on the nullptr hyphenator path the code already had: explicit and
// soft hyphens still break, and ParsedText's emergency fallback still applies.
// A missing dictionary must never be a new kind of failure.

#include <Memory.h>

#include "HyphenationDataFormat.h"

namespace hyphenation {

// Where the bundled copy sits inside the data/spiffs partition. The partition
// (0xe70000, 1.5 MB per partitions_readpico.csv) is not mounted as a filesystem by
// anything in this tree, so it is used as a plain blob store with a fixed map:
//
//   offset 0x00000  pinyin.dat   (349,500 bytes today, see lib/Pinyin/README.md)
//   offset 0x80000  DO NOT USE -- 12 bytes here are not 0xFF in the factory image
//   offset 0x90000  hyphen.dat   (about 352 KB)
//
// The 0x80000 gap is deliberate. Those 12 bytes (`01 00 00 00` at +0, `02 00 00 00
// 9a 98 43 47` at +0x18) are present in the pristine full backup taken before this
// project ever wrote to the device, they are the ONLY non-erased bytes in the whole
// 3.38 MB partition, and nothing in this tree reads them. Their purpose is unknown,
// so the container is placed past them rather than over them: routing around an
// unexplained factory artifact costs one line in a flash command, and overwriting
// one is not reversible.
//
// 0x90000 still leaves the Pinyin database 232 KB of growth room. This constant is
// the only source of truth for the offset: scripts/generate_data_manifest.py reads it
// out of this header, checks the whole layout for overlaps and for the reserved gap,
// and emits the addresses the flashing tool writes. The loader additionally
// bounds-checks every read against the partition size, so a map that somehow did get
// violated fails here rather than reading another blob's bytes.
inline constexpr uint32_t HYPN_PARTITION_OFFSET = 0x90000u;

class HyphenationStore {
 public:
  static HyphenationStore& shared();

  // Fills `out` with the trie for a two-letter ISO 639-1 primary tag. Idempotent
  // for a language that is already loaded. Returns false when the language is not
  // in the container, no container is reachable, PSRAM cannot fund the blob, or
  // validation fails -- all of which mean "hyphenation unavailable", not an error
  // the user can act on, so they are logged and otherwise swallowed.
  bool patterns(const char lang2[2], SerializedHyphenationPatterns& out);

  // Called once from the main task during boot, while the stack is in internal
  // RAM.  Reads the container and all tries into PSRAM so the render task (whose
  // stack is in PSRAM) never has to touch internal flash for hyphenation.
  void preloadAll();

  // PDF pages need the PSRAM more than optional hyphenation.  Releases every
  // loaded trie and disables lazy loading for the rest of this boot, so a later
  // EPUB render cannot touch internal flash from the PSRAM-stacked render task.
  void releaseAll();

  // Two-letter tag of the most recently loaded language, or nullptr. Diagnostic.
  const char* loadedLanguage() const { return haveLang_ ? lang_ : nullptr; }

 private:
  struct Slot {
    char lang[2] = {0, 0};
    bool used = false;
    memory::ByteBuffer buffer;
    SerializedHyphenationPatterns patterns{};
  };

  enum class Source { None, Card, Partition };

  bool ensureIndex();
  bool readIndexFromCard();
#if !defined(SIMULATOR)
  bool readIndexFromPartition();
#endif
  // `outOfMemory` distinguishes a transient refusal from a permanent one: only the
  // permanent kind is latched, because PSRAM pressure comes and goes.
  bool loadBlob(const HyphEntry& entry, Slot& slot, bool& outOfMemory);
  void rememberFailure(const char lang2[2]);
  void invalidateIndex();

  // Header plus the largest index this format allows, so the whole prefix is read
  // in one pass into a fixed internal buffer (384 bytes -- small enough that it
  // does not matter, and it is not a per-language cost).
  static constexpr size_t kPrefixBytes = HYPN_HEADER_SIZE + HYPN_MAX_ENTRIES * HYPN_ENTRY_SIZE;

  uint8_t prefix_[kPrefixBytes] = {};
  uint32_t entryCount_ = 0;
  Source source_ = Source::None;
  uint64_t cardSize_ = 0;
  uint32_t cardMtime_ = 0;
  // The partition is probed at most once. Unlike the card it cannot appear later,
  // and a device flashed without the data image answers every probe with the same
  // "no container here", which would otherwise be logged once per section.
  bool partitionProbed_ = false;

  Slot slots_[HYPN_MAX_ENTRIES] = {};
  size_t slotCount_ = 0;
  char lang_[2] = {0, 0};
  bool haveLang_ = false;
  bool disabled_ = false;
};

}  // namespace hyphenation
