#include "plus_ota.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "network/FirmwareFlasher.h"
#include "network/HttpDownloader.h"
#include "plus_cloudzao_ids.h"

namespace plus_ota {
namespace {

constexpr const char* kCheckPath = "/updates/check";
// The OTA service registers the Plus under the vendor board name and the
// esp32s3 chip model (the platform's hardware-model row), even though the
// silicon is ESP32-S31. The resource/binding service uses different strings;
// do not share these constants with plus_resources.cpp.
constexpr const char* kBoard = "Metalio E-Ink4 Plus";
constexpr const char* kChip = "esp32s3";
constexpr const char* kApp = "脆皮鸭CPYOS";

std::string g_deviceId;

const char* deviceIdCached() {
  if (g_deviceId.empty()) {
    uint8_t mac[6] = {0};
    // The vendor firmware and the activation mini-program identify a device by
    // its Wi-Fi station MAC, not by the eFuse base MAC. On ESP32-S31 those
    // differ (the STA MAC is base + 4 here), so reading eFuse directly makes
    // the resource service believe the bound device is a different one.
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
      char buf[18];
      std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
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
      return "请求无效（400）：" + body;
    case 401:
      return "开发者资源密钥缺失或已失效（401）";
    case 404:
      return "没有可用的固件版本（404）：" + body;
    case 409:
      return "OTA 已关闭或下载地址不可用（409）：" + body;
    default:
      return (http > 0 ? ("HTTP " + std::to_string(http) + ": ") : std::string("no response: ")) + body;
  }
}

// The endpoint's has_update is "this firmware exists", not "this is newer".
// Compare the advertised version against the running app version instead.
bool parseVersion(std::string text, int parts[4], std::string& prerelease) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.erase(text.begin());
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                           text.back() == '\n')) {
    text.pop_back();
  }
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.erase(text.begin());
  const size_t cut = text.find_first_of("-+");
  prerelease = cut == std::string::npos ? std::string() : text.substr(cut + 1);
  if (cut != std::string::npos) text.resize(cut);
  for (int i = 0; i < 4; ++i) parts[i] = 0;
  int count = 0;
  size_t pos = 0;
  while (pos < text.size() && count < 4) {
    if (!std::isdigit(static_cast<unsigned char>(text[pos]))) return false;
    int value = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) && value < 1000000) {
      value = value * 10 + (text[pos] - '0');
      ++pos;
    }
    parts[count++] = value;
    if (pos < text.size() && text[pos] == '.') {
      ++pos;
      continue;
    }
    break;
  }
  return count > 0 && (pos == text.size() || text[pos] == '.');
}

bool versionIsNewer(const std::string& remote, const std::string& local) {
  int rp[4], lp[4];
  std::string rpre, lpre;
  if (!parseVersion(remote, rp, rpre) || !parseVersion(local, lp, lpre)) {
    return false;  // never advertise an update from an unparseable version
  }
  for (int i = 0; i < 4; ++i) {
    if (rp[i] != lp[i]) return rp[i] > lp[i];
  }
  if (rpre == lpre) return false;
  if (rpre.empty()) return true;   // release beats a prerelease
  if (lpre.empty()) return false;
  return rpre > lpre;
}

}  // namespace

bool configured() { return plus_cloudzao::kIntegrationId[0] != '\0' && plus_cloudzao::kFirmwareId[0] != '\0'; }
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
  (void)channel;  // the partner API only serves reviewed stable versions
  if (!configured()) {
    info.error = "this build has no CloudZao integration id";
    return info;
  }
  JsonDocument request;
  request["integrationId"] = plus_cloudzao::kIntegrationId;
  request["firmwareId"] = plus_cloudzao::kFirmwareId;
  request["currentVersion"] = currentVersion();
  std::string payload;
  serializeJson(request, payload);

  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(20000);
  const std::string url = std::string(plus_cloudzao::kBaseUrl) + kCheckPath;
  if (!http.begin(url.c_str())) {
    info.error = "cannot open the OTA endpoint";
    return info;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(payload);
  const std::string body = http.getString();
  http.end();
  info.http = code;
  LOG_INF("OTA", "partner check: http=%d body=%u B", code, static_cast<unsigned>(body.size()));
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
  const bool advertised = resp["updateAvailable"] | false;
  const JsonVariantConst release = resp["release"];
  if (release.is<JsonObjectConst>()) {
    info.version = release["version"] | "";
    info.url = release["downloadUrl"] | "";
    info.size = release["sizeBytes"] | 0;
  }
  info.versionStatus = "latest";
  info.firmwareName = kApp;
  info.productName = kApp;
  info.firmwareId = plus_cloudzao::kFirmwareId;
  info.ok = true;
  info.hasUpdate = advertised && !info.version.empty() && versionIsNewer(info.version, currentVersion());
  if (info.hasUpdate) {
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
      info.ok = false;
      info.hasUpdate = false;
      info.error = "此分区表没有可用的非活动 OTA 槽";
      return info;
    }
    if (info.size != 0 && info.size > target->size) {
      info.ok = false;
      info.hasUpdate = false;
      info.error = "固件包大于非活动 OTA 槽（" + std::to_string(info.size) + " > " +
                   std::to_string(target->size) + "），已拒绝安装";
      LOG_ERR("OTA", "firmware does not fit the inactive slot: %u > %u",
              static_cast<unsigned>(info.size), static_cast<unsigned>(target->size));
      return info;
    }
  }
  if (!info.hasUpdate) {
    LOG_INF("OTA", "no update: remote=%s local=%s advertised=%d", info.version.c_str(), currentVersion(),
            advertised ? 1 : 0);
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
