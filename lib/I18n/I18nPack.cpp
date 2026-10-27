#include "I18nPack.h"

// The loader only exists in the configuration that needs it. On C3, the
// simulator, and every non-Read-Pico target this translation unit is empty, so
// those builds keep all language blobs in the image and reference neither
// heap_caps nor the partition API.
#if defined(CROSSPOINT_I18N_PACK)

// The pack is read into PSRAM and the loader calls heap_caps_*; a build that
// defines the macro without those would fail deep inside an unrelated header.
// Say what is wrong instead, the way WebAssets.cpp guards its own pack. This is
// checked before the SIMULATOR exclusion so a simulator build that selects the
// macro fails here rather than at link time with no pack loader at all.
#if !defined(BOARD_HAS_PSRAM)
#error "CROSSPOINT_I18N_PACK needs BOARD_HAS_PSRAM: the language pack is read into PSRAM"
#endif

#if !defined(SIMULATOR)
#if !defined(I18N_PACK_HOST_TEST)
#if defined(__has_include)
#if !__has_include(<esp_heap_caps.h>)
#error "CROSSPOINT_I18N_PACK needs esp_heap_caps.h: the language pack is read into PSRAM"
#endif
#else
#error "CROSSPOINT_I18N_PACK needs a compiler with __has_include to verify esp_heap_caps.h"
#endif
#endif

#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

#include "I18nOffsetBuilder.h"
#include "I18nStrings.h"

#if defined(I18N_PACK_HOST_TEST)
#include "I18nPackHost.h"
#else
#include <esp_cpu.h>
#include <esp_memory_utils.h>
#include <esp_partition.h>
#endif

using namespace i18n_strings;

namespace i18n_pack {

namespace {

constexpr const char* kTag = "I18N";
constexpr size_t kChunkBytes = 16 * 1024;
// Other PSRAM users (fonts, the JJWXC containers, the reader's page cache) keep
// working while a language loads. Same reserve PinyinDb uses.
constexpr size_t kContiguousReserve = 64 * 1024;

Header gHeader{};
Validity gStatus = Validity::TooSmall;
bool gInitialized = false;
uint8_t* gIndex = nullptr;  // PSRAM copy of the entry table
uint32_t gEntryCount = 0;
RuntimeLanguage gRuntime[static_cast<size_t>(Language::_COUNT)];
bool gFailed[static_cast<size_t>(Language::_COUNT)] = {};
uint32_t gLoaded = 0;

const esp_partition_t* dataPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

// Internal-flash reads disable the caches; a task whose stack is in PSRAM
// cannot run then, and ESP-IDF asserts. init()/loadLanguage() are expected on
// the main task (boot) or the UI task (language picker); this is the backstop
// that turns a caller mistake into a log line instead of a panic.
bool stackIsInPsram() {
#if defined(I18N_PACK_HOST_TEST)
  return i18nPackStackIsPsram();
#else
  return esp_ptr_external_ram(esp_cpu_get_sp());
#endif
}

bool readPartition(const esp_partition_t* partition, uint32_t offset, uint8_t* dst, size_t size) {
  for (size_t at = 0; at < size; at += kChunkBytes) {
    const size_t want = std::min(kChunkBytes, size - at);
    if (esp_partition_read(partition, static_cast<size_t>(offset) + at, dst + at, want) != ESP_OK) {
      return false;
    }
  }
  return true;
}

struct Pending {
  memory::ByteBuffer blob;
  uint16_t* offsets = nullptr;
  uint8_t* bitmap = nullptr;

