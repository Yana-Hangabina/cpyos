#include "WebAssets.h"

#include "WebPackFormat.h"

// This macro is set only by [readpico_hardware]. It means "the pages live in the
// data partition", and reading that partition into PSRAM is the whole point --
// a build without PSRAM has nowhere to put the pack and no flash script that
// writes one. Fail at compile time rather than silently shipping a firmware
// whose web pages can never load.
#if defined(CROSSPOINT_WEB_ASSETS_FROM_PARTITION) && !defined(BOARD_HAS_PSRAM)
#error "CROSSPOINT_WEB_ASSETS_FROM_PARTITION needs PSRAM and a data partition; scope it to Read Pico"
#endif

namespace web_assets {
namespace {

// Shown when a page asset cannot be served. Deliberately plain and small: this
// is what a user sees when they open the file-transfer page after flashing only
// app0, and it has to explain the fix and offer the upload route in one screen.
// Both languages inline because the page cannot depend on the i18n tables.
//
// Embedded builds cannot reach it (their arrays are always present), and C3
// flash is at ~98%, so only the partition build pays for the full page.
#if defined(CROSSPOINT_WEB_ASSETS_FROM_PARTITION)
constexpr char kRecoveryPage[] =
    "<!doctype html><html lang=en><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Web assets missing</title>"
    "<style>body{font-family:system-ui,sans-serif;max-width:40em;margin:2em auto;padding:0 1em;"
    "line-height:1.45}code{background:#eee;padding:0 .2em}"
    "form{margin:1.5em 0;padding:1em;border:1px solid #ccc;border-radius:6px}</style>"
    "<h1>Web assets missing</h1>"
    "<p>These pages come from the data partition, and the pack there is missing, corrupt, or "
    "built by a different firmware. Status, upload and WebDAV still work.</p>"
    "<p>Reflash <code>web_pack.dat</code> at partition offset <code>0x56000</code> over USB, "
    "then reload. The upload form below only stores books and files on the SD card; it cannot "
    "repair this pack.</p>"
    "<hr><h2>资源包缺失</h2>"
    "<p>网页资源来自数据分区；当前资源包缺失、损坏或与固件版本不符。"
    "状态、上传与 WebDAV 仍可用。</p>"
    "<p>请通过 USB 重新刷写偏移 <code>0x56000</code> 处的 <code>web_pack.dat</code> 后刷新。"
    "下面的上传表单只能把书或文件存到 SD 卡，<b>无法修复资源包</b>。</p>"
    "<hr><form action=\"/upload?path=/\" method=\"post\" enctype=\"multipart/form-data\">"
    "<label>Upload a book or file to SD root / 上传书或文件到 SD 卡根目录<br>"
    "<input type=\"file\" name=\"file\"></label><br>"
    "<button type=\"submit\">Upload 上传</button></form>"
    "<p><a href=\"/\">Home</a> &middot; <a href=\"/api/status\">Status</a></p>";

#else
constexpr char kRecoveryPage[] = "web assets unavailable\n";
#endif

}  // namespace

const char* recoveryPage(size_t& size) {
  size = sizeof(kRecoveryPage) - 1;  // without the trailing NUL
  return kRecoveryPage;
}

#if defined(CROSSPOINT_WEB_ASSETS_FROM_PARTITION) && !defined(SIMULATOR)

// ---------------------------------------------------------------------------
// Pack-backed implementation (Read Pico)
// ---------------------------------------------------------------------------

}  // namespace web_assets

#include <Logging.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstring>

#include "WebPackSchema.generated.h"

