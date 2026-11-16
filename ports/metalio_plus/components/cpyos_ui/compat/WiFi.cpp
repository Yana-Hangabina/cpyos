// Real Wi-Fi backend for the Metalio E-INK4 Plus (ESP-IDF esp_wifi).
//
// The Arduino WiFi API is blocking: begin() waits for an IP by default and
// scanNetworks() is synchronous unless asked otherwise. This implementation
// keeps that contract on top of the IDF station netif, and is deliberately
// small: one STA interface, no AP mode yet.
//
// UNTESTED ON HARDWARE.

#include "WiFi.h"

#include "CrossPointSettings.h"
#include "PlusCellular.h"
#include "PortRadioState.h"

// Set while the radio brings its driver up/down. The SD log mirror pauses its
// file work during this window: esp_wifi_init needs large contiguous internal
// DMA blocks and an in-flight FAT window can starve it (ESP_ERR_NO_MEM).
volatile bool g_portRadioBusy = false;

// The S31 has both a 16-bit EPD feeder (two 128-line queues in internal RAM)
// and the radio competing for the same pool. The default Wi-Fi config asks for
// 16 static RX + 32 cached TX + 32 management buffers, which does not fit next
// to the panel (esp_wifi_init: ESP_ERR_NO_MEM) even though ~57 KB is free.
// These are runtime fields, so a lean profile is legal without touching the
// precompiled driver: enough buffers for one STA association and TLS, far less
// static memory.

// (The earlier reserved-arena experiment was removed: holding 40 KB from boot
// lowered the baseline free memory and did not make esp_wifi_init succeed.)

// IDF and the Arduino API both call their mode enum `wifi_mode_t`. Rename the
// IDF one inside this translation unit only; the public header keeps the
// Arduino type that every CPYOS activity uses.
#define wifi_mode_t idf_wifi_mode_t
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#undef wifi_mode_t
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr const char* TAG = "PLUS-WIFI";
wifi_init_config_t leanInitConfig() {
    // The panel's two 128-line feeder queues leave the stock 16 static RX
    // buffers without a contiguous internal block (esp_wifi_init NO_MEM). Two
    // are enough to associate, scan and run TLS. Everything else (TX cache,
    // management short buffers, aggregation) stays at the vendor defaults.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 8;
    cfg.nvs_enable = false;  // reference S31 profile: RAM-only driver config
    // BA aggregation makes each static RX buffer carry the reorder window
    // (16 x 1700 B); two of those do not fit, and even the second plain buffer
    // failed. Single-stream TLS/HTTP does not need aggregation.
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;
    cfg.rx_ba_win = 1;
    // With Wi-Fi/LWIP forced to internal RAM (SPIRAM_TRY_ALLOCATE_WIFI_LWIP=n)
    // the stock 16 static + 32 cached TX buffers consume ~77 KB and starve the
    // second static RX allocation. Four of each still cover one HTTP/TLS stream.
    cfg.static_tx_buf_num = 4;
    cfg.cache_tx_buf_num = 4;
    cfg.mgmt_sbuf_num = 6;
    return cfg;
}

constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;
constexpr EventBits_t kStartedBit = BIT2;

EventGroupHandle_t s_events = nullptr;
esp_netif_t* s_staNetif = nullptr;
esp_netif_t* s_apNetif = nullptr;
bool s_started = false;
bool s_connected = false;
bool s_initialised = false;
std::vector<wifi_ap_record_t> s_scan;
// Async scans: the activity polls scanComplete() while esp_wifi_scan_get_ap_num
// returns 0 until the driver publishes SCAN_DONE. Track the event explicitly so
// "still running" and "genuinely zero networks" stay distinguishable.
bool s_scanPending = false;
bool s_scanDone = false;
bool s_scanFailed = false;

esp_err_t s_lastError = ESP_OK;