  ~Pending() {
    if (bitmap != nullptr) heap_caps_free(bitmap);
    if (offsets != nullptr) heap_caps_free(offsets);
  }
};

}  // namespace

bool init() {
  if (gInitialized) return gStatus == Validity::Ok;
  if (stackIsInPsram()) {
    gStatus = Validity::PsramStack;
    LOG_ERR(kTag, "refusing to read the language pack from a PSRAM-stack task");
    return false;
  }

  const esp_partition_t* partition = dataPartition();
  if (partition == nullptr) {
    gStatus = Validity::NoPartition;
    gInitialized = true;
    LOG_INF(kTag, "no data partition; every packed language falls back to English");
    return false;
  }
  if (static_cast<uint64_t>(I18N_PACK_PARTITION_OFFSET) + I18N_PACK_HEADER_SIZE > partition->size) {
    gStatus = Validity::TotalSize;
    gInitialized = true;
    LOG_ERR(kTag, "pack offset +0x%06x is outside the data partition",
            static_cast<unsigned>(I18N_PACK_PARTITION_OFFSET));
    return false;
  }

  uint8_t raw[I18N_PACK_HEADER_SIZE];
  if (!readPartition(partition, I18N_PACK_PARTITION_OFFSET, raw, sizeof(raw))) {
    gStatus = Validity::PartitionRead;
    gInitialized = true;
    LOG_ERR(kTag, "cannot read the pack header at +0x%06x", static_cast<unsigned>(I18N_PACK_PARTITION_OFFSET));
    return false;
  }

  const uint32_t remaining = static_cast<uint32_t>(partition->size - I18N_PACK_PARTITION_OFFSET);
  Header header;
  const Validity headerValidity =
      validateHeader(raw, sizeof(raw), remaining, I18N_KEY_COUNT, I18N_KEY_SIGNATURE, I18N_CONTENT_SIGNATURE, header);
  if (headerValidity != Validity::Ok) {
    gStatus = headerValidity;
    gInitialized = true;
    // An unflashed data partition (all 0xFF, or --skip-data) is a normal state
    // for a single-image install; only a pack that exists but is wrong is an
    // error worth alarming about. Both are logged once, with the reason.
    const bool normalAbsence = headerValidity == Validity::BadMagic || headerValidity == Validity::TooSmall;
    if (normalAbsence) {
      LOG_INF(kTag, "language pack unusable: %s; packed languages fall back to English",
              validityName(headerValidity));
    } else {
      LOG_ERR(kTag, "language pack unusable: %s; packed languages fall back to English",
              validityName(headerValidity));
    }
    return false;
  }

  const size_t indexBytes = static_cast<size_t>(header.languageCount) * I18N_PACK_ENTRY_SIZE;
  auto* index = static_cast<uint8_t*>(heap_caps_malloc(indexBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (index == nullptr) {
    // Not latched: PSRAM pressure is transient, and init() may be retried on
    // the next language switch.
    gStatus = Validity::OutOfMemory;
    LOG_ERR(kTag, "no PSRAM for the %u-byte pack index", static_cast<unsigned>(indexBytes));
    return false;
  }
  if (!readPartition(partition, I18N_PACK_PARTITION_OFFSET + I18N_PACK_INDEX_OFFSET, index, indexBytes)) {
    heap_caps_free(index);
    gStatus = Validity::PartitionRead;
    gInitialized = true;
    LOG_ERR(kTag, "cannot read the pack index");
    return false;
  }

  const Validity indexValidity =
      validateIndex(header, index, indexBytes, static_cast<uint16_t>(Language::_COUNT));
  if (indexValidity != Validity::Ok) {
    heap_caps_free(index);
    gStatus = indexValidity;
    gInitialized = true;
    LOG_ERR(kTag, "language pack unusable: %s; packed languages fall back to English",
            validityName(indexValidity));
    return false;
  }

  gHeader = header;
  gIndex = index;
  gEntryCount = header.languageCount;
  gStatus = Validity::Ok;
  gInitialized = true;
  LOG_INF(kTag, "pack: %u languages, %u keys, %u bytes at partition +0x%06x", static_cast<unsigned>(gEntryCount),
          static_cast<unsigned>(gHeader.keyCount), static_cast<unsigned>(gHeader.totalSize),
          static_cast<unsigned>(I18N_PACK_PARTITION_OFFSET));
  return true;
}

bool ready() { return gStatus == Validity::Ok; }

Validity status() { return gStatus; }

const char* statusText() { return gStatus == Validity::Ok ? nullptr : validityName(gStatus); }

bool isPacked(Language lang) {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) return false;
  // English always lives in the image, and Chinese does only when this build
  // embedded it. Using the generated I18N_EMBEDDED_ZH instead of assuming
  // Chinese is present keeps a `custom_i18n_builtin_langs` build without ZH from
  // reporting Chinese as available while serving English strings.
  return packedByContract(lang == Language::EN, lang == Language::ZH_CN, I18N_EMBEDDED_ZH != 0);
}

bool hasLanguage(Language lang) {
  if (!isPacked(lang)) return false;
  if (!init()) return false;
  Entry entry;
  return findEntry(gIndex, gEntryCount, static_cast<uint16_t>(lang), entry);
}

bool loadLanguage(Language lang) {
  const auto slot = static_cast<size_t>(lang);
  if (slot >= static_cast<size_t>(Language::_COUNT)) return false;
  if (gRuntime[slot].offsets != nullptr) return true;
  if (gFailed[slot]) return false;
  if (!isPacked(lang)) return false;
  if (!init()) return false;
  if (stackIsInPsram()) {
    LOG_ERR(kTag, "refusing to load '%s' from a PSRAM-stack task", LANGUAGE_CODES[slot]);
    return false;
  }

  Entry entry;
  if (!findEntry(gIndex, gEntryCount, static_cast<uint16_t>(lang), entry)) {
    gFailed[slot] = true;
    LOG_INF(kTag, "'%s' is not in this language pack; English fallback", LANGUAGE_CODES[slot]);
    return false;
  }
  if ((entry.flags & I18N_PACK_FLAG_EMBEDDED) != 0) {
    gFailed[slot] = true;
    LOG_ERR(kTag, "'%s' is marked embedded but has no image table", LANGUAGE_CODES[slot]);
    return false;
  }

  const uint32_t keyCount = gHeader.keyCount;
  const size_t offsetBytes = static_cast<size_t>(keyCount) * sizeof(uint16_t);
  const size_t bitmapBytes = expectedBitmapBytes(keyCount);
  if (!memory::psramHasHeadroom(entry.blobSize + offsetBytes + bitmapBytes, entry.blobSize, kContiguousReserve)) {
    LOG_ERR(kTag, "no PSRAM headroom for '%s' (%u B blob + %u B offsets)", LANGUAGE_CODES[slot],
            static_cast<unsigned>(entry.blobSize), static_cast<unsigned>(offsetBytes));
    return false;
  }

  Pending pending;
  pending.blob = memory::makePsramByteBufferUninitializedNoThrow(entry.blobSize);
  if (entry.blobSize != 0 && !pending.blob) {
    LOG_ERR(kTag, "allocation of the %u-byte '%s' blob failed", static_cast<unsigned>(entry.blobSize),
            LANGUAGE_CODES[slot]);
    return false;
  }
  pending.offsets = static_cast<uint16_t*>(heap_caps_malloc(offsetBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  pending.bitmap = static_cast<uint8_t*>(heap_caps_malloc(bitmapBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (pending.offsets == nullptr || pending.bitmap == nullptr) {
    LOG_ERR(kTag, "allocation of '%s' tables failed", LANGUAGE_CODES[slot]);
    return false;
  }

  const esp_partition_t* partition = dataPartition();
  if (partition == nullptr) return false;  // init() would have caught this first
  if (!readPartition(partition, I18N_PACK_PARTITION_OFFSET + entry.blobOff, pending.blob.get(), entry.blobSize) ||
      !readPartition(partition, I18N_PACK_PARTITION_OFFSET + entry.bitmapOff, pending.bitmap, bitmapBytes)) {
    LOG_ERR(kTag, "short read while loading '%s'", LANGUAGE_CODES[slot]);
    return false;
  }

  Validity validity = validateBlob(entry, pending.blob.get(), entry.blobSize);
  if (validity == Validity::Ok) {
    validity = validateBitmap(entry, pending.bitmap, bitmapBytes);
  }
  if (validity != Validity::Ok) {
    // The bytes are dropped rather than used: offsets built from a bitmap that
    // fails its CRC would resolve every key to the wrong string, which is worse
    // than English. A permanent failure is latched so a corrupt pack is not
    // re-read on every switch.
    gFailed[slot] = true;
    LOG_ERR(kTag, "'%s' rejected: %s", LANGUAGE_CODES[slot], validityName(validity));
    return false;
  }

  if (!i18n_offsets::buildOffsets(pending.bitmap, keyCount, OFFSETS_EN,
                                  reinterpret_cast<const char*>(pending.blob.get()), entry.blobSize,
                                  pending.offsets)) {
    gFailed[slot] = true;
    LOG_ERR(kTag, "'%s' rejected: blob and key set disagree", LANGUAGE_CODES[slot]);
    return false;
  }

  // Success: the blob and its offsets live for the rest of the session, because
  // tr() hands out pointers into them that callers keep.
  gRuntime[slot].data = reinterpret_cast<const char*>(pending.blob.release());
  gRuntime[slot].offsets = pending.offsets;
  pending.offsets = nullptr;  // now owned by gRuntime
  ++gLoaded;
  LOG_INF(kTag, "'%s' loaded from the pack: %u B blob + %u B offsets", LANGUAGE_CODES[slot],
          static_cast<unsigned>(entry.blobSize), static_cast<unsigned>(offsetBytes));
  return true;
}

const RuntimeLanguage* runtime(Language lang) {
  const auto slot = static_cast<size_t>(lang);
  if (slot >= static_cast<size_t>(Language::_COUNT)) return nullptr;
  return gRuntime[slot].offsets == nullptr ? nullptr : &gRuntime[slot];
}

uint32_t loadedLanguageCount() { return gLoaded; }

}  // namespace i18n_pack

#endif  // !SIMULATOR
#endif  // CROSSPOINT_I18N_PACK
