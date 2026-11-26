#include "SystemCoreFont.h"

#include "SystemCoreFontCheck.h"

#if defined(CROSSPOINT_FONT_PACK_EMBEDDED) && defined(BOARD_HAS_PSRAM) && defined(ESP_PLATFORM)

#include <HalStorage.h>
#include <Logging.h>
#include <MinizConfig.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using system_core_font::classify;
using system_core_font::Facts;
using system_core_font::mayInstall;
using system_core_font::Status;
using system_core_font::tierLabel;
using system_core_font::Verdict;

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
  char tier[8];      // the generator's self-description; readTierLabel() interprets it
  uint8_t reserved[4];
};
static_assert(sizeof(CarrierHeader) == kHeaderSize, "core carrier header layout changed");
static_assert(offsetof(CarrierHeader, tier) == system_core_font::kTierFieldOffset,
              "the tier label moved away from the offset the generator and the tools use");

CarrierHeader g_header{};
bool g_probed = false;
bool g_present = false;
bool g_ready = false;
bool g_readyChecked = false;
Verdict g_verdict = Verdict::NotInstalled;
Status::Problem g_problem = Status::Problem::None;
system_core_font::TierLabel g_tier;

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
  g_problem = Status::Problem::None;
  const esp_partition_t* partition = spiffsPartition();
  if (partition == nullptr) {
    g_problem = Status::Problem::NoPartition;
    LOG_ERR("CORE", "no data/spiffs partition; this image cannot have a reading core");
    return false;
  }
  if (system_core_font::kPartitionOffset + kHeaderSize > partition->size) {
    g_problem = Status::Problem::BadCarrier;
    LOG_ERR("CORE", "core offset 0x%x is outside the %u byte data partition",
            static_cast<unsigned>(system_core_font::kPartitionOffset), static_cast<unsigned>(partition->size));
    return false;
  }
  CarrierHeader header{};
  if (esp_partition_read(partition, system_core_font::kPartitionOffset, &header, sizeof(header)) != ESP_OK) {
    g_problem = Status::Problem::BadCarrier;
    LOG_ERR("CORE", "cannot read the core header");
    return false;
  }
  const size_t blobSize = partition->size - system_core_font::kPartitionOffset;
  if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion ||
      header.headerSize != kHeaderSize || header.rawSize == 0 || header.rawSize > kMaxRawBytes ||
      header.compressedSize == 0 || header.compressedSize > blobSize - kHeaderSize) {
    g_problem = Status::Problem::BadCarrier;
    // The usual cause is exactly what it says: an app-only update over a partition
    // that never received the carrier. The reader silently drops to the UI subset,
    // so this has to be loud enough to find in a log.
    LOG_ERR("CORE", "core carrier missing or invalid (run a full install)");
    return false;
  }
  g_header = header;
  g_present = true;
  // A label, not a check: an untagged carrier (written by a build before this field was
  // used) is still the same font, and rejecting it over a text field would turn a working
  // reader font into tofu -- the failure this file exists to prevent.
  g_tier = system_core_font::readTierLabel(reinterpret_cast<const uint8_t*>(header.tier));
  LOG_INF("CORE", "carrier present: %u -> %u bytes, tier %s",
          static_cast<unsigned>(header.compressedSize), static_cast<unsigned>(header.rawSize),
          tierLabel());
  return true;
}