esp_err_t ensureInit() {
    if (s_initialised) return s_lastError;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    err = esp_event_loop_create_default();
    if (err != ESP_OK) ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
    s_events = xEventGroupCreate();

    esp_event_handler_instance_t anyId;
    esp_event_handler_instance_t gotIpId;
    esp_event_handler_instance_t lostId;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, [](void*, esp_event_base_t, int32_t id, void* data) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const auto* d = static_cast<const wifi_event_sta_disconnected_t*>(data);
            ESP_LOGW(TAG, "station disconnected reason=%d", d != nullptr ? static_cast<int>(d->reason) : -1);
            s_connected = false;
            if (s_events) xEventGroupSetBits(s_events, kFailedBit);
        } else if (id == WIFI_EVENT_STA_START) {
            if (s_events) xEventGroupSetBits(s_events, kStartedBit);
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "station associated");
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            const auto* done = static_cast<const wifi_event_sta_scan_done_t*>(data);
            s_scanPending = false;
            s_scanDone = true;
            s_scanFailed = done != nullptr && done->status != 0;
            ESP_LOGD(TAG, "scan done: status=%d number=%u", done ? done->status : -1,
                     done ? static_cast<unsigned>(done->number) : 0u);
        }
    }, nullptr, &anyId);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, [](void*, esp_event_base_t, int32_t, void*) {
        s_connected = true;
        if (s_events) {
            xEventGroupClearBits(s_events, kFailedBit);
            xEventGroupSetBits(s_events, kConnectedBit);
        }
    }, nullptr, &gotIpId);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, [](void*, esp_event_base_t, int32_t, void*) {
        // The driver emits LOST_IP while starting, before any association. It
        // means "no lease right now", not "this connect attempt failed"; treating
        // it as a failure made the first saved-network attempt bail out in <1 s
        // and left the Wi-Fi sheet up until the scan-based retry (~40 s later).
        if (s_connected) {
            s_connected = false;
            if (s_events) xEventGroupSetBits(s_events, kFailedBit);
        }
    }, nullptr, &lostId);

    g_portRadioBusy = true;
    s_staNetif = esp_netif_create_default_wifi_sta();
    // Created up front so AP-mode callers get a DHCP server without extra steps.
    s_apNetif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = leanInitConfig();
    err = esp_wifi_init(&cfg);
    g_portRadioBusy = false;
    if (err != ESP_OK) ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
    esp_err_t errStorage = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (errStorage != ESP_OK) ESP_LOGE(TAG, "set_storage: %s", esp_err_to_name(errStorage));
    esp_err_t errMode = esp_wifi_set_mode(WIFI_MODE_STA);
    if (errMode != ESP_OK) ESP_LOGE(TAG, "set_mode: %s", esp_err_to_name(errMode));
    if (err == ESP_OK) err = errMode;
    s_lastError = err;
    s_initialised = true;
    return s_lastError;
}

// NetworkStartup::releaseRadio() calls esp_wifi_deinit() directly (the
// reference build's Arduino WiFi library re-initialises transparently). Netifs
// and the event loop survive that call, so re-create only the driver here.
esp_err_t ensureDriver() {
    const esp_err_t initErr = ensureInit();
    if (initErr != ESP_OK) return initErr;
    idf_wifi_mode_t current = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&current) == ESP_OK) return ESP_OK;
    wifi_init_config_t cfg = leanInitConfig();
    g_portRadioBusy = true;
    esp_err_t err = esp_wifi_init(&cfg);
    g_portRadioBusy = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init (re-init): %s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    const esp_err_t errMode = esp_wifi_set_mode(WIFI_MODE_STA);
    if (errMode != ESP_OK) {
        ESP_LOGE(TAG, "set_mode (re-init): %s", esp_err_to_name(errMode));
        return errMode;
    }
    s_started = false;
    s_scanPending = false;
    s_scanDone = false;
    s_scanFailed = false;
    ESP_LOGI(TAG, "driver re-initialised after a radio release");
    return ESP_OK;
}

