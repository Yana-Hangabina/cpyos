#include "SystemCoreFont.h"

#if defined(CROSSPOINT_FONT_PACK_EMBEDDED) && defined(BOARD_HAS_PSRAM) && defined(ESP_PLATFORM)

#include <HalStorage.h>
#include <Logging.h>
#include <MinizConfig.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr char kMagic[4] = {'C', 'P', 'F', 'Z'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr size_t kMaxRawBytes = 4u * 1024u * 1024u;

struct CarrierHeader {
  char magic[4];
  uint16_t version;
  uint16_t headerSize;
  uint32_t rawSize;
  uint32_t rawCrc;
  uint32_t compressedSize;
  uint8_t reserved[12];
};
static_assert(sizeof(CarrierHeader) == kHeaderSize, "core carrier header layout changed");

CarrierHeader g_header{};
bool g_probed = false;
bool g_present = false;
bool g_ready = false;
bool g_readyChecked = false;

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  crc = ~crc;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

const esp_partition_t* spiffsPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

bool probe() {
  if (g_probed) return g_present;
  g_probed = true;
  g_present = false;
  const esp_partition_t* partition = spiffsPartition();
  if (partition == nullptr) {
    LOG_INF("CORE", "no data/spiffs partition");
    return false;
  }
  if (system_core_font::kPartitionOffset + kHeaderSize > partition->size) {
    LOG_INF("CORE", "core offset is outside the data partition");
    return false;
  }
  CarrierHeader header{};
  if (esp_partition_read(partition, system_core_font::kPartitionOffset, &header, sizeof(header)) != ESP_OK) {
    LOG_INF("CORE", "cannot read the core header");
    return false;
  }
  const size_t blobSize = partition->size - system_core_font::kPartitionOffset;
  if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion ||
      header.headerSize != kHeaderSize || header.rawSize == 0 || header.rawSize > kMaxRawBytes ||
      header.compressedSize == 0 || header.compressedSize > blobSize - kHeaderSize) {
    LOG_INF("CORE", "core carrier missing or invalid (run a full install)");
    return false;
  }
  g_header = header;
  g_present = true;
  LOG_INF("CORE", "carrier present: %u -> %u bytes", static_cast<unsigned>(header.compressedSize),
          static_cast<unsigned>(header.rawSize));
  return true;
}

bool readSidecarCrc(uint32_t* out) {
  HalFile file;
  if (!Storage.openFileForRead("CORE", system_core_font::kCrcPath, file)) return false;
  char buffer[16] = {};
  const int got = file.read(buffer, sizeof(buffer) - 1);
  file.close();
  if (got <= 0) return false;
  unsigned value = 0;
  if (std::sscanf(buffer, "%8x", &value) != 1) return false;
  *out = static_cast<uint32_t>(value);
  return true;
}

bool writeFile(const char* path, const void* data, size_t size) {
  char tmp[128];
  std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  HalFile file;
  if (!Storage.openFileForWrite("CORE", tmp, file)) return false;
  const bool wrote = file.write(data, size) == size;
  file.flush();
  file.close();
  if (!wrote) {
    Storage.remove(tmp);
    return false;
  }
  if (!Storage.replaceFile(tmp, path)) {
    Storage.remove(tmp);
    return false;
  }
  return true;
}

}  // namespace

