#include "PdfAssets.h"

#if defined(CROSSPOINT_MUPDF)

#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <miniz.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "PdfAssetDownload.h"

namespace {
constexpr const char* kFontPath = "/.crosspoint/font_pack.dat";
constexpr const char* kCmapDir = "/.crosspoint/cmaps";
constexpr const char* kMarker = "/.crosspoint/pdf_assets_v2";
constexpr const char* kCmapProbe = "/.crosspoint/cmaps/Adobe-GB1-UCS2";
constexpr uint32_t kCarrierOffset = 0x1000;
constexpr size_t kCarrierHeaderSize = 20;
constexpr uint32_t kMaxFontRaw = 2u * 1024u * 1024u;
constexpr uint32_t kMaxCmapPack = 2u * 1024u * 1024u;
constexpr char kCarrierMagic[4] = {'P', 'F', 'P', 'K'};
constexpr char kCmapMagic[4] = {'C', 'M', 'P', 'S'};

bool writeFile(const std::string& path, const uint8_t* data, const size_t size) {
  HalFile out;
  if (!Storage.openFileForWrite("PDF", path.c_str(), out)) return false;
  const bool ok = out.write(data, size) == static_cast<int>(size);
  out.close();
  return ok;
}

bool inflateTo(const uint8_t* in, const size_t inSize, uint8_t* out, const size_t outSize) {
  return tinfl_decompress_mem_to_mem(out, outSize, in, inSize, 0) == outSize;
}

bool installFromApp1() {
  const esp_partition_t* app1 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (!app1) {
    LOG_ERR("PDF", "asset carrier: app1 partition missing");
    return false;
  }
  uint8_t header[kCarrierHeaderSize];
  if (esp_partition_read(app1, kCarrierOffset, header, sizeof(header)) != ESP_OK) {
    LOG_ERR("PDF", "asset carrier: header read failed");
    return false;
  }
  if (std::memcmp(header, kCarrierMagic, sizeof(kCarrierMagic)) != 0) {
    LOG_ERR("PDF", "asset carrier: magic mismatch (app1 was overwritten by OTA?)");
    return false;
  }
  const uint16_t version = static_cast<uint16_t>(header[4] | (header[5] << 8));
  uint32_t fontRaw = 0;
  uint32_t fontComp = 0;
  uint32_t cmapSize = 0;
  std::memcpy(&fontRaw, header + 8, 4);
  std::memcpy(&fontComp, header + 12, 4);
  std::memcpy(&cmapSize, header + 16, 4);
  if (version != 1 || fontRaw == 0 || fontRaw > kMaxFontRaw || fontComp == 0 || fontComp > kMaxFontRaw ||
      cmapSize == 0 || cmapSize > kMaxCmapPack) {
    LOG_ERR("PDF", "asset carrier: invalid header");
    return false;
  }

  const size_t payload = static_cast<size_t>(fontComp) + cmapSize;
  uint8_t* pack = static_cast<uint8_t*>(heap_caps_malloc(payload, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!pack) {
    LOG_ERR("PDF", "asset carrier: PSRAM allocation failed (%u)", static_cast<unsigned>(payload));
    return false;
  }
  if (esp_partition_read(app1, kCarrierOffset + kCarrierHeaderSize, pack, payload) != ESP_OK) {
    LOG_ERR("PDF", "asset carrier: payload read failed");
    heap_caps_free(pack);
    return false;
  }

  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(fontRaw, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw || !inflateTo(pack, fontComp, raw, fontRaw) || !writeFile(kFontPath, raw, fontRaw)) {
    LOG_ERR("PDF", "asset carrier: font unpack failed");
    if (raw) heap_caps_free(raw);
    heap_caps_free(pack);
    return false;
  }
  heap_caps_free(raw);
  LOG_INF("PDF", "installed full font pack (%u bytes)", static_cast<unsigned>(fontRaw));

  const uint8_t* cmap = pack + fontComp;
  if (std::memcmp(cmap, kCmapMagic, sizeof(kCmapMagic)) != 0) {
    LOG_ERR("PDF", "asset carrier: cmap magic mismatch");
    heap_caps_free(pack);
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(cmap[6] | (cmap[7] << 8));
  Storage.mkdir(kCmapDir, true);
  size_t pos = 8;
  bool ok = true;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 2 > cmapSize) {
      ok = false;
      break;
    }
    const uint16_t nameLen = static_cast<uint16_t>(cmap[pos] | (cmap[pos + 1] << 8));
    pos += 2;
    if (nameLen == 0 || pos + nameLen + 8 > cmapSize) {
      ok = false;
      break;
    }
    const std::string name(reinterpret_cast<const char*>(cmap + pos), nameLen);
    pos += nameLen;
    uint32_t rawSize = 0;
    uint32_t compSize = 0;
    std::memcpy(&rawSize, cmap + pos, 4);
    std::memcpy(&compSize, cmap + pos + 4, 4);
    pos += 8;
    if (rawSize == 0 || compSize == 0 || pos + compSize > cmapSize) {
      ok = false;
      break;
    }
    uint8_t* body = static_cast<uint8_t*>(heap_caps_malloc(rawSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!body || !inflateTo(cmap + pos, compSize, body, rawSize) ||
        !writeFile(std::string(kCmapDir) + "/" + name, body, rawSize)) {
      ok = false;
      if (body) heap_caps_free(body);
      break;
    }
    heap_caps_free(body);
    pos += compSize;
  }
  heap_caps_free(pack);
  if (!ok) {
    LOG_ERR("PDF", "asset carrier: cmap unpack failed");
    return false;
  }
  const uint8_t marker[2] = {'v', '2'};
  if (!writeFile(kMarker, marker, sizeof(marker))) {
    LOG_ERR("PDF", "asset carrier: marker write failed");
    return false;
  }
  LOG_INF("PDF", "installed %u CMaps to SD", static_cast<unsigned>(count));
  return true;
}
}  // namespace

bool ensurePdfAssets() {
  if (Storage.exists(kMarker) && Storage.exists(kFontPath) && Storage.exists(kCmapProbe)) return true;

  if (installFromApp1()) return true;

  // No repository URL yet; keep the download path compiled but disabled.
  if (tryDownloadPdfAssets()) return true;

  LOG_ERR("PDF", "PDF assets unavailable: app1 carrier missing/invalid and no download source");
  return false;
}

#endif