esp_err_t startRadio() {
    const esp_err_t initErr = ensureDriver();
    if (initErr != ESP_OK) return initErr;
    if (!s_started) {
        if (s_events) xEventGroupClearBits(s_events, kStartedBit);
        const esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
            return err;
        }
        // esp_wifi_start() only queues STA_START. Calling esp_wifi_connect()
        // before the driver reaches that state makes the first association
        // fail (reason 1) while the scan-based retry ~40 s later succeeds --
        // that is the "it stops on the Wi-Fi list and waits for a tap" report.
        const EventBits_t started = xEventGroupWaitBits(s_events, kStartedBit, pdFALSE, pdTRUE,
                                                        pdMS_TO_TICKS(2000));
        if ((started & kStartedBit) == 0) {
            ESP_LOGW(TAG, "STA_START not seen within 2 s; connecting anyway");
        }
        s_started = true;
    }
    return ESP_OK;
}

}  // namespace

namespace {
// The official firmware selects WiFi or 4G as a persisted network type and
// reboots on change; it never auto-switches. 4G only makes sense when the
// module was detected, so a lightless/4G-less unit keeps the plain WiFi path.
bool cellularWanted() {
    return SETTINGS.networkType == CrossPointSettings::NETWORK_CELLULAR && SETTINGS.cellularPresent != 0;
}
}  // namespace

WiFiClass WiFi;

wl_status_t WiFiClass::status() {
    if (plus_cellular_active()) return plus_cellular_ready() ? WL_CONNECTED : WL_DISCONNECTED;
    if (!s_initialised) return WL_DISCONNECTED;
    if (s_connected) return WL_CONNECTED;
    wifi_ap_record_t ap{};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? WL_IDLE_STATUS : WL_DISCONNECTED;
}

int WiFiClass::begin(const char* ssid, const char* passphrase, int32_t, const uint8_t*, bool connect) {
    if (cellularWanted()) return plus_cellular_ready() ? WL_CONNECTED : WL_CONNECT_FAILED;
    if (ssid == nullptr) return WL_CONNECT_FAILED;
    if (startRadio() != ESP_OK) return WL_CONNECT_FAILED;
    wifi_config_t config{};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid, sizeof(config.sta.ssid) - 1);
    if (passphrase != nullptr) {
        std::strncpy(reinterpret_cast<char*>(config.sta.password), passphrase,
                     sizeof(config.sta.password) - 1);
    }
    // Accept WPA2, WPA3-SAE and transition APs: the default threshold
    // (WPA2_PSK) made a WPA3-only AP answer AUTH_EXPIRE (reason 2).
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &config);
    s_connected = false;
    if (!connect) {
        if (s_events) xEventGroupClearBits(s_events, kConnectedBit | kFailedBit);
        ESP_LOGI(TAG, "connecting to '%s'", ssid);
        esp_wifi_connect();
        return WL_IDLE_STATUS;
    }

    // Arduino's default: block until an IP arrives. One association retry is
    // deliberate: the first attempt right after the radio starts has been seen
    // to fail once and succeed on the next try (the caller would otherwise sit
    // on the Wi-Fi sheet until the scan-based fallback, ~40 s later).
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (s_events) xEventGroupClearBits(s_events, kConnectedBit | kFailedBit);
        ESP_LOGI(TAG, "connecting to '%s'%s", ssid, attempt == 0 ? "" : " (retry)");
        esp_wifi_connect();
        const EventBits_t bits = xEventGroupWaitBits(s_events, kConnectedBit | kFailedBit,
                                                     pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        if (bits & kConnectedBit) {
            ESP_LOGI(TAG, "station connected");
            return WL_CONNECTED;
        }
        if (attempt == 0) vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGW(TAG, "station connect failed");
    return WL_CONNECT_FAILED;
}

