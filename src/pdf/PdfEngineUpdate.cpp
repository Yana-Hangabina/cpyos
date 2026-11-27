#include "pdf/PdfEngineUpdate.h"
#include <FaultLog.h>

#include <StackSafety.h>
#include <esp_cpu.h>
#include <esp_memory_utils.h>

#include "Logging.h"

#if defined(CROSSPOINT_PDF_XIP)

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "BackgroundWork.h"
#include "HalPowerManager.h"
#include "HalStorage.h"
#include "Logging.h"
#include "NetworkStartup.h"
#include "network/DownloadRoutes.h"
#include "network/HttpDownloader.h"
#if defined(CPYOS_OTA_MANIFEST_URL)
#include "network/CrossMuxEndpoints.h"
#endif
#include <mbedtls/sha256.h>

namespace pdf_engine {
namespace {

std::string g_urlOverride;
std::string g_preferredMirror;

constexpr uint64_t kMaxManifestBytes = 4096;
constexpr int kManifestTimeoutMs = 20000;
constexpr int kBlobTimeoutMs = 60000;
constexpr const char* kStateDir = "/.crosspoint/pdf_engine";
constexpr const char* kTempFile = "/.crosspoint/pdf_engine/download.bin";
constexpr const char* kManifestCache = "/.crosspoint/pdf_engine/manifest.txt";

constexpr const char* kFallbackMirrors[] = {
    "https://gh-proxy.org/", "https://git.yylx.win/", "https://ghfast.top/",
    "https://gh-proxy.com/", "https://github.tbap.top/", "https://gh.dpik.top/", "https://ghproxy.net/",
};

const char* const* mirrorList(size_t& count) {
#if defined(CPYOS_OTA_MANIFEST_URL)
  count = CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRROR_COUNT;
  return CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRRORS;
#else
  count = sizeof(kFallbackMirrors) / sizeof(kFallbackMirrors[0]);
  return kFallbackMirrors;
#endif
}

bool ensureDir() { return Storage.ensureDirectoryExists(kStateDir); }

bool sha256File(const char* path, uint8_t out[32]) {
  HalFile file;
  if (!Storage.openFileForRead("PENG", path, file)) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buffer[1024];
  bool ok = true;
  while (true) {
    const int read = file.read(buffer, sizeof(buffer));
    if (read < 0) {
      ok = false;
      break;
    }
    if (read == 0) break;
    mbedtls_sha256_update(&ctx, buffer, static_cast<size_t>(read));
  }
  file.close();
  if (ok) mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
  return ok;
}

class TimeSyncPause {
 public:
  TimeSyncPause() { NetworkStartup::suspendTimeSync(); }
  ~TimeSyncPause() { NetworkStartup::resumeTimeSync(); }
  TimeSyncPause(const TimeSyncPause&) = delete;
  TimeSyncPause& operator=(const TimeSyncPause&) = delete;
};

bool readCache(std::string& out) {
  out.clear();
  HalFile file;
  if (!Storage.openFileForRead("PENG", kManifestCache, file)) return false;
  char buf[256];
  int n;
  while ((n = file.read(buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
  file.close();
  return !out.empty();
}

void writeCache(const std::string& text) {
  if (!ensureDir()) return;
  const std::string tmp = std::string(kManifestCache) + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("PENG", tmp.c_str(), file)) return;
  file.write(text.data(), text.size());
  file.close();
  Storage.replaceFile(tmp.c_str(), kManifestCache);
}

}  // namespace

const char* manifestUrl() {
  if (!g_urlOverride.empty()) return g_urlOverride.c_str();
#ifdef CPYOS_PDF_ENGINE_MANIFEST_URL
  return CPYOS_PDF_ENGINE_MANIFEST_URL;
#else
  return "";
#endif
}

void setManifestUrlOverride(const char* url) { g_urlOverride = (url != nullptr) ? url : ""; }
bool isConfigured() { const char* u = manifestUrl(); return u != nullptr && *u != '\0'; }
const char* tempPath() { return kTempFile; }

namespace {
// Written by whichever thread runs the phases, read by the hosting UI task.
// Byte counts are advisory (a torn read at worst shows a stale figure), so a
// plain relaxed store/load pair is enough -- no lock on the display path.
std::atomic<uint8_t> g_stage{static_cast<uint8_t>(Stage::Idle)};
std::atomic<size_t> g_downloaded{0};
std::atomic<size_t> g_total{0};
}  // namespace

// The manifest is a rolling index: one line per published engine.  A device
// only ever considers entries for its own compatibility class and picks the
// highest engineRev, so an older firmware keeps getting the newest engine that
// was ever built for that contract.  Nothing here reads the 0xb0 ELF digest:
// compatibility is a compile-time property (PdfModuleApi.h), not a build id.
Failure fetchManifest(Entry& entry, std::string& detail, const bool* cancelFlag) {
  detail.clear();
  g_stage.store(static_cast<uint8_t>(Stage::Manifest), std::memory_order_relaxed);
  if (!isConfigured()) {
    detail = "engine update URL is not configured";
    return Failure::NotConfigured;
  }
  if (WiFi.status() != WL_CONNECTED) {
    detail = "no Wi-Fi";
    return Failure::NoWifi;
  }
  if (!ensureDir()) return Failure::SdCard;

  size_t mirrorCount = 0;
  const char* const* mirrors = mirrorList(mirrorCount);
  std::vector<download_routes::Candidate> candidates;
  if (!g_urlOverride.empty()) {
    candidates.push_back({"override", manifestUrl(), kManifestTimeoutMs});
  } else {
    candidates = download_routes::buildManifestShortlist(manifestUrl(), "", mirrors, mirrorCount, kManifestTimeoutMs, 4);
  }

  std::string text;
  bool got = false;
  {
    TimeSyncPause pause;  // never start DNS with a pending SNTP callback (lwIP assert)
    for (const auto& c : candidates) {
      if (cancelFlag != nullptr && *cancelFlag) return Failure::Cancelled;
      text.clear();
      HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
      const bool ok = HttpDownloader::fetchUrl(
          c.url,
          [&text, cancelFlag](const uint8_t* data, const size_t len) {
            if (cancelFlag != nullptr && *cancelFlag) return false;
            if (text.size() + len > kMaxManifestBytes) return false;
            text.append(reinterpret_cast<const char*>(data), len);
            return true;
          },
          "", "", {}, kManifestTimeoutMs, cancelFlag, &result);
      // One line per candidate: without it a failed shortlist looks like one
      // silent 35 s wait, and there is no way to tell "every mirror failed"
      // from "only the first was tried".
      LOG_INF("PDFENG", "manifest candidate %u/%u (%s) -> %s, %u bytes, err=%d",
              static_cast<unsigned>(&c - candidates.data() + 1), static_cast<unsigned>(candidates.size()),
              c.url.c_str(), ok ? "ok" : "fail", static_cast<unsigned>(text.size()),
              static_cast<int>(result));
      if (ok && !text.empty()) {
        g_preferredMirror = download_routes::mirrorPrefixForUrl(c.url, mirrors, mirrorCount);
        got = true;
        break;
      }
      if (result == HttpDownloader::NO_WIFI) return Failure::NoWifi;
    }
  }

  if (!got) {
    // Never answer a failed fetch from the cache: the cache holds the previous
    // index, and serving it as "no compatible engine" turns a network failure
    // into a misleading verdict (device log 2026-11-20).
    detail = "manifest fetch failed";
    return Failure::Network;
  }
  {
    writeCache(text);
  }

  std::array<Entry, kMaxEntries> entries{};
  size_t count = 0;
  if (!parseManifest(text, entries, count)) {
    detail = "manifest is not a PDFENG2 index";
    return Failure::Manifest;
  }
  if (!selectForApp(entries, count, entry)) {
    char buf[128];
    snprintf(buf, sizeof(buf), "no engine for abi=%u hostApi=%u.%u layout=0x%08x", static_cast<unsigned>(kPdfModuleAbiVersion),
             static_cast<unsigned>(kPdfModuleHostApiMajor), static_cast<unsigned>(kPdfModuleHostApiMinor),
             static_cast<unsigned>(kPdfModuleLayoutKey));
    detail = buf;
    LOG_ERR("PDFENG", "manifest has %u entries but none match: %s", static_cast<unsigned>(count), buf);
    return Failure::WrongImage;
  }
  LOG_INF("PDFENG", "manifest: %u entries, selected rev=%u build=%02x%02x..", static_cast<unsigned>(count),
          static_cast<unsigned>(entry.engineRev), entry.buildId[0], entry.buildId[1]);
  return Failure::None;
}

Failure downloadToTemp(const Entry& entry, ProgressCallback progress, const bool* cancelFlag) {
  g_stage.store(static_cast<uint8_t>(Stage::Body), std::memory_order_relaxed);
  g_total.store(entry.size, std::memory_order_relaxed);
  g_downloaded.store(0, std::memory_order_relaxed);
  if (!ensureDir()) return Failure::SdCard;
  // Wrap so the card advances even when the caller passed no callback.
  ProgressCallback tracked = [progress](const size_t got, const size_t total) {
    g_downloaded.store(got, std::memory_order_relaxed);
    if (total != 0) g_total.store(total, std::memory_order_relaxed);
    if (progress) progress(got, total);
  };
  size_t mirrorCount = 0;
  const char* const* mirrors = mirrorList(mirrorCount);
  const auto candidates =
      download_routes::buildGithubShortlist(entry.url, g_preferredMirror, mirrors, mirrorCount, 3, kBlobTimeoutMs);
  bool downloaded = false;
  {
    TimeSyncPause pause;
    for (const auto& c : candidates) {
      if (cancelFlag != nullptr && *cancelFlag) return Failure::Cancelled;
      Storage.remove(kTempFile);
      g_downloaded.store(0, std::memory_order_relaxed);
      const auto r = HttpDownloader::downloadToFile(c.url, kTempFile, tracked, cancelFlag, "", "", {}, false,
                                                    kBlobTimeoutMs);
      if (r == HttpDownloader::OK) {
        g_preferredMirror = download_routes::mirrorPrefixForUrl(c.url, mirrors, mirrorCount);
        downloaded = true;
        break;
      }
      LOG_INF("PDFENG", "blob candidate %u/%u (%s) failed, err=%d",
              static_cast<unsigned>(&c - candidates.data() + 1), static_cast<unsigned>(candidates.size()),
              c.url.c_str(), static_cast<int>(r));
      if (r == HttpDownloader::NO_WIFI) return Failure::NoWifi;
      if (cancelFlag != nullptr && *cancelFlag) return Failure::Cancelled;
    }
  }
  if (!downloaded) return Failure::Download;
  g_stage.store(static_cast<uint8_t>(Stage::Verify), std::memory_order_relaxed);

  HalFile file;
  if (!Storage.openFileForRead("PENG", kTempFile, file)) return Failure::Verify;
  const uint64_t size = file.fileSize();
  file.close();
  if (size != entry.size) {
    Storage.remove(kTempFile);
    return Failure::Verify;
  }
  uint8_t actual[32] = {};
  if (!sha256File(kTempFile, actual) || std::memcmp(actual, entry.sha256.data(), 32) != 0) {
    Storage.remove(kTempFile);
    return Failure::Verify;
  }
  return Failure::None;
}

// ---- module-owned worker (network + SD only; install stays on the caller) --
namespace {
TaskHandle_t g_task = nullptr;
bool g_taskInPsram = false;
// A plain bool, not an atomic: HttpDownloader's abort callback takes a
// const bool* and polls it mid-handshake, so cancelling has to reach the
// in-flight request rather than only the gap between attempts (same shape as
// FontExtrasActivity::cancelRequested_).
bool g_cancel = false;
std::atomic<bool> g_done{false};
State g_state = State::Idle;
Result g_result;
constexpr uint32_t kWorkerStackBytes = 16 * 1024;

void workerTrampoline(void*);

void runWorker() {
  vTaskDelay(pdMS_TO_TICKS(1000));  // let DHCP/ARP settle; hold 240 MHz for TLS
  HalPowerManager::Lock powerLock(/*quiet=*/true);
  Entry entry;
  std::string detail;
  Failure f = fetchManifest(entry, detail, &g_cancel);
  if (f == Failure::None) f = downloadToTemp(entry, nullptr, &g_cancel);
  g_result.failure = f;
  g_result.detail = std::move(detail);
  g_result.entry = entry;
  g_state = (f == Failure::None) ? State::Done : State::Failed;
}

void workerTrampoline(void*) {
  {
    background_work::Guard guard;
    runWorker();
  }
  g_done.store(true, std::memory_order_release);
  vTaskSuspend(nullptr);  // reaped by pollUpdate()/cancelUpdate()
}

void reapTask() {
  if (g_task == nullptr) return;
  if (g_taskInPsram) {
    vTaskDeleteWithCaps(g_task);
  } else {
    vTaskDelete(g_task);
  }
  g_task = nullptr;
  g_taskInPsram = false;
}
}  // namespace

bool startUpdate() {
  if (g_task != nullptr) return false;
  // The worker runs on a PSRAM stack; nothing below it may touch internal flash.
  // Compatibility is compile-time, so there is no digest to read here.
  if (!memory::internalFlashAllowed(fault::Code::PdfEngineStartRefused)) {
    LOG_ERR("PDFENG", "startUpdate refused on a PSRAM stack");
    return false;
  }
  g_cancel = false;
  g_stage.store(static_cast<uint8_t>(Stage::Manifest), std::memory_order_relaxed);
  g_downloaded.store(0, std::memory_order_relaxed);
  g_total.store(0, std::memory_order_relaxed);
  g_done.store(false, std::memory_order_relaxed);
  g_result = Result{};
  g_state = State::Running;
  BaseType_t ok = pdFAIL;
#if defined(BOARD_HAS_PSRAM)
  ok = xTaskCreatePinnedToCoreWithCaps(workerTrampoline, "pdfEngUpd", kWorkerStackBytes / sizeof(StackType_t), nullptr,
                                       tskIDLE_PRIORITY, &g_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_taskInPsram = ok == pdPASS;
#endif
  if (ok != pdPASS) {
    fault::raise(fault::Code::WorkerStackFallback, static_cast<uint32_t>(ok));
    g_taskInPsram = false;
    ok = xTaskCreatePinnedToCore(workerTrampoline, "pdfEngUpd", kWorkerStackBytes / sizeof(StackType_t), nullptr,
                                 tskIDLE_PRIORITY, &g_task, 0);
  }
  if (ok != pdPASS) {
    fault::raise(fault::Code::WorkerCreateFailed, static_cast<uint32_t>(ok));
    g_task = nullptr;
    g_state = State::Idle;
    return false;
  }
  return true;
}

bool updateInProgress() { return g_task != nullptr && !g_done.load(std::memory_order_acquire); }

void cancelUpdate() { g_cancel = true; }

State pollUpdate() {
  if (g_task != nullptr && g_done.load(std::memory_order_acquire)) reapTask();
  return g_state;
}

const Result& result() { return g_result; }

Progress progress() {
  Progress p;
  p.stage = static_cast<Stage>(g_stage.load(std::memory_order_relaxed));
  p.downloaded = g_downloaded.load(std::memory_order_relaxed);
  p.total = g_total.load(std::memory_order_relaxed);
  return p;
}

pdf_module::InstallStatus installFromTemp() {
  const auto status = pdf_module::installFromSdFile(kTempFile);
  if (status == pdf_module::InstallStatus::Ok) {
    // A 1.9 MB download has no reason to linger on the card, and a stale image
    // is exactly the file someone later re-installs by accident.
    Storage.remove(kTempFile);
  }
  return status;
}

}  // namespace pdf_engine

#endif
