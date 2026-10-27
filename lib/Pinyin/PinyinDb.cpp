#include "PinyinDb.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>

#if !defined(SIMULATOR)
#include <esp_partition.h>
#endif

namespace pinyin {

namespace {

constexpr const char* kTag = "PINYIN";
// Sanity bounds on the resource, not expectations: a smaller file cannot be a
// database, and a larger one is not something this build ever wrote.
constexpr size_t kMinBytes = 4 * 1024;
constexpr size_t kMaxBytes = 2 * 1024 * 1024;
constexpr size_t kChunkBytes = 16 * 1024;
// Other PSRAM users (fonts, JJWXC containers) keep working while this loads.
constexpr size_t kContiguousReserve = 64 * 1024;

}  // namespace

PinyinDb& PinyinDb::shared() {
  static PinyinDb instance;
  return instance;
}

// The card copy wins when it is there: it is the one the user can replace
// without reflashing. The bundled copy (below) is what makes a freshly flashed
// device work with no card preparation at all.
bool PinyinDb::loadFromCard() {
  if (!Storage.exists(PINYIN_DB_PATH)) return false;

  HalFile file;
  if (!Storage.openFileForRead(kTag, PINYIN_DB_PATH, file)) {
    LOG_ERR(kTag, "cannot open %s", PINYIN_DB_PATH);
    return false;
  }
  const uint64_t fileSize = file.fileSize64();
  if (fileSize < kMinBytes || fileSize > kMaxBytes) {
    LOG_ERR(kTag, "%s has implausible size %llu", PINYIN_DB_PATH, static_cast<unsigned long long>(fileSize));
    return false;
  }
  const size_t bytes = static_cast<size_t>(fileSize);
  if (!memory::psramHasHeadroom(bytes, bytes, kContiguousReserve)) {
    LOG_ERR(kTag, "no PSRAM headroom for %u bytes", static_cast<unsigned>(bytes));
    return false;
  }

  memory::ByteBuffer buffer = memory::makePsramByteBufferUninitializedNoThrow(bytes);
  if (!buffer) {
    LOG_ERR(kTag, "allocation of %u bytes failed", static_cast<unsigned>(bytes));
    return false;
  }
  size_t total = 0;
  while (total < bytes) {
    const size_t want = std::min(kChunkBytes, bytes - total);
    const int got = file.read(buffer.get() + total, want);
    if (got <= 0) {
      LOG_ERR(kTag, "short read at %u of %u", static_cast<unsigned>(total), static_cast<unsigned>(bytes));
      return false;
    }
    total += static_cast<size_t>(got);
  }

  buffer_ = std::move(buffer);
  const PydbValidity validity = tables_.adopt(buffer_.get(), bytes);
  if (validity != PydbValidity::Ok) {
    LOG_ERR(kTag, "%s rejected: %s", PINYIN_DB_PATH, pydbValidityName(validity));
    release();
    return false;
  }
  LOG_INF(kTag, "loaded %s: %u bytes, %u syllables", PINYIN_DB_PATH, static_cast<unsigned>(bytes),
          static_cast<unsigned>(tables_.syllableCount()));
  return true;
}

#if !defined(SIMULATOR)

// The flasher writes the same database file into the data/spiffs partition
// (unused by CrossMux -- nothing mounts a filesystem there), so a device
// flashed with the two-image command has the feature before it ever sees a
// card. Read-only by design: a partition holding a real filesystem, or another
// board's payload, simply fails the header check and the caller keeps its old
// behaviour.
bool PinyinDb::loadBundled() {
  const esp_partition_t* partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (partition == nullptr) return false;
  uint8_t header[PYDB_HEADER_SIZE];
  if (esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK) return false;
  size_t bytes = 0;
  if (checkBundledPydbHeader(header, sizeof(header), partition->size, bytes) != PydbValidity::Ok) return false;
  if (bytes < kMinBytes || bytes > kMaxBytes) return false;
  if (!memory::psramHasHeadroom(bytes, bytes, kContiguousReserve)) {
    LOG_ERR(kTag, "no PSRAM headroom for the bundled %u bytes", static_cast<unsigned>(bytes));
    return false;
  }

  memory::ByteBuffer buffer = memory::makePsramByteBufferUninitializedNoThrow(bytes);
  if (!buffer) {
    LOG_ERR(kTag, "allocation of the bundled %u bytes failed", static_cast<unsigned>(bytes));
    return false;
  }
  for (size_t at = 0; at < bytes; at += kChunkBytes) {
    const size_t want = std::min(kChunkBytes, bytes - at);
    if (esp_partition_read(partition, at, buffer.get() + at, want) != ESP_OK) {
      LOG_ERR(kTag, "bundled read failed at %u of %u", static_cast<unsigned>(at), static_cast<unsigned>(bytes));
      return false;
    }
  }

  buffer_ = std::move(buffer);
  const PydbValidity validity = tables_.adopt(buffer_.get(), bytes);
  if (validity != PydbValidity::Ok) {
    LOG_ERR(kTag, "bundled database rejected: %s", pydbValidityName(validity));
    release();
    return false;
  }
  LOG_INF(kTag, "loaded bundled database: %u bytes, %u syllables", static_cast<unsigned>(bytes),
          static_cast<unsigned>(tables_.syllableCount()));
  return true;
}

#endif  // !SIMULATOR

bool PinyinDb::ensureLoaded() {
  if (tables_.valid()) return true;
  if (loadFromCard()) return true;
#if !defined(SIMULATOR)
  if (loadBundled()) return true;
#endif
  return false;
}

void PinyinDb::release() {
  tables_ = PinyinTables{};
  buffer_.reset();
}

}  // namespace pinyin