bool WiFiClass::mode(wifi_mode_t m) {
    if (cellularWanted()) {
        if (m == WIFI_OFF) {
            plus_cellular_stop();
            return true;
        }
        // The modem brings up its own netif + DHCP; block until it has an IP so
        // callers that request immediately afterwards (TLS, HTTP) see a
        // connected station. Cold start can take a while on 4G.
        ESP_LOGI(TAG, "starting 4G data path");
        return plus_cellular_start(45000);
    }
    if (plus_cellular_active()) plus_cellular_stop();
    if (ensureDriver() != ESP_OK) return false;
    const idf_wifi_mode_t idfMode = m == WIFI_OFF     ? WIFI_MODE_NULL
                                    : m == WIFI_STA   ? WIFI_MODE_STA
                                    : m == WIFI_AP    ? WIFI_MODE_AP
                                                      : WIFI_MODE_APSTA;
    return esp_wifi_set_mode(idfMode) == ESP_OK;
}

wifi_mode_t WiFiClass::getMode() {
    if (plus_cellular_active()) return WIFI_STA;
    if (!s_initialised) return WIFI_OFF;
    idf_wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    switch (mode) {
        case WIFI_MODE_STA: return WIFI_STA;
        case WIFI_MODE_AP: return WIFI_AP;
        case WIFI_MODE_APSTA: return WIFI_AP_STA;
        default: return WIFI_OFF;
    }
}

bool WiFiClass::disconnect(bool radioOff, bool) {
    if (plus_cellular_active()) {
        if (radioOff) plus_cellular_stop();
        return true;
    }
    if (!s_initialised) return true;
    esp_wifi_disconnect();
    s_connected = false;
    if (radioOff) {
        esp_wifi_stop();
        s_started = false;
    }
    return true;
}

bool WiFiClass::setSleep(bool enable) {
    return esp_wifi_set_ps(enable ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) == ESP_OK;
}

bool WiFiClass::setAutoReconnect(bool) { return true; }
bool WiFiClass::persistent(bool) { return true; }

bool WiFiClass::setHostname(const char* name) {
    ensureInit();
    return esp_netif_set_hostname(s_staNetif, name) == ESP_OK;
}

String WiFiClass::getHostname() {
    const char* name = nullptr;
    if (s_staNetif != nullptr && esp_netif_get_hostname(s_staNetif, &name) == ESP_OK && name != nullptr) {
        return String(name);
    }
    return String();
}

bool WiFiClass::hostByName(const char* host, IPAddress& out) {
    if (host == nullptr || *host == '\0') return false;
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const int rc = getaddrinfo(host, nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        ESP_LOGW(TAG, "getaddrinfo(%s) failed: %d", host, rc);
        if (res != nullptr) freeaddrinfo(res);
        return false;
    }
    const auto* addr = reinterpret_cast<const struct sockaddr_in*>(res->ai_addr);
    out = IPAddress(static_cast<uint32_t>(addr->sin_addr.s_addr));
    freeaddrinfo(res);
    return true;
}

bool WiFiClass::setDNS(IPAddress primary, IPAddress secondary) {
    if (s_staNetif == nullptr) return false;
    esp_netif_dns_info_t mainDns{};
    mainDns.ip.type = ESP_IPADDR_TYPE_V4;
    mainDns.ip.u_addr.ip4.addr = static_cast<uint32_t>(primary);
    if (esp_netif_set_dns_info(s_staNetif, ESP_NETIF_DNS_MAIN, &mainDns) != ESP_OK) return false;
    if (static_cast<uint32_t>(secondary) != 0) {
        esp_netif_dns_info_t backupDns{};
        backupDns.ip.type = ESP_IPADDR_TYPE_V4;
        backupDns.ip.u_addr.ip4.addr = static_cast<uint32_t>(secondary);
        (void)esp_netif_set_dns_info(s_staNetif, ESP_NETIF_DNS_BACKUP, &backupDns);
    }
    // A cached A record outlives the resolver change; flush it or the next
    // lookup still answers with whatever DHCP taught lwIP.
    dns_clear_cache();
    ESP_LOGI(TAG, "DNS main %u.%u.%u.%u", primary[0], primary[1], primary[2], primary[3]);
    return true;
}

