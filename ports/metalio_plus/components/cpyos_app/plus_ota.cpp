#include "plus_ota.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <esp_app_desc.h>
#include <esp_mac.h>

#include <cstdio>
#include <cstring>

#include "network/FirmwareFlasher.h"
#include "network/HttpDownloader.h"

#ifndef PLUS_OTA_KEY
#define PLUS_OTA_KEY ""
#endif

namespace plus_ota {
namespace {

constexpr const char* kCheckUrl = "https://admin.metalio.world/xiaozhi/api/firmware-ota/check";
constexpr const char* kBoard = "Metalio E-Ink4 Plus";
constexpr const char* kChip = "esp32s31";
constexpr const char* kApp = "脆皮鸭CPYOS";

std::string g_deviceId;

const char* deviceIdCached() {
  if (g_deviceId.empty()) {
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
      char buf[18];
      std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                    mac[5]);
      g_deviceId = buf;
    } else {
      g_deviceId = "00:00:00:00:00:00";
    }
  }
  return g_deviceId.c_str();
}

// Vendor error strings are stable per the OTA docs; map the ones a user can act
// on. Anything else is shown verbatim (never the key).
std::string describeHttp(int http, const std::string& body) {
  switch (http) {
    case 200:
      return {};
    case 400:
      return "request rejected (400): " + body;
    case 401:
      return "OTA key missing or rejected (401)";
    case 404:
      return "firmware not found for this board/name (404): " + body;
    case 409:
      return "OTA disabled or download unavailable (409): " + body;
    default:
      return (http > 0 ? ("HTTP " + std::to_string(http) + ": ") : std::string("no response: ")) + body;
  }
}

}  // namespace

namespace {
// The key may also live on the card (/.crosspoint/ota_key), so a key obtained
// later does not require a firmware rebuild. Read once, trimmed, never logged.
std::string g_sdKey;
bool g_sdKeyLoaded = false;

const char* activeKey() {
  if (std::strlen(PLUS_OTA_KEY) > 0) return PLUS_OTA_KEY;
  if (!g_sdKeyLoaded) {
    g_sdKeyLoaded = true;
    if (Storage.exists("/.crosspoint/ota_key")) {
      HalFile f = Storage.open("/.crosspoint/ota_key", O_RDONLY);
      if (f) {
        char buf[160] = {0};
        const int n = f.read(buf, sizeof(buf) - 1);
        f.close();
        if (n > 0) {
          std::string key(buf, static_cast<size_t>(n));
          while (!key.empty() && (key.back() == '\n' || key.back() == '\r' || key.back() == ' ')) key.pop_back();
          if (key.rfind("fw_ota_", 0) == 0) {
            g_sdKey = std::move(key);
            LOG_INF("OTA", "using the key from /.crosspoint/ota_key (%u chars)", static_cast<unsigned>(g_sdKey.size()));
          } else {
            LOG_ERR("OTA", "ignoring /.crosspoint/ota_key: not an fw_ota_ key");
          }
        }
      }
    }
  }
  return g_sdKey.c_str();
}
}  // namespace

bool configured() { return std::strlen(activeKey()) > 0; }
const char* currentVersion() {
  const esp_app_desc_t* app = esp_app_get_description();
  return app ? app->version : "?";
}
const char* deviceId() { return deviceIdCached(); }
const char* boardName() { return kBoard; }
const char* chipName() { return kChip; }
const char* appName() { return kApp; }