namespace web_assets {
namespace {

constexpr const char* kTag = "WEB";
constexpr size_t kChunkBytes = 16 * 1024;

web_pack::Header gHeader{};
// The index lives in internal RAM: 16 * 20 bytes at worst, and it is read on
// every view() call.
web_pack::Entry gEntries[web_pack::WEB_PACK_MAX_ENTRIES];
bool gEntryUsable[web_pack::WEB_PACK_MAX_ENTRIES] = {};
uint32_t gEntryCount = 0;
uint8_t* gData = nullptr;  // whole pack, PSRAM
web_pack::Validity gStatus = web_pack::Validity::TooSmall;
bool gInitialized = false;

const esp_partition_t* dataPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

// Reading internal flash disables the caches, and a task whose stack is in PSRAM
// cannot run then (ESP-IDF asserts). begin() is expected on the loop task, which
// owns the web server; this turns a future caller's mistake into a log line
// instead of a panic. Same guard as I18nPack.cpp.
bool stackIsInPsram() { return esp_ptr_external_ram(esp_cpu_get_sp()); }

bool readPartition(const esp_partition_t* partition, uint32_t offset, uint8_t* dst, size_t size) {
  for (size_t at = 0; at < size; at += kChunkBytes) {
    const size_t want = std::min(kChunkBytes, size - at);
    if (esp_partition_read(partition, static_cast<size_t>(offset) + at, dst + at, want) != ESP_OK) {
      return false;
    }
  }
  return true;
}

void releaseBuffer() {
  if (gData != nullptr) {
    heap_caps_free(gData);
    gData = nullptr;
  }
}

}  // namespace

bool begin() {
  if (gInitialized) return gStatus == web_pack::Validity::Ok;
  if (stackIsInPsram()) {
    // Not latched: the web server may start on a PSRAM-stack task in a future
    // caller, and a later begin() from the loop task must be allowed to
    // succeed. Same rule as I18nPack::init(), which this guard mirrors.
    gStatus = web_pack::Validity::PsramStack;
    LOG_ERR(kTag, "refusing to read the web asset pack from a PSRAM-stack task");
    return false;
  }

  const esp_partition_t* partition = dataPartition();
  if (partition == nullptr) {
    gStatus = web_pack::Validity::NoPartition;
    gInitialized = true;
    LOG_ERR(kTag, "no data partition; web pages need the asset pack");
    return false;
  }
  if (static_cast<uint64_t>(web_pack::WEB_PACK_PARTITION_OFFSET) + web_pack::WEB_PACK_HEADER_SIZE >
      partition->size) {
    gStatus = web_pack::Validity::TotalSize;
    gInitialized = true;
    LOG_ERR(kTag, "pack offset +0x%06x is outside the data partition",
            static_cast<unsigned>(web_pack::WEB_PACK_PARTITION_OFFSET));
    return false;
  }

  uint8_t raw[web_pack::WEB_PACK_HEADER_SIZE];
  if (!readPartition(partition, web_pack::WEB_PACK_PARTITION_OFFSET, raw, sizeof(raw))) {
    gStatus = web_pack::Validity::PartitionRead;
    gInitialized = true;
    LOG_ERR(kTag, "cannot read the pack header at +0x%06x",
            static_cast<unsigned>(web_pack::WEB_PACK_PARTITION_OFFSET));
    return false;
  }

  const uint32_t remaining = static_cast<uint32_t>(partition->size - web_pack::WEB_PACK_PARTITION_OFFSET);
  web_pack::Header header;
  web_pack::Validity validity = web_pack::validateHeader(raw, sizeof(raw), remaining, header);
  if (validity != web_pack::Validity::Ok) {
    gStatus = validity;
    gInitialized = true;
    // An unflashed data partition (all 0xFF, or a --skip-data flash) reaches
    // here as TooSmall/BadMagic. It is a normal single-image install, but the
    // pages will not work, so say so once and clearly.
    LOG_ERR(kTag, "web asset pack unusable: %s; pages fall back to the recovery page",
            web_pack::validityName(validity));
    return false;
  }

  const size_t indexBytes = web_pack::expectedIndexBytes(header.entryCount);
  if (!readPartition(partition, web_pack::WEB_PACK_PARTITION_OFFSET + web_pack::WEB_PACK_HEADER_SIZE,
                     reinterpret_cast<uint8_t*>(gEntries), indexBytes)) {
    gStatus = web_pack::Validity::PartitionRead;
    gInitialized = true;
    LOG_ERR(kTag, "cannot read the %u-byte pack index", static_cast<unsigned>(indexBytes));
    return false;
  }

  validity = web_pack::validateIndex(header, reinterpret_cast<const uint8_t*>(gEntries), indexBytes);
  if (validity != web_pack::Validity::Ok) {
    gStatus = validity;
    gInitialized = true;
    LOG_ERR(kTag, "web asset pack unusable: %s; pages fall back to the recovery page",
            web_pack::validityName(validity));
    return false;
  }

  validity = web_pack::validateSchema(header, reinterpret_cast<const uint8_t*>(gEntries), indexBytes,
                                      web_pack::WEB_PACK_SCHEMA_SIGNATURE);
  if (validity != web_pack::Validity::Ok) {
    gStatus = validity;
    gInitialized = true;
    LOG_ERR(kTag, "web asset pack unusable: %s; reflash build/web_pack.dat", web_pack::validityName(validity));
    return false;
  }

  // Not latched on failure: PSRAM pressure is transient, and a later web-server
  // start may well succeed.
  uint8_t* data = static_cast<uint8_t*>(heap_caps_malloc(header.totalSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (data == nullptr) {
    gStatus = web_pack::Validity::OutOfMemory;
    LOG_ERR(kTag, "no PSRAM for the %u-byte web asset pack", static_cast<unsigned>(header.totalSize));
    return false;
  }
  if (!readPartition(partition, web_pack::WEB_PACK_PARTITION_OFFSET, data, header.totalSize)) {
    heap_caps_free(data);
    gStatus = web_pack::Validity::PartitionRead;
    gInitialized = true;
    LOG_ERR(kTag, "short read of the %u-byte web asset pack", static_cast<unsigned>(header.totalSize));
    return false;
  }

  // Per-asset CRC: one bad asset costs that asset only. The header and the index
  // were already checked, so a failure here cannot be a layout problem.
  uint32_t usable = 0;
  for (uint32_t i = 0; i < header.entryCount; ++i) {
    const web_pack::Entry& entry = gEntries[i];
    const size_t available = header.totalSize - entry.offset;
    const web_pack::Validity entryValidity =
        web_pack::validateEntry(entry, data + entry.offset, available);
    gEntryUsable[i] = entryValidity == web_pack::Validity::Ok;
    if (gEntryUsable[i]) {
      ++usable;
    } else {
      LOG_ERR(kTag, "asset %u rejected: %s (%u bytes at +0x%06x)", static_cast<unsigned>(entry.id),
              web_pack::validityName(entryValidity), static_cast<unsigned>(entry.size),
              static_cast<unsigned>(entry.offset));
    }
  }

  gHeader = header;
  gEntryCount = header.entryCount;
  gData = data;
  gStatus = web_pack::Validity::Ok;
  gInitialized = true;
  LOG_INF(kTag, "pack: %u assets (%u usable), %u bytes at partition +0x%06x", static_cast<unsigned>(gEntryCount),
          static_cast<unsigned>(usable), static_cast<unsigned>(header.totalSize),
          static_cast<unsigned>(web_pack::WEB_PACK_PARTITION_OFFSET));
  return true;
}

void end() {
  releaseBuffer();
  gInitialized = false;
  gStatus = web_pack::Validity::TooSmall;
  gEntryCount = 0;
  std::memset(gEntryUsable, 0, sizeof(gEntryUsable));
}

bool ready() { return gStatus == web_pack::Validity::Ok && gData != nullptr; }

const char* statusText() {
  return gStatus == web_pack::Validity::Ok ? nullptr : web_pack::validityName(gStatus);
}

View view(Asset asset, bool chinese) {
  if (!ready()) return View{};

  uint32_t id = 0;
  switch (asset) {
    case Asset::FilesPage: id = chinese ? web_pack::WEB_ASSET_FILES_PAGE_ZH : web_pack::WEB_ASSET_FILES_PAGE_EN; break;
    case Asset::SettingsPage:
      id = chinese ? web_pack::WEB_ASSET_SETTINGS_PAGE_ZH : web_pack::WEB_ASSET_SETTINGS_PAGE_EN;
      break;
    case Asset::JsZip: id = web_pack::WEB_ASSET_JSZIP; break;
    case Asset::PluginRunner: id = web_pack::WEB_ASSET_PLUGIN_RUNNER; break;
  }

  for (uint32_t i = 0; i < gEntryCount; ++i) {
    if (gEntries[i].id != id) continue;
    if (!gEntryUsable[i]) return View{};
    return View{reinterpret_cast<const char*>(gData + gEntries[i].offset), gEntries[i].size};
  }
  return View{};
}

#else  // embedded arrays

// ---------------------------------------------------------------------------
// Embedded implementation (C3, simulator, other S3 boards)
// ---------------------------------------------------------------------------

}  // namespace web_assets

// The generated arrays are declared `constexpr char NAME[] PROGMEM`. PROGMEM
// comes from Arduino.h on the ESP cores and from the simulator's shim; this is
// the first include in this translation unit that needs it, and it must be at
// global scope (the arrays are included inside namespaces below).
#include <Arduino.h>

namespace web_zh_cn {
#include "html/zh-CN/FilesPageHtml.generated.h"
#include "html/zh-CN/SettingsPageHtml.generated.h"
}  // namespace web_zh_cn
namespace web_en {
#include "html/en/FilesPageHtml.generated.h"
#include "html/en/SettingsPageHtml.generated.h"
}  // namespace web_en
#include "html/RunnerPageHtml.generated.h"
#include "html/js/jszip_minJs.generated.h"

namespace web_assets {

bool begin() { return true; }
void end() {}
bool ready() { return true; }
const char* statusText() { return nullptr; }

View view(Asset asset, bool chinese) {
  switch (asset) {
    case Asset::FilesPage:
      return chinese ? View{web_zh_cn::FilesPageHtml, sizeof(web_zh_cn::FilesPageHtml)}
                     : View{web_en::FilesPageHtml, sizeof(web_en::FilesPageHtml)};
    case Asset::SettingsPage:
      return chinese ? View{web_zh_cn::SettingsPageHtml, sizeof(web_zh_cn::SettingsPageHtml)}
                     : View{web_en::SettingsPageHtml, sizeof(web_en::SettingsPageHtml)};
    case Asset::JsZip:
      return View{jszip_minJs, sizeof(jszip_minJs)};
    case Asset::PluginRunner:
      return View{RunnerPageHtml, sizeof(RunnerPageHtml)};
  }
  return View{};
}

#endif  // CROSSPOINT_WEB_ASSETS_FROM_PARTITION && !SIMULATOR

}  // namespace web_assets