bool WiFiClass::useDhcpDns() {
    if (s_staNetif == nullptr) return false;
    // Restarting the client is the only supported way to get the DHCP-provided
    // servers back: there is no "clear custom DNS" call in esp_netif.
    esp_netif_dhcpc_stop(s_staNetif);
    const esp_err_t err = esp_netif_dhcpc_start(s_staNetif);
    dns_clear_cache();
    ESP_LOGI(TAG, "DNS back to DHCP (%s)", esp_err_to_name(err));
    return err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED;
}

IPAddress WiFiClass::localIP() {
    if (plus_cellular_active()) {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        esp_netif_ip_info_t info{};
        if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
            return IPAddress(static_cast<uint32_t>(info.ip.addr));
        }
        return IPAddress();
    }
    IPAddress result;
    if (s_staNetif == nullptr) return result;
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(s_staNetif, &info) != ESP_OK) return result;
    return IPAddress(static_cast<uint32_t>(info.ip.addr));
}

IPAddress WiFiClass::softAPIP() {
    IPAddress result;
    if (s_apNetif == nullptr) return result;
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(s_apNetif, &info) != ESP_OK) return result;
    return IPAddress(static_cast<uint32_t>(info.ip.addr));
}

int8_t WiFiClass::scanNetworks(bool async, bool showHidden) {
    if (cellularWanted()) return 0;
    const esp_err_t startErr = startRadio();
    if (startErr != ESP_OK) {
        ESP_LOGE(TAG, "scan skipped: radio %s", esp_err_to_name(startErr));
        return -1;
    }
    wifi_scan_config_t config{};
    config.show_hidden = showHidden;
    s_scanDone = false;
    s_scanFailed = false;
    s_scanPending = true;
    const esp_err_t scanErr = esp_wifi_scan_start(&config, /*block=*/false);
    if (scanErr != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start: %s", esp_err_to_name(scanErr));
        s_scanPending = false;
        return -1;
    }
    if (async) return 0;
    // Bounded wait. esp_wifi_scan_start(block=true) waits forever when the
    // SCAN_DONE event is lost (radio released mid-scan, driver wedged); that
    // hung the console task, which is the only recovery channel on a board
    // without a reset key.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(8000);
    while (!s_scanDone && xTaskGetTickCount() < deadline) vTaskDelay(pdMS_TO_TICKS(25));
    s_scanPending = false;
    if (!s_scanDone) {
        ESP_LOGE(TAG, "scan timed out after 8 s");
        return -1;
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    s_scan.resize(count);
    if (count > 0 && esp_wifi_scan_get_ap_records(&count, s_scan.data()) == ESP_OK) {
        s_scan.resize(count);
    } else {
        s_scan.clear();
    }
    return static_cast<int8_t>(std::min<size_t>(s_scan.size(), 127));
}

int16_t WiFiClass::scanComplete() {
    if (s_scanPending) return WIFI_SCAN_RUNNING;
    if (!s_scanDone) {
        // Never scanned through this wrapper: report the driver's latest count
        // the way the Arduino API does.
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        return static_cast<int16_t>(count);
    }
    if (s_scanFailed) {
        s_scan.clear();
        return WIFI_SCAN_FAILED;
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    s_scan.resize(count);
    if (count > 0 && esp_wifi_scan_get_ap_records(&count, s_scan.data()) == ESP_OK) {
        s_scan.resize(count);
    } else {
        s_scan.clear();
    }
    s_scanDone = false;  // results are now in s_scan; callers use scanDelete()
    return static_cast<int16_t>(std::min<size_t>(s_scan.size(), 127));
}

void WiFiClass::scanDelete() {
    s_scan.clear();
    s_scanPending = false;
    s_scanDone = false;
    s_scanFailed = false;
}

String WiFiClass::SSID(uint8_t networkItem) {
    if (networkItem >= s_scan.size()) return String();
    return String(reinterpret_cast<const char*>(s_scan[networkItem].ssid));
}

String WiFiClass::SSID() {
    if (plus_cellular_active()) return String("4G");
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return String(reinterpret_cast<const char*>(ap.ssid));
    }
    return String();
}

int32_t WiFiClass::RSSI(uint8_t networkItem) {
    if (plus_cellular_active()) return -70;  // fixed icon strength; the modem has CSQ, not RSSI
    if (networkItem == 0 && s_scan.empty() && s_connected) {
        wifi_ap_record_t ap{};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    }
    return networkItem < s_scan.size() ? s_scan[networkItem].rssi : 0;
}

uint8_t WiFiClass::encryptionType(uint8_t networkItem) {
    if (networkItem >= s_scan.size()) return 0;
    return static_cast<uint8_t>(s_scan[networkItem].authmode);
}

uint8_t* WiFiClass::BSSID(uint8_t networkItem) {
    if (networkItem >= s_scan.size()) return nullptr;
    return s_scan[networkItem].bssid;
}

uint8_t* WiFiClass::BSSID(uint8_t* out) {
    if (out == nullptr) return nullptr;
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(out, ap.bssid, 6);
    } else {
        memset(out, 0, 6);
    }
    return out;
}

