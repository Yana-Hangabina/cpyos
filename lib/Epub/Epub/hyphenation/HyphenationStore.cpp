#include "HyphenationStore.h"

#include "HyphenationEmbedding.h"

// The whole loader only exists in the configuration that needs it. Where the tries
// are still compiled into the image (C3, the simulator, and the host tests that
// assert hyphenation quality against the embedded arrays) this translation unit is
// empty, so it costs those builds nothing and cannot be linked against by accident.
#if !defined(CROSSPOINT_EMBED_HYPHENATION)

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#if !defined(SIMULATOR)
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#endif

namespace hyphenation {

namespace {

constexpr const char* kTag = "HYPH";
constexpr size_t kChunkBytes = 16 * 1024;
// Other PSRAM users (fonts, the JJWXC containers, the reader's page cache) keep
// working while a trie loads. Same reserve PinyinDb uses.
constexpr size_t kContiguousReserve = 64 * 1024;

#if !defined(SIMULATOR)
// esp_partition_read disables the flash cache.  A render task whose stack lives
// in PSRAM must never enter that path: the IDF cache guard asserts and reboots
// the device.  preloadAll() runs on the main task; this check is the safety net
// for languages it did not get to (or a missing preload call).
bool stackIsInPsram() { return esp_ptr_external_ram(esp_cpu_get_sp()); }
#else
bool stackIsInPsram() { return false; }
#endif

}  // namespace

HyphenationStore& HyphenationStore::shared() {
  static HyphenationStore instance;
  return instance;
}

bool HyphenationStore::patterns(const char lang2[2], SerializedHyphenationPatterns& out) {
  if (disabled_) return false;
  if (lang2 == nullptr || lang2[0] == '\0' || lang2[1] == '\0') return false;

  // The cache holds successes and failures alike. A failure has to be remembered:
  // this is called once per section, and re-reading plus re-hashing a 206 KB blob
  // that already failed its CRC -- or re-logging "no trie for 'xx'" -- on every one
  // of them would be both slow and noisy.
  for (size_t i = 0; i < slotCount_; ++i) {
    if (slots_[i].used && slots_[i].lang[0] == lang2[0] && slots_[i].lang[1] == lang2[1]) {
      out = slots_[i].patterns;
      return slots_[i].patterns.data != nullptr;
    }
  }

  // No container at all is deliberately NOT latched: it costs one stat per section,
  // and a card inserted after boot should still work. Everything below this point
  // has a container, so its outcome is final for the session.
  if (!ensureIndex()) return false;

  HyphEntry entry;
  if (!hyphFindEntry(prefix_ + HYPN_INDEX_OFFSET, entryCount_, lang2, entry)) {
    // Not an error: a book in a language this container does not carry simply gets
    // no pattern breaks, exactly as it did when the tries were embedded and the
    // language was not one of the eleven.
    LOG_INF(kTag, "no trie for '%c%c' in the container (%u languages)", lang2[0], lang2[1], (unsigned)entryCount_);
    rememberFailure(lang2);
    return false;
  }

  if (slotCount_ >= HYPN_MAX_ENTRIES) {
    LOG_ERR(kTag, "no free slot for '%c%c' (%u already loaded)", lang2[0], lang2[1], (unsigned)slotCount_);
    return false;
  }
  Slot& slot = slots_[slotCount_];
  bool outOfMemory = false;
  if (!loadBlob(entry, slot, outOfMemory)) {
    // A permanent refusal (corrupt blob, language absent, unreadable file) is latched
    // so it is not retried per section. Running out of PSRAM is not latched: that is
    // a transient condition -- fonts and the JJWXC containers come and go -- and
    // turning it into a session-long "this language cannot hyphenate" would be a
    // decision made from a moment that has already passed.
    if (!outOfMemory) rememberFailure(lang2);
    return false;
  }

  slot.lang[0] = lang2[0];
  slot.lang[1] = lang2[1];
  slot.used = true;
  slot.patterns = SerializedHyphenationPatterns{entry.rootOffset, slot.buffer.get(), entry.blobSize};
  ++slotCount_;

  lang_[0] = lang2[0];
  lang_[1] = lang2[1];
  haveLang_ = true;
  out = slot.patterns;
  LOG_INF(kTag, "loaded '%c%c' trie: %u bytes from %s, crc ok", lang2[0], lang2[1], (unsigned)entry.blobSize,
          source_ == Source::Card ? HYPHEN_DB_PATH : "the data partition");
  return true;
}

// Boot-time warm-up: the render task's stack lives in PSRAM, and every
// esp_partition_read there trips the IDF cache guard.  Loading here, on the main
// task, turns later patterns() calls into pure PSRAM cache hits.
void HyphenationStore::releaseAll() {
  for (size_t i = 0; i < HYPN_MAX_ENTRIES; ++i) {
    slots_[i].buffer.reset();
    slots_[i].patterns = {};
    slots_[i].used = false;
  }
  slotCount_ = 0;
  entryCount_ = 0;
  source_ = Source::None;
  haveLang_ = false;
  disabled_ = true;
  LOG_INF(kTag, "released all tries and disabled hyphenation for this boot");
}

void HyphenationStore::preloadAll() {
  disabled_ = false;
  if (!ensureIndex()) return;

  for (uint32_t i = 0; i < entryCount_; ++i) {
    HyphEntry entry{};
    std::memcpy(&entry, prefix_ + HYPN_INDEX_OFFSET + i * HYPN_ENTRY_SIZE, sizeof(entry));

    bool already = false;
    for (size_t s = 0; s < slotCount_; ++s) {
      if (slots_[s].used && slots_[s].lang[0] == entry.lang[0] && slots_[s].lang[1] == entry.lang[1]) {
        already = true;
        break;
      }
    }
    if (already) continue;
    if (slotCount_ >= HYPN_MAX_ENTRIES) break;

    Slot& slot = slots_[slotCount_];
    bool outOfMemory = false;
    if (!loadBlob(entry, slot, outOfMemory)) {
      if (!outOfMemory) rememberFailure(entry.lang);
      continue;
    }
    slot.lang[0] = entry.lang[0];
    slot.lang[1] = entry.lang[1];
    slot.used = true;
    slot.patterns = SerializedHyphenationPatterns{entry.rootOffset, slot.buffer.get(), entry.blobSize};
    ++slotCount_;
  }

  LOG_INF(kTag, "preloaded %u/%u language(s) into PSRAM", (unsigned)slotCount_, (unsigned)entryCount_);
}

// Records "this language has no usable trie" so the answer is not recomputed per
// section. The slot is permanent: nothing frees a loaded blob, so there is no state
// where a latched failure could become stale within a session.
void HyphenationStore::rememberFailure(const char lang2[2]) {
  if (slotCount_ >= HYPN_MAX_ENTRIES) return;
  Slot& slot = slots_[slotCount_];
  slot.lang[0] = lang2[0];
  slot.lang[1] = lang2[1];
  slot.used = true;
  slot.buffer.reset();
  slot.patterns = SerializedHyphenationPatterns{};
  ++slotCount_;
}

// Reads and validates the header plus the language index, once per source. A card
// file that changed size or mtime since the last read invalidates the cache, so
// replacing the dictionary and then opening a book in a new language picks up the
// new file. Languages already installed keep their existing bytes; see the
// lifetime rules in the header for why that is safe rather than merely convenient.
bool HyphenationStore::ensureIndex() {
  // A partition cannot change without a reflash, so its index is final for the
  // session and is never re-read.
  if (source_ == Source::Partition) return true;

  if (source_ == Source::Card) {
    HalFile probe;
    if (Storage.exists(HYPHEN_DB_PATH) && Storage.openFileForRead(kTag, HYPHEN_DB_PATH, probe)) {
      const uint64_t size = probe.fileSize64();
      const uint32_t mtime = probe.modificationTime();
      if (size == cardSize_ && mtime == cardMtime_) return true;
      LOG_INF(kTag, "%s changed under us (%llu -> %llu bytes); re-reading the index", HYPHEN_DB_PATH,
              (unsigned long long)cardSize_, (unsigned long long)size);
    }
    invalidateIndex();
  }

  if (readIndexFromCard()) return true;
#if !defined(SIMULATOR)
  // Internal flash reads disable the cache.  A render task with a PSRAM stack
  // must not enter this path; preloadAll() runs on the main task instead.
  if (stackIsInPsram()) {
    LOG_ERR(kTag, "partition index read refused: PSRAM stack");
    return false;
  }
  if (!partitionProbed_) {
    partitionProbed_ = true;
    if (readIndexFromPartition()) return true;
  }
#endif
  invalidateIndex();
  return false;
}

void HyphenationStore::invalidateIndex() {
  source_ = Source::None;
  entryCount_ = 0;
  cardSize_ = 0;
  cardMtime_ = 0;
}

// Validates whatever is in prefix_. Shared by both sources so the card and the
// partition cannot drift apart in strictness.
static bool adoptPrefix(const uint8_t* prefix, uint32_t& entryCount, size_t available) {
  uint32_t count = 0;
  const HyphValidity validity = hyphValidateIndex(prefix, available, count);
  if (validity != HyphValidity::Ok) {
    LOG_ERR(kTag, "container rejected: %s", hyphValidityName(validity));
    return false;
  }
  entryCount = count;
  return true;
}

bool HyphenationStore::readIndexFromCard() {
  if (!Storage.exists(HYPHEN_DB_PATH)) return false;

  HalFile file;
  if (!Storage.openFileForRead(kTag, HYPHEN_DB_PATH, file)) {
    LOG_ERR(kTag, "cannot open %s", HYPHEN_DB_PATH);
    return false;
  }
  const uint64_t fileSize = file.fileSize64();
  if (fileSize < HYPN_HEADER_SIZE || fileSize > HYPN_MAX_FILE) {
    LOG_ERR(kTag, "%s has implausible size %llu", HYPHEN_DB_PATH, (unsigned long long)fileSize);
    return false;
  }

  // The header alone says how long the index is, so read it first and then exactly
  // the index: never the whole file, which is the point of the exercise.
  uint8_t header[HYPN_HEADER_SIZE];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    LOG_ERR(kTag, "short read on the header of %s", HYPHEN_DB_PATH);
    return false;
  }
  uint32_t ignored = 0;
  const HyphValidity early = hyphValidateIndex(header, sizeof(header), ignored);
  // TooSmall here means "the header is fine but the index is not in this buffer
  // yet", which is the expected case; anything else is a real rejection.
  if (early != HyphValidity::Ok && early != HyphValidity::TooSmall) {
    LOG_ERR(kTag, "%s rejected: %s", HYPHEN_DB_PATH, hyphValidityName(early));
    return false;
  }

