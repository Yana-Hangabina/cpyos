#pragma once

#include <Logging.h>

// NetworkStartup.h pulls WiFi.h. On the Plus port the compat WiFi.h renames
// IDF's wifi_mode_t while it includes esp_wifi.h, so esp_wifi.h must not be
// included first (that would leave the real typedef visible and the Arduino
// enum would then conflict).
#include "NetworkStartup.h"
#include <esp_wifi.h>

// Network transfers can run for minutes (OPDS crawls, plugin API calls, OTA
// streams). Modem sleep powers the radio down between DTIM beacons, which adds
// multi-second latency to TLS handshakes and can stall packets mid-transfer,
// so disable WiFi power-save for the duration of the exchange. The previous
// mode is restored on scope exit, so guards nest safely and contexts that keep
// power-save off for their whole lifetime (the web server) are not reverted.
// The same scope also holds NetworkStartup's idle timer, so a long fetch can
// never have the radio torn down underneath it.
struct WifiPowerSaveGuard {
  WifiPowerSaveGuard() {
    NetworkStartup::hold();
    if (esp_wifi_get_ps(&previous) != ESP_OK) previous = WIFI_PS_MIN_MODEM;
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) LOG_ERR("WIFI", "Failed to disable WiFi power-save: %d", err);
  }
  ~WifiPowerSaveGuard() {
    if (previous != WIFI_PS_NONE) {
      esp_err_t err = esp_wifi_set_ps(previous);
      if (err != ESP_OK) LOG_ERR("WIFI", "Failed to restore WiFi power-save: %d", err);
    }
    NetworkStartup::releaseHold();
  }
  WifiPowerSaveGuard(const WifiPowerSaveGuard&) = delete;
  WifiPowerSaveGuard& operator=(const WifiPowerSaveGuard&) = delete;

 private:
  wifi_ps_type_t previous = WIFI_PS_MIN_MODEM;
};