// Stream the installed face through CRC-32 without buffering it. Used only when the
// sidecar disagrees, i.e. to tell "half-installed by us" from "a different font".
bool fileCrc(uint32_t* out) {
  HalFile file;
  if (!Storage.openFileForRead("CORE", system_core_font::kTtfPath, file)) return false;
  uint32_t crc = 0;
  uint8_t buffer[4096];
  int got = 0;
  while ((got = file.read(buffer, sizeof(buffer))) > 0) {
    crc = crc32Update(crc, buffer, static_cast<size_t>(got));
  }
  file.close();
  if (got < 0) return false;
  *out = crc;
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
    // The Plus FAT rejects the remove+rename pair while the target exists (the
    // engine installer sees the same thing). Retry with an explicit remove, and
    // as a last resort overwrite in place: the caller re-validates size + CRC.
    Storage.remove(path);
    if (!Storage.rename(tmp, path)) {
      HalFile src;
      HalFile dst;
      if (!Storage.openFileForRead("CORE", tmp, src) || !Storage.openFileForWrite("CORE", path, dst)) {
        if (src) src.close();
        Storage.remove(tmp);
        return false;
      }
      uint8_t buffer[4096];
      bool copied = true;
      int got = 0;
      while ((got = src.read(buffer, sizeof(buffer))) > 0) {
        if (dst.write(buffer, static_cast<size_t>(got)) != got) {
          copied = false;
          break;
        }
      }
      src.close();
      dst.close();
      Storage.remove(tmp);
      if (!copied) return false;
    }
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
  g_verdict = Verdict::NotInstalled;
  if (!probe()) return false;

  // Gather what is knowable, then let one pure rule decide. The rule lives in
  // SystemCoreFontCheck.h because every branch of it has been wrong on a real
  // device: an unchecked marker write made a good face read as absent, and a size
  // comparison alone made the installer overwrite the user's wider downloaded core.
  Facts facts;
  uint32_t fileSize = 0;
  facts.storageReady = Storage.ready();
  facts.fileExists = facts.storageReady && Storage.exists(kTtfPath);
  if (facts.fileExists) {
    HalFile file;
    if (Storage.openFileForRead("CORE", kTtfPath, file)) {
      fileSize = static_cast<uint32_t>(file.fileSize());
      facts.sizeMatches = fileSize == g_header.rawSize;
      file.close();
    } else {
      facts.fileExists = false;
    }
  }
  if (facts.sizeMatches) {
    uint32_t marker = 0;
    facts.markerPresent = readSidecarCrc(&marker);
    facts.markerMatches = facts.markerPresent && marker == g_header.rawCrc;
    if (!facts.markerMatches) {
      uint32_t content = 0;
      facts.contentReadable = fileCrc(&content);
      facts.contentMatches = facts.contentReadable && content == g_header.rawCrc;
      if (facts.contentReadable && !facts.contentMatches) {
        LOG_ERR("CORE", "installed core is %u bytes with CRC 0x%08x, expected CRC 0x%08x: not this build's core",
                static_cast<unsigned>(g_header.rawSize), static_cast<unsigned>(content),
                static_cast<unsigned>(g_header.rawCrc));
      }
    }
  }
  g_verdict = classify(facts);

  switch (g_verdict) {
    case Verdict::Ready:
      g_problem = Status::Problem::None;
      g_ready = true;
      return true;
    case Verdict::RepairMarker: {
      // The bytes are the carrier's; only the record was lost. Rewrite it and keep
      // the font instead of declaring the reader fontless.
      char crcText[16];
      std::snprintf(crcText, sizeof(crcText), "%08x\n", static_cast<unsigned>(g_header.rawCrc));
      if (!writeFile(kCrcPath, crcText, std::strlen(crcText))) {
        LOG_ERR("CORE", "verified the core by content but could not rewrite its marker");
      }
      g_problem = Status::Problem::None;
      g_ready = true;
      LOG_INF("CORE", "repaired the core CRC marker from a content check");
      return true;
    }
    case Verdict::ForeignSize:
    case Verdict::ForeignContent:
      // Not our face: most likely the downloaded 8,105 pack on the same family name
      // and path. Leaving it in place is the safer half of the trade -- and it is only
      // half, because nothing else would use it: SdCardFontSystem gates the core family
      // on faceOnSd(), not ready(), so the wider face is still registered (15.19).
      g_problem = Status::Problem::ForeignCopy;
      if (g_verdict == Verdict::ForeignSize) {
        // ForeignContent already logged the observed versus expected CRC above.
        LOG_ERR("CORE", "%s holds a %u-byte face, this build's core is %u (tier %s); keeping the installed face",
                kTtfPath, static_cast<unsigned>(fileSize), static_cast<unsigned>(g_header.rawSize),
                tierLabel());
      }
      return false;
    case Verdict::NotInstalled:
      g_problem = facts.storageReady ? Status::Problem::NotInstalled : Status::Problem::SdUnavailable;
      return false;
    case Verdict::Unreadable:
      g_problem = Status::Problem::SidecarStale;
      LOG_ERR("CORE", "cannot read back the installed core to verify it");
      return false;
  }
  return false;
}