  HyphHeader parsed;
  memcpy(&parsed, header, sizeof(parsed));
  const size_t indexBytes = static_cast<size_t>(parsed.entryCount) * HYPN_ENTRY_SIZE;
  if (HYPN_HEADER_SIZE + indexBytes > kPrefixBytes || HYPN_HEADER_SIZE + indexBytes > fileSize) {
    LOG_ERR(kTag, "%s claims %u languages, which does not fit", HYPHEN_DB_PATH, (unsigned)parsed.entryCount);
    return false;
  }

  memcpy(prefix_, header, sizeof(header));
  if (!file.seek(HYPN_INDEX_OFFSET) || file.read(prefix_ + HYPN_INDEX_OFFSET, indexBytes) != static_cast<int>(indexBytes)) {
    LOG_ERR(kTag, "short read on the index of %s", HYPHEN_DB_PATH);
    return false;
  }

  const size_t available = HYPN_HEADER_SIZE + indexBytes;
  // totalSize must match the file, which only the caller knows, so it is checked
  // here rather than inside the format helper.
  if (parsed.totalSize != fileSize) {
    LOG_ERR(kTag, "%s header says %u bytes but the file is %llu", HYPHEN_DB_PATH, (unsigned)parsed.totalSize,
            (unsigned long long)fileSize);
    return false;
  }
  if (!adoptPrefix(prefix_, entryCount_, available)) return false;