namespace system_core_font {

bool carrierPresent() { return probe(); }

bool ready() {
  if (g_readyChecked) return g_ready;
  g_readyChecked = true;
  g_ready = false;
  if (!probe()) return false;
  if (!Storage.ready() || !Storage.exists(kTtfPath)) return false;
  HalFile file;
  if (!Storage.openFileForRead("CORE", kTtfPath, file)) return false;
  const size_t size = file.fileSize();
  file.close();
  if (size != g_header.rawSize) {
    LOG_INF("CORE", "installed core has %u bytes, expected %u", static_cast<unsigned>(size),
            static_cast<unsigned>(g_header.rawSize));
    return false;
  }
  uint32_t crc = 0;
  if (!readSidecarCrc(&crc) || crc != g_header.rawCrc) {
    LOG_INF("CORE", "installed core CRC marker is missing or stale");
    return false;
  }
  g_ready = true;
  return true;
}

bool releaseToSd() {
  if (ready()) return true;
  if (!probe()) return false;
  if (!Storage.ready()) {
    LOG_INF("CORE", "no SD card; the reader will use the UI subset and offer the download pack");
    return false;
  }
  const size_t need = g_header.rawSize + 256u * 1024u;
  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (freePsram < need) {
    LOG_ERR("CORE", "not enough PSRAM to inflate the core (%u free, need %u)", static_cast<unsigned>(freePsram),
            static_cast<unsigned>(need));
    return false;
  }
  auto* raw = static_cast<uint8_t*>(heap_caps_malloc(g_header.rawSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (raw == nullptr) {
    LOG_ERR("CORE", "core PSRAM allocation failed (%u)", static_cast<unsigned>(g_header.rawSize));
    return false;
  }

  const esp_partition_t* partition = spiffsPartition();
  const void* mapped = nullptr;
  esp_partition_mmap_handle_t handle = 0;
  uint8_t* compressed = nullptr;
  const uint8_t* source = nullptr;
  // The carrier starts with the 32-byte CPFZ header; the deflate stream begins
  // at +32. Map (and feed) from there. Mapping only compressedSize bytes from
  // the header both starts at the wrong byte and truncates the last 32 bytes.
  const size_t mappedSize = kHeaderSize + g_header.compressedSize;
  if (partition != nullptr &&
      esp_partition_mmap(partition, kPartitionOffset, mappedSize, ESP_PARTITION_MMAP_DATA, &mapped,
                         &handle) == ESP_OK) {
    source = static_cast<const uint8_t*>(mapped) + kHeaderSize;
  } else {
    // Fallback for a target that cannot map data flash: copy the compressed
    // carrier into PSRAM. The peak is compressed + raw, so this path is only
    // taken when the map is unavailable.
    if (freePsram < need + g_header.compressedSize) {
      LOG_ERR("CORE", "cannot map or buffer the compressed core");
      heap_caps_free(raw);
      return false;
    }
    compressed = static_cast<uint8_t*>(
        heap_caps_malloc(g_header.compressedSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (compressed == nullptr ||
        esp_partition_read(partition, kPartitionOffset + kHeaderSize, compressed, g_header.compressedSize) != ESP_OK) {
      LOG_ERR("CORE", "cannot read the compressed core");
      heap_caps_free(compressed);
      heap_caps_free(raw);
      return false;
    }
    source = compressed;
  }

  bool ok = tinfl_decompress_mem_to_mem(raw, g_header.rawSize, source, g_header.compressedSize, 0) ==
            static_cast<int>(g_header.rawSize);
  if (ok && crc32Update(0, raw, g_header.rawSize) != g_header.rawCrc) {
    LOG_ERR("CORE", "core carrier CRC mismatch");
    ok = false;
  }
  if (handle != 0) esp_partition_munmap(handle);
  heap_caps_free(compressed);

  if (!ok) {
    LOG_ERR("CORE", "core carrier inflate failed");
    heap_caps_free(raw);
    return false;
  }

  const bool wrote = Storage.ensureDirectoryExists(kDir) && writeFile(kTtfPath, raw, g_header.rawSize);
  heap_caps_free(raw);
  if (!wrote) {
    LOG_ERR("CORE", "could not install the core on SD");
    return false;
  }

  char crcText[16];
  std::snprintf(crcText, sizeof(crcText), "%08x\n", static_cast<unsigned>(g_header.rawCrc));
  writeFile(kCrcPath, crcText, std::strlen(crcText));
  writeFile(kMarkerPath, "", 0);

  g_readyChecked = false;
  LOG_INF("CORE", "released the %u-byte core to %s", static_cast<unsigned>(g_header.rawSize), kTtfPath);
  return ready();
}

}  // namespace system_core_font

#else

namespace system_core_font {
bool carrierPresent() { return false; }
bool releaseToSd() { return false; }
bool ready() { return false; }
}  // namespace system_core_font

#endif