Info check(const Channel channel) {
  Info info;
  if (!configured()) {
    info.error = "this build has no OTA key (plus_ota_key.local)";
    return info;
  }
  JsonDocument doc;
  doc["chip_model_name"] = kChip;
  doc["board"]["type"] = kBoard;
  doc["application"]["name"] = kApp;
  std::string payload;
  serializeJson(doc, payload);

  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(20000);
  if (!http.begin(kCheckUrl)) {
    info.error = "cannot open the OTA endpoint";
    return info;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-OTA-Key", activeKey());
  http.addHeader("Device-Id", deviceIdCached());
  if (channel == Channel::Test) http.addHeader("X-OTA-Channel", "test");
  const int code = http.POST(payload);
  const std::string body = http.getString();
  http.end();
  info.http = code;
  LOG_INF("OTA", "check: http=%d body=%u B channel=%s", code, static_cast<unsigned>(body.size()),
          channel == Channel::Test ? "test" : "stable");
  if (code != 200) {
    info.error = describeHttp(code, body);
    return info;
  }

  JsonDocument resp;
  const DeserializationError err = deserializeJson(resp, body);
  if (err) {
    info.error = std::string("bad JSON from the OTA service: ") + err.c_str();
    return info;
  }
  info.version = resp["firmware"]["version"] | "";
  info.url = resp["firmware"]["url"] | "";
  info.firmwareName = resp["firmware"]["firmware_name"] | "";
  info.size = resp["firmware"]["size"] | 0;
  const bool has = resp["has_update"] | false;
  info.ok = true;
  info.hasUpdate = has && !info.version.empty() && info.version != currentVersion();
  if (!info.hasUpdate) {
    LOG_INF("OTA", "no update: remote=%s local=%s has_update=%d", info.version.c_str(), currentVersion(),
            has ? 1 : 0);
  } else {
    LOG_INF("OTA", "update available: %s -> %s (%u B)", currentVersion(), info.version.c_str(),
            static_cast<unsigned>(info.size));
  }
  if (info.hasUpdate && info.url.empty()) {
    info.hasUpdate = false;
    info.error = "the service reported an update without a download URL";
  }
  return info;
}

bool download(const Info& info, const char* destPath, void (*progress)(size_t, size_t, void*), void* ctx,
              const bool* cancelFlag, std::string& error) {
  if (!info.ok || info.url.empty()) {
    error = "no download URL";
    return false;
  }
  const std::string dir = "/.crosspoint/ota";
  Storage.ensureDirectoryExists(dir.c_str());
  // A previous interrupted run (or a crash mid-write) can leave a file whose
  // FAT chain no longer truncates; remove it before opening for write.
  Storage.remove(destPath);
  HttpDownloader::ProgressCallback cb;
  if (progress) {
    cb = [progress, ctx](size_t done, size_t total) { progress(done, total, ctx); };
  }
  const auto derr = HttpDownloader::downloadToFile(info.url, destPath, cb, cancelFlag, "", "", {}, false, 30000);
  if (derr != HttpDownloader::OK) {
    error = derr == HttpDownloader::NO_WIFI      ? "Wi-Fi is not connected"
            : derr == HttpDownloader::FILE_ERROR ? "cannot write the card"
            : derr == HttpDownloader::ABORTED    ? "cancelled"
                                                 : "download failed";
    return false;
  }
  HalFile f = Storage.open(destPath, O_RDONLY);
  if (!f) {
    error = "downloaded file cannot be reopened";
    return false;
  }
  const long got = static_cast<long>(f.fileSize());
  f.close();
  if (info.size && static_cast<size_t>(got) != info.size) {
    error = "size mismatch: got " + std::to_string(got) + " of " + std::to_string(info.size);
    return false;
  }
  LOG_INF("OTA", "downloaded %ld B to %s", got, destPath);
  return true;
}

bool install(const char* path, std::string& error) {
  const auto result = firmware_flash::flashFromSdPath(path, nullptr, nullptr);
  if (result != firmware_flash::Result::OK) {
    error = "flash failed (result " + std::to_string(static_cast<int>(result)) + ")";
    LOG_ERR("OTA", "install failed: %s", error.c_str());
    return false;
  }
  LOG_INF("OTA", "installed %s; inactive slot written, otadata switched", path);
  return true;
}

}  // namespace plus_ota