  source_ = Source::Card;
  cardSize_ = fileSize;
  cardMtime_ = file.modificationTime();
  return true;
}

#if !defined(SIMULATOR)

bool HyphenationStore::readIndexFromPartition() {
  if (stackIsInPsram()) return false;
  const esp_partition_t* partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (partition == nullptr) return false;
  if (static_cast<uint64_t>(HYPN_PARTITION_OFFSET) + HYPN_HEADER_SIZE > partition->size) return false;

  if (esp_partition_read(partition, HYPN_PARTITION_OFFSET, prefix_, HYPN_HEADER_SIZE) != ESP_OK) return false;

  uint32_t ignored = 0;
  const HyphValidity early = hyphValidateIndex(prefix_, HYPN_HEADER_SIZE, ignored);
  if (early != HyphValidity::Ok && early != HyphValidity::TooSmall) {
    // Logged at INF, not ERR: an unflashed or erased partition is the normal state
    // on a device that was written with the single-image command, and the card copy
    // may still be there. A partition that is present but corrupt is worth an ERR,
    // and BadMagic after a real flash is what that looks like -- but the two cannot
    // be told apart without reading twice, so both stay quiet and the summary line
    // in patterns() says which source won.
    LOG_INF(kTag, "no usable bundled container at +%06x: %s", (unsigned)HYPN_PARTITION_OFFSET,
            hyphValidityName(early));
    return false;
  }

  HyphHeader parsed;
  memcpy(&parsed, prefix_, sizeof(parsed));
  const size_t indexBytes = static_cast<size_t>(parsed.entryCount) * HYPN_ENTRY_SIZE;
  if (HYPN_HEADER_SIZE + indexBytes > kPrefixBytes) return false;
  if (static_cast<uint64_t>(HYPN_PARTITION_OFFSET) + parsed.totalSize > partition->size) {
    LOG_ERR(kTag, "bundled container claims %u bytes but only %u remain in the partition", (unsigned)parsed.totalSize,
            (unsigned)(partition->size - HYPN_PARTITION_OFFSET));
    return false;
  }
  if (esp_partition_read(partition, HYPN_PARTITION_OFFSET + HYPN_INDEX_OFFSET, prefix_ + HYPN_INDEX_OFFSET,
                         indexBytes) != ESP_OK) {
    LOG_ERR(kTag, "cannot read the bundled index");
    return false;
  }
  if (!adoptPrefix(prefix_, entryCount_, HYPN_HEADER_SIZE + indexBytes)) return false;

  source_ = Source::Partition;
  return true;
}