bool faceOnSd() {
  // ready() is the evaluation: it probes the card, repairs a lost marker and caches the
  // verdict. Its return value answers "is this our carrier", which is the wrong question for
  // registration -- a wider face left on the card by any other route must still be used.
  ready();
  return facePresent(g_verdict);
}

const char* tierLabel() {
  // Deliberately does not probe: probe()'s own log line calls this, and re-entering it would
  // recurse. Every caller that needs an answer here has already run the evaluation (ready(),
  // releaseToSd(), status()), and before that the honest answer is "unknown".
  return g_tier.known ? g_tier.text : "unknown";
}

// Inflate the carrier's deflate stream into `raw` (rawSize bytes) and verify its CRC.
//
// Shared by releaseToSd() and installResident() deliberately: two copies would drift, and a
// CRC check present on one path but not the other would let a corrupt carrier become the
// reading face.
bool inflateCarrier(uint8_t* raw, const size_t freePsram) {
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
    if (freePsram < g_header.rawSize + 256u * 1024u + g_header.compressedSize) {
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

  return ok;
}

bool releaseToSd() {
  if (ready()) return true;
  if (!probe()) return false;
  if (!mayInstall(g_verdict)) {
    // ready() logged why the face on the card is not ours; re-installing would
    // delete a font the user downloaded deliberately.
    return false;
  }
  if (!Storage.ready()) {
    g_problem = Status::Problem::SdUnavailable;
    LOG_ERR("CORE", "no SD card; the reader stays on the UI subset and offers the download pack");
    return false;
  }
  const size_t need = g_header.rawSize + 256u * 1024u;
  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (freePsram < need) {
    g_problem = Status::Problem::PsramShort;
    LOG_ERR("CORE", "not enough PSRAM to inflate the core (%u free, need %u)", static_cast<unsigned>(freePsram),
            static_cast<unsigned>(need));
    return false;
  }
  auto* raw = static_cast<uint8_t*>(heap_caps_malloc(g_header.rawSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (raw == nullptr) {
    LOG_ERR("CORE", "core PSRAM allocation failed (%u)", static_cast<unsigned>(g_header.rawSize));
    return false;
  }

  if (!inflateCarrier(raw, freePsram)) {
    LOG_ERR("CORE", "core carrier inflate failed");
    heap_caps_free(raw);
    return false;
  }

  const bool wrote = Storage.ensureDirectoryExists(kDir) && writeFile(kTtfPath, raw, g_header.rawSize);
  heap_caps_free(raw);
  if (!wrote) {
    g_problem = Status::Problem::SdUnavailable;
    LOG_ERR("CORE", "could not install the core on SD");
    return false;
  }

  // The marker is what ready() trusts, so a silently failed write used to leave a
  // perfectly good face marked unusable -- and the log above already claimed success.
  char crcText[16];
  std::snprintf(crcText, sizeof(crcText), "%08x\n", static_cast<unsigned>(g_header.rawCrc));
  if (!writeFile(kCrcPath, crcText, std::strlen(crcText))) {
    g_problem = Status::Problem::SidecarStale;
    LOG_ERR("CORE", "installed the core but could not write its CRC marker");
  }
  writeFile(kMarkerPath, "", 0);

  g_readyChecked = false;
  LOG_INF("CORE", "released the %u-byte core to %s", static_cast<unsigned>(g_header.rawSize), kTtfPath);
  return ready();
}

const char* problemName(Status::Problem problem) {
  switch (problem) {
    case Status::Problem::NoPartition: return "no-partition";
    case Status::Problem::BadCarrier: return "bad-carrier";
    case Status::Problem::PsramShort: return "psram-short";
    case Status::Problem::SdUnavailable: return "no-sd";
    case Status::Problem::NotInstalled: return "not-installed";
    case Status::Problem::SidecarStale: return "marker-stale";
    case Status::Problem::ForeignCopy: return "foreign-copy";
    case Status::Problem::None: break;
  }
  return "ok";
}

Status status() {
  Status out;
  out.carrierPresent = probe();
  out.compressedSize = g_header.compressedSize;
  out.rawSize = g_header.rawSize;
  out.rawCrc = g_header.rawCrc;
  out.sdFilePresent = Storage.ready() && Storage.exists(kTtfPath);
  if (out.sdFilePresent) {
    HalFile file;
    if (Storage.openFileForRead("CORE", kTtfPath, file)) {
      out.sdSize = static_cast<uint32_t>(file.fileSize());
      file.close();
    } else {
      out.sdFilePresent = false;
    }
  }
  out.ready = ready();
  out.faceInUse = faceOnSd();  // reuses the cached verdict; no second probe
  out.tier = tierLabel();      // probe() above is what filled it
  out.problem = out.ready ? Status::Problem::None : g_problem;
  return out;
}

// --- No-card fallback (see the header): keep the inflated core in PSRAM -----------------
//
// Never freed on purpose. A font family descriptor outlives whoever registered it, so
// releasing this buffer at the end of a call would leave the renderer reading freed PSRAM
// (the same trap the hyphenation store documents). It only ever exists on a device that has
// no card to put the font on, so it is not a steady-state cost on a normal install.
static uint8_t* g_resident = nullptr;
static uint32_t g_residentLen = 0;

bool installResident() {
  if (g_resident != nullptr) return true;
  // This path reads internal flash (mmap/esp_partition_read). A caller whose stack is in
  // PSRAM would have that cache disabled underneath it and panic -- the exact crash AGENTS.md
  // §43 records for the font flash read. Refuse loudly instead of silently doing nothing.
  if (esp_ptr_external_ram(esp_cpu_get_sp())) {
    LOG_ERR("CORE", "refused: called from a PSRAM-stack task; the core stays unavailable");
    return false;
  }
  if (!probe()) return false;
  // A card means the normal path; do not spend ~1 MB of PSRAM the SD family already provides.
  if (faceOnSd() || ready()) return false;
  const size_t need = g_header.rawSize + 128u * 1024u;
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  if (largest < need) {
    g_problem = Status::Problem::PsramShort;
    LOG_ERR("CORE", "no SD card and only %u KB largest PSRAM block; resident core needs %u KB",
            static_cast<unsigned>(largest / 1024), static_cast<unsigned>(need / 1024));
    return false;
  }
  auto* raw = static_cast<uint8_t*>(
      heap_caps_malloc(g_header.rawSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (raw == nullptr) {
    g_problem = Status::Problem::PsramShort;
    LOG_ERR("CORE", "resident core allocation failed (%u bytes)", static_cast<unsigned>(g_header.rawSize));
    return false;
  }
  if (!inflateCarrier(raw, heap_caps_get_free_size(MALLOC_CAP_SPIRAM))) {
    LOG_ERR("CORE", "resident core inflate failed");
    heap_caps_free(raw);
    return false;
  }
  g_resident = raw;
  g_residentLen = g_header.rawSize;
  LOG_INF("CORE", "resident core installed: %u KB tier %s (no SD card)",
          static_cast<unsigned>(g_residentLen / 1024), tierLabel());
  return true;
}

const uint8_t* residentData(uint32_t* len) {
  if (len != nullptr) *len = g_residentLen;
  return g_resident;
}

}  // namespace system_core_font

#else

namespace system_core_font {
bool carrierPresent() { return false; }
bool releaseToSd() { return false; }
bool ready() { return false; }
const char* problemName(Status::Problem) { return "unsupported"; }

// Both of these are called from code that is *not* gated on this feature:
// SdCardFontSystem::loadSupplementFonts() asks faceOnSd() on every path that builds the
// supplement list, and that function compiles wherever the header is included -- including
// the C3 spiffs profile, which sets CROSSPOINT_SYSTEM_FONT_PACK without
// CROSSPOINT_FONT_PACK_EMBEDDED and therefore lands in this branch. Omitting either symbol
// here is not a missing feature, it is an undefined reference at link.
// scripts/tests/test_system_core_font_api.py pins the declared set against both branches.

// No carrier was ever released in this config, so there is no core family on the card.
bool faceOnSd() { return false; }

// Nothing was decompressed, so there is no tier to report; "unknown" is the documented
// answer and this value is never branched on.
const char* tierLabel() { return "unknown"; }
Status status() {
  Status out;
  out.problem = Status::Problem::NoPartition;
  return out;
}
bool installResident() { return false; }
const uint8_t* residentData(uint32_t* len) {
  if (len != nullptr) *len = 0;
  return nullptr;
}

}  // namespace system_core_font

#endif
