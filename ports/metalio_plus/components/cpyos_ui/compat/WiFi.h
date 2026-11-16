#pragma once

#include <cstdint>

// The activities use IDF constants (WIFI_AUTH_OPEN, WIFI_ALL_CHANNEL_SCAN,
// WIFI_MODE_*), so expose esp_wifi.h here. Its `wifi_mode_t` is renamed around
// the include to leave the Arduino enum below as the visible type.
#define wifi_mode_t idf_wifi_mode_t
#include <esp_wifi.h>
#undef wifi_mode_t

#include "IPAddress.h"
#include "WiFiClient.h"
#include "WiFiClientSecure.h"
#include "WString.h"

typedef enum {
  WL_NO_SHIELD = 255,
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6,
} wl_status_t;

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;

// Arduino scan-result sentinels (asynchronous scan).
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

class WiFiClass {
 public:
  wl_status_t status();
  int begin(const char* ssid, const char* passphrase = nullptr, int32_t channel = 0,
            const uint8_t* bssid = nullptr, bool connect = true);
  bool mode(wifi_mode_t m);
  wifi_mode_t getMode();
  bool disconnect(bool radioOff = false, bool eraseAp = false);
  bool setSleep(bool enable);
  bool setAutoReconnect(bool enable);
  bool persistent(bool enable);
  bool setHostname(const char* name);
  String getHostname();
  // Bounded A-record lookup through lwIP. Exposed for bench diagnostics: the
  // router's DNS here hands out a dead CDN edge for one of the book sources,
  // and there is no way to prove the firmware path without seeing what the
  // resolver actually returned.
  bool hostByName(const char* host, IPAddress& out);
  // Override / restore the resolver applied to the station netif. setDNS()
  // flushes lwIP's cache so an entry learned from the DHCP servers cannot keep
  // answering after the switch.
  bool setDNS(IPAddress primary, IPAddress secondary = IPAddress());
  bool useDhcpDns();
  IPAddress localIP();
  IPAddress softAPIP();
  int8_t scanNetworks(bool async = false, bool showHidden = false);
  int16_t scanComplete();
  void scanDelete();
  String SSID(uint8_t networkItem);
  String SSID();  // current station SSID
  int32_t RSSI(uint8_t networkItem = 0);
  uint8_t encryptionType(uint8_t networkItem);
  uint8_t* BSSID(uint8_t networkItem = 0);
  // Arduino also has BSSID(uint8_t* out): fill the six bytes of the
  // current station (or zeros when not associated).
  uint8_t* BSSID(uint8_t* out);
  int32_t channel(uint8_t networkItem = 0);
  bool softAP(const char* ssid, const char* passphrase = nullptr, int channel = 1, int ssidHidden = 0,
              int maxConnection = 4, bool ftmResponder = false, int ftmMaxBurstMs = 8);
  bool softAPdisconnect(bool wifioff = false);
  uint8_t softAPgetStationNum();
  void setScanMethod(int) {}
  void setSortMethod(int) {}
  void macAddress(uint8_t* mac);
  String macAddress();

  // Real esp_wifi backend (station + SoftAP). SoftAP exists for AP-mode file
  // transfer; there is still no web server activity on this port.
  static constexpr bool kBackendAvailable = true;
};

// Reserve the internal-DMA block the radio needs before the rest of the boot
// fragments the heap. Call once, right after the display is up.

extern WiFiClass WiFi;