#endif  // !SIMULATOR

bool HyphenationStore::loadBlob(const HyphEntry& entry, Slot& slot, bool& outOfMemory) {
  outOfMemory = false;
  if (source_ == Source::Partition && stackIsInPsram()) {
    LOG_ERR(kTag, "partition blob read refused: PSRAM stack");
    return false;
  }
  if (!memory::psramHasHeadroom(entry.blobSize, entry.blobSize, kContiguousReserve)) {
    LOG_ERR(kTag, "no PSRAM headroom for a %u byte trie", (unsigned)entry.blobSize);
    outOfMemory = true;
    return false;
  }
  memory::ByteBuffer buffer = memory::makePsramByteBufferUninitializedNoThrow(entry.blobSize);
  if (!buffer) {
    LOG_ERR(kTag, "allocation of %u bytes failed", (unsigned)entry.blobSize);
    outOfMemory = true;
    return false;
  }

  if (source_ == Source::Card) {
    HalFile file;
    if (!Storage.openFileForRead(kTag, HYPHEN_DB_PATH, file)) {
      LOG_ERR(kTag, "cannot reopen %s for the blob", HYPHEN_DB_PATH);
      return false;
    }
    if (!file.seek(entry.blobOff)) {
      LOG_ERR(kTag, "cannot seek to %u in %s", (unsigned)entry.blobOff, HYPHEN_DB_PATH);
      return false;
    }
    size_t total = 0;
    while (total < entry.blobSize) {
      const size_t want = std::min(kChunkBytes, static_cast<size_t>(entry.blobSize) - total);
      const int got = file.read(buffer.get() + total, want);
      if (got <= 0) {
        LOG_ERR(kTag, "short read at %u of %u", (unsigned)total, (unsigned)entry.blobSize);
        return false;
      }
      total += static_cast<size_t>(got);
    }
#if !defined(SIMULATOR)
  } else {
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (partition == nullptr) return false;
    for (size_t at = 0; at < entry.blobSize; at += kChunkBytes) {
      const size_t want = std::min(kChunkBytes, static_cast<size_t>(entry.blobSize) - at);
      if (esp_partition_read(partition, HYPN_PARTITION_OFFSET + entry.blobOff + at, buffer.get() + at, want) !=
          ESP_OK) {
        LOG_ERR(kTag, "bundled read failed at %u of %u", (unsigned)at, (unsigned)entry.blobSize);
        return false;
      }
    }
#endif
  }

  const HyphValidity validity = hyphValidateBlob(entry, buffer.get(), entry.blobSize);
  if (validity != HyphValidity::Ok) {
    // The bytes are dropped rather than used: a trie that fails its CRC would still
    // walk without crashing (decodeState bounds-checks every node) and would place
    // hyphens in the wrong words, which is worse than no hyphenation at all.
    LOG_ERR(kTag, "trie rejected: %s", hyphValidityName(validity));
    return false;
  }

  slot.buffer = std::move(buffer);
  return true;
}

}  // namespace hyphenation

#endif  // !CROSSPOINT_EMBED_HYPHENATION