int32_t WiFiClass::channel(uint8_t networkItem) {
    if (networkItem >= s_scan.size()) return 0;
    return s_scan[networkItem].primary;
}

bool WiFiClass::softAP(const char* ssid, const char* passphrase, int channel, int ssidHidden, int maxConnection,
                       bool, int) {
    if (ssid == nullptr) return false;
    if (ensureDriver() != ESP_OK) return false;
    // Keep the station up if it is already running: file transfer can be useful
    // while the device is also joined to a network.
    const bool keepStation = getMode() == WIFI_STA || getMode() == WIFI_AP_STA;
    if (esp_wifi_set_mode(keepStation ? WIFI_MODE_APSTA : WIFI_MODE_AP) != ESP_OK) return false;

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.ap.ssid), ssid, sizeof(config.ap.ssid) - 1);
    config.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
    if (channel > 0 && channel <= 14) config.ap.channel = static_cast<uint8_t>(channel);
    const bool secured = passphrase != nullptr && passphrase[0] != '\0';
    if (secured) {
        std::strncpy(reinterpret_cast<char*>(config.ap.password), passphrase, sizeof(config.ap.password) - 1);
    }
    config.ap.authmode = secured ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.ap.max_connection = maxConnection > 0 ? static_cast<uint8_t>(maxConnection) : 4;
    config.ap.ssid_hidden = ssidHidden ? 1 : 0;
    if (esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK) return false;
    const esp_err_t err = esp_wifi_start();
    s_started = err == ESP_OK;
    return s_started;
}

bool WiFiClass::softAPdisconnect(bool radioOff) {
    if (!s_initialised) return true;
    const bool hadStation = getMode() == WIFI_AP_STA;
    if (radioOff) {
        esp_wifi_stop();
        s_started = false;
    }
    return esp_wifi_set_mode(hadStation ? WIFI_MODE_STA : WIFI_MODE_NULL) == ESP_OK;
}

uint8_t WiFiClass::softAPgetStationNum() {
    if (!s_initialised) return 0;
    wifi_sta_list_t stations{};
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) return 0;
    return static_cast<uint8_t>(stations.num);
}

void WiFiClass::macAddress(uint8_t* mac) {
    if (mac != nullptr) esp_wifi_get_mac(WIFI_IF_STA, mac);
}

String WiFiClass::macAddress() {
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    return String(text);
}

String IPAddress::toString() const {
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", octets_[0], octets_[1], octets_[2], octets_[3]);
    return String(text);
}
