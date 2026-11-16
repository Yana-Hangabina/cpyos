#pragma once

// Vendor OTA (CloudZao Burner) for the Metalio E-Ink4 Plus.
//
// Protocol: POST https://admin.metalio.world/xiaozhi/api/firmware-ota/check
//   X-OTA-Key: <personal key>          (compiled in from a local-only file)
//   Device-Id: <MAC>                   (esp_efuse_mac_get_default, AA:BB:..)
//   X-OTA-Channel: test                (only for the test channel)
//   {"chip_model_name":"esp32s31","board":{"type":"Metalio E-Ink4 Plus"},
//    "application":{"name":"CPYOS"}}
//
// The firmware is downloaded to the card and then written by the same
// firmware_flash path the SD update page uses (validated image, inactive OTA
// slot, otadata switch), so OTA and SD update cannot drift apart.
//
// The key must never be committed or logged: it lives in
// ports/metalio_plus/components/cpyos_app/plus_ota_key.local (gitignored) and
// is injected by CMake as PLUS_OTA_KEY. Without it `configured()` is false and
// the page explains that this build has no OTA credentials.

#include <cstddef>
#include <cstdint>
#include <string>

namespace plus_ota {

enum class Channel : uint8_t { Stable = 0, Test = 1 };

struct Info {
  bool ok = false;            // request + parse succeeded
  int http = 0;               // HTTP status (0 = transport failure)
  bool hasUpdate = false;     // remote version differs from the running one
  std::string version;
  std::string url;
  std::string firmwareName;
  std::string error;          // human readable; NEVER contains the key
  size_t size = 0;
};

// False when this build has no PLUS_OTA_KEY (OTA page stays explanatory).
bool configured();
// Running app version (esp_app_desc).
const char* currentVersion();
// "AA:BB:CC:DD:EE:FF" (cached).
const char* deviceId();
const char* boardName();   // "Metalio E-Ink4 Plus"
const char* chipName();    // "esp32s31"
const char* appName();     // "脆皮鸭CPYOS" (must match the firmware title)

// Blocking; call from a worker task (TLS + heap).
Info check(Channel channel);

// Stream `info.url` to `destPath` with progress (bytes so far, total).
bool download(const Info& info, const char* destPath,
              void (*progress)(size_t, size_t, void*), void* ctx, const bool* cancelFlag, std::string& error);

// Validate + write to the inactive OTA slot + switch otadata. Does NOT reboot.
bool install(const char* path, std::string& error);

}  // namespace plus_ota
