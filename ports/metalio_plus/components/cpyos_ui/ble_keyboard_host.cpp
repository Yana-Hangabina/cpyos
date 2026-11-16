// BLE HID host on IDF NimBLE for the Metalio Plus.
//
// Scope: scan -> connect (Just Works bonding) -> subscribe to the HID Report
// characteristic -> decode keyboard boot reports and consumer-control reports.
// Raw button edges and passkey entry are not produced (the app does not use
// them on this port).
//
// UNTESTED ON HARDWARE.

#include "BleKeyboardHost.h"

#include "ble_hid_decode.h"

#include <Arduino.h>

#include <esp_log.h>
#include <nvs.h>

#include <cstdio>
#include <cstring>

extern "C" {
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
}

namespace freeink {
namespace {

constexpr const char* TAG = "PLUS-BLE";
constexpr uint16_t kHidService = 0x1812;
constexpr uint16_t kReportChr = 0x2A4D;

bool s_synced = false;
uint8_t s_ownAddrType = 0;
uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
uint16_t s_svcStart = 0;
uint16_t s_svcEnd = 0;
uint16_t s_reportVal = 0;
uint8_t s_prevKeys[6] = {0};
uint8_t s_heldUsage = 0;
uint32_t s_heldSinceMs = 0;
uint32_t s_lastRepeatMs = 0;
uint8_t s_lastConsumerCode = 0xFF;
uint32_t s_lastConsumerMs = 0;
BleKeyboardHost* s_host = nullptr;

void persistBonds(const BleKeyboardHost& host);

// A newly encrypted link is a bonded peer: remember it so the settings UI and
// reconnect path can use it. Oldest entry is evicted when full.
void rememberBond(BleKeyboardHost& host, const char* addr, const char* name, uint8_t type) {
    if (addr == nullptr || addr[0] == '\0') return;
    int existing = -1;
    for (uint8_t i = 0; i < host.bondCount_; ++i) {
        if (std::strcmp(host.bonds_[i].addr, addr) == 0) existing = i;
    }
    if (existing < 0) {
        if (host.bondCount_ >= BleKeyboardHost::kMaxBonds) {
            for (uint8_t i = 1; i < host.bondCount_; ++i) host.bonds_[i - 1] = host.bonds_[i];
            host.bondCount_ = BleKeyboardHost::kMaxBonds - 1;
        }
        existing = host.bondCount_++;
    }
    PairedHidDevice& entry = host.bonds_[existing];
    std::snprintf(entry.addr, sizeof(entry.addr), "%s", addr);
    std::snprintf(entry.name, sizeof(entry.name), "%s", name != nullptr && name[0] != '\0' ? name : addr);
    entry.addrType = type;
    persistBonds(host);
    ESP_LOGI(TAG, "bonded %s (%s)", entry.addr, entry.name);
}

void addrToString(const ble_addr_t& addr, char out[18]) {
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", addr.val[5], addr.val[4], addr.val[3], addr.val[2],
                  addr.val[1], addr.val[0]);
}

bool parseAddr(const char* text, ble_addr_t* out) {
    unsigned values[6] = {};
    if (std::sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &values[0], &values[1], &values[2], &values[3], &values[4],
                    &values[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) out->val[i] = static_cast<uint8_t>(values[5 - i]);
    out->type = 0;
    return true;
}

void loadBonds(BleKeyboardHost& host) {
    nvs_handle_t handle;
    if (nvs_open("blehid", NVS_READONLY, &handle) != ESP_OK) return;
    for (uint8_t i = 0; i < BleKeyboardHost::kMaxBonds; ++i) {
        char key[4] = {'b', static_cast<char>('0' + i), 0, 0};
        PairedHidDevice entry{};
        uint8_t type = 0;
        size_t len = sizeof(entry.addr);
        if (nvs_get_blob(handle, key, entry.addr, &len) == ESP_OK && len == sizeof(entry.addr)) {
            // Older builds wrote addr and name under the SAME key, so the name
            // replaced the address. Such a record has a 48-byte blob and is
            // dropped here instead of exposing a bogus peer.
            char nameKey[4] = {'n', static_cast<char>('0' + i), 0, 0};
            len = sizeof(entry.name);
            nvs_get_blob(handle, nameKey, entry.name, &len);
            char typeKey[5] = {'t', static_cast<char>('0' + i), 0, 0};
            len = sizeof(type);
            nvs_get_u8(handle, typeKey, &type);
            entry.addrType = type;
            if (i < BleKeyboardHost::kMaxBonds) {
                host.bonds_[host.bondCount_++] = entry;
            }
        }
    }
    nvs_close(handle);
}

void persistBonds(const BleKeyboardHost& host) {
    nvs_handle_t handle;
    if (nvs_open("blehid", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_all(handle);
    for (uint8_t i = 0; i < host.bondCount_; ++i) {
        char key[4] = {'b', static_cast<char>('0' + i), 0, 0};
        char nameKey[4] = {'n', static_cast<char>('0' + i), 0, 0};
        nvs_set_blob(handle, key, host.bonds_[i].addr, sizeof(host.bonds_[i].addr));
        nvs_set_blob(handle, nameKey, host.bonds_[i].name, sizeof(host.bonds_[i].name));
        char typeKey[5] = {'t', static_cast<char>('0' + i), 0, 0};
        nvs_set_u8(handle, typeKey, host.bonds_[i].addrType);
    }
    nvs_commit(handle);
    nvs_close(handle);
}

void queueKey(KeyEvent event) {
    BleKeyboardHost& host = BleKeyboardHost::getInstance();
    const uint8_t next = static_cast<uint8_t>((host.ringHead_ + 1) % BleKeyboardHost::kKeyQueueLen);
    if (next == host.ringTail_) return;
    host.ring_[host.ringHead_] = event;
    host.ringHead_ = next;
}

void emitUsage(uint8_t usage, uint8_t mods) {
    if (usage == 0) return;
    KeyEvent event;
    event.keycode = usage;
    event.mods = mods;
    event.ch = ble_hid::asciiForUsage(usage, mods);
    event.special = ble_hid::specialForUsage(usage);
    // Unknown-but-standard HID usages still reach the capture page as a raw
    // keycode; without this a pager whose buttons are not in the ASCII/special
    // tables could never be learned. Unmapped keys are ignored downstream.
    queueKey(event);
}

void applyReport(const uint8_t* data, size_t len) {
    if (len == 0) {
        s_heldUsage = 0;
        s_lastConsumerCode = 0xFF;
        return;
    }
    ble_hid::stripReportId(data, len, 8, 2);
    if (len >= 8) {
        ble_hid::Edge edges[6];
        const size_t count = ble_hid::decodeKeyboard(data, len, s_prevKeys, edges, 6);
        for (size_t i = 0; i < count; ++i) {
            emitUsage(edges[i].usage, edges[i].mods);
        }
        uint8_t first = 0;
        for (size_t i = 0; i < 6; ++i) {
            if (data[2 + i] != 0) {
                first = data[2 + i];
                break;
            }
        }
        if (first == 0) {
            s_heldUsage = 0;
        } else if (first != s_heldUsage) {
            s_heldUsage = first;
            s_heldSinceMs = millis();
            s_lastRepeatMs = s_heldSinceMs;
        }
        return;
    }
    ble_hid::Edge edge;
    if (ble_hid::decodeConsumer(data, len, &edge)) {
        // A pager may repeat the consumer report while the button is held. One
        // page per 500 ms, and never through the keyboard repeat path (which
        // would pass the 0x80 marker to emitUsage and silently drop it).
        const uint32_t now = millis();
        if (edge.usage == s_lastConsumerCode && now - s_lastConsumerMs < 500) return;
        s_lastConsumerCode = edge.usage;
        s_lastConsumerMs = now;
        KeyEvent event;
        event.keycode = edge.usage;
        switch (edge.usage) {
            case 0: event.special = SpecialKey::Left; break;
            case 1: event.special = SpecialKey::Right; break;
            case 2: event.special = SpecialKey::Enter; break;
            default: return;
        }
        queueKey(event);
        s_heldUsage = 0;
    } else {
        s_heldUsage = 0;
        s_lastConsumerCode = 0xFF;
        // Some pagers send a one-byte vendor code instead of HID usages.
        // Surface it as a raw keyboard usage so the capture page can bind it;
        // unmapped codes do nothing.
        if (len == 1 && data[0] != 0 && data[0] < 0x80) {
            KeyEvent event;
            event.keycode = data[0];
            queueKey(event);
        }
    }
}

int chrCallback(uint16_t conn, const struct ble_gatt_error* error, const struct ble_gatt_chr* chr, void*) {
    if (chr != nullptr && ble_uuid_u16(&chr->uuid.u) == kReportChr &&
        (chr->properties & BLE_GATT_CHR_PROP_NOTIFY) != 0) {
        s_reportVal = chr->val_handle;
    }
    if (error->status == BLE_HS_EDONE) {
        BleKeyboardHost& host = BleKeyboardHost::getInstance();
        if (s_reportVal != 0) {
            const uint16_t cccd = s_reportVal + 1;
            uint8_t subscribe[2] = {1, 0};
            ble_gattc_write_flat(conn, cccd, subscribe, sizeof(subscribe), nullptr, nullptr);
            host.connected_ = true;
            host.connecting_ = false;
            ESP_LOGI(TAG, "HID report subscribed (handle %u)", cccd);
        } else {
            host.connecting_ = false;
            host.connectFailed_ = true;
            std::snprintf(host.connectFailure_, sizeof(host.connectFailure_), "%s", "no HID report");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

int svcCallback(uint16_t conn, const struct ble_gatt_error* error, const struct ble_gatt_svc* svc, void*) {
    if (svc != nullptr) {
        s_svcStart = svc->start_handle;
        s_svcEnd = svc->end_handle;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_svcStart != 0) {
            s_reportVal = 0;
            ble_gattc_disc_all_chrs(conn, s_svcStart, s_svcEnd, chrCallback, nullptr);
        } else {
            BleKeyboardHost& host = BleKeyboardHost::getInstance();
            host.connecting_ = false;
            host.connectFailed_ = true;
            std::snprintf(host.connectFailure_, sizeof(host.connectFailure_), "%s", "no HID service");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

int gapEvent(struct ble_gap_event* event, void* arg) {
    BleKeyboardHost& host = *static_cast<BleKeyboardHost*>(arg);
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            const struct ble_gap_disc_desc& disc = event->disc;
            char addr[18];
            addrToString(disc.addr, addr);
            char name[32] = {0};
            bool hid = false;
            bool hasName = false;
            const uint8_t* p = disc.data;
            int remaining = disc.length_data;
            while (remaining >= 2) {
                const uint8_t length = p[0];
                if (length == 0 || length + 1 > remaining) break;
                const uint8_t type = p[1];
                const uint8_t* payload = p + 2;
                const uint8_t payloadLen = length - 1;
                if (type == 0x09 && payloadLen > 0) {
                    const size_t copy = payloadLen < sizeof(name) - 1 ? payloadLen : sizeof(name) - 1;
                    std::memcpy(name, payload, copy);
                    name[copy] = '\0';
                    hasName = true;
                } else if ((type == 0x02 || type == 0x03) && payloadLen >= 2) {
                    for (int i = 0; i + 1 < payloadLen; i += 2) {
                        const uint16_t uuid = static_cast<uint16_t>(payload[i] | (payload[i + 1] << 8));
                        if (uuid == kHidService) hid = true;
                    }
                }
                p += length + 1;
                remaining -= length + 1;
            }
            int existing = -1;
            for (uint8_t i = 0; i < host.deviceCount_; ++i) {
                if (std::strcmp(host.devices_[i].addr, addr) == 0) {
                    existing = i;
                    break;
                }
            }
            if (existing < 0) {
                if (host.deviceCount_ >= BleKeyboardHost::kMaxDiscovered) break;
                existing = host.deviceCount_++;
                host.devices_[existing] = DiscoveredDevice{};
                std::snprintf(host.devices_[existing].addr, sizeof(host.devices_[existing].addr), "%s", addr);
            }
            DiscoveredDevice& device = host.devices_[existing];
            if (hasName) {
                std::snprintf(device.name, sizeof(device.name), "%s", name);
                device.hasName = true;
            } else if (device.name[0] == '\0') {
                std::snprintf(device.name, sizeof(device.name), "%s", addr);
            }
            device.rssi = disc.rssi;
            device.addrType = disc.addr.type;
            device.hid = device.hid || hid;
            device.connectable = disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND;
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            host.scanning_ = false;
            return 0;
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn = event->connect.conn_handle;
                host.connecting_ = false;
                host.autoReconnect_ = true;
                host.reconnectAttempts_ = 0;
                host.reconnectAtMs_ = 0;
                s_svcStart = s_svcEnd = s_reportVal = 0;
                ble_gap_security_initiate(s_conn);
                static const ble_uuid16_t hidUuid = BLE_UUID16_INIT(kHidService);
                ble_gattc_disc_svc_by_uuid(s_conn, &hidUuid.u, svcCallback, nullptr);
            } else {
                host.connecting_ = false;
                host.connectFailed_ = true;
                std::snprintf(host.connectFailure_, sizeof(host.connectFailure_), "connect failed (%d)",
                              event->connect.status);
                s_conn = BLE_HS_CONN_HANDLE_NONE;
                // The HCI result is asynchronous: poll()'s synchronous-failure
                // branch never sees it, so reschedule here or only one retry
                // would ever run.
                if (host.autoReconnect_ && host.reconnectAttempts_ < BleKeyboardHost::kMaxReconnectAttempts &&
                    host.connAddr_[0] != '\0') {
                    host.reconnectAttempts_ = host.reconnectAttempts_ + 1;
                    host.reconnectAtMs_ = millis() + 4000;
                    ESP_LOGI(TAG, "connect failed; retry %u/%u in 4 s",
                             static_cast<unsigned>(host.reconnectAttempts_),
                             static_cast<unsigned>(BleKeyboardHost::kMaxReconnectAttempts));
                }
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            host.connected_ = false;
            host.connecting_ = false;
            if (host.autoReconnect_ && host.reconnectAttempts_ < BleKeyboardHost::kMaxReconnectAttempts &&
                host.connAddr_[0] != '\0') {
                host.reconnectAtMs_ = millis() + 4000;
                host.reconnectAttempts_ = host.reconnectAttempts_ + 1;
                ESP_LOGI(TAG, "link dropped; reconnect %u/%u in 4 s",
                         static_cast<unsigned>(host.reconnectAttempts_),
                         static_cast<unsigned>(BleKeyboardHost::kMaxReconnectAttempts));
            }
            return 0;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint8_t buffer[32] = {};
            const int len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > 0 && len <= static_cast<int>(sizeof(buffer))) {
                os_mbuf_copydata(event->notify_rx.om, 0, len, buffer);
                applyReport(buffer, static_cast<size_t>(len));
            }
            return 0;
        }
        case BLE_GAP_EVENT_ENC_CHANGE: {
            ESP_LOGI(TAG, "encryption change status=%d", event->enc_change.status);
            if (event->enc_change.status == 0) {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                    char addr[18];
                    addrToString(desc.peer_id_addr, addr);
                    rememberBond(host, addr, host.connName_, desc.peer_id_addr.type);
                }
            }
            return 0;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        default:
            return 0;
    }
    return 0;
}

void onReset(int reason) {
    ESP_LOGW(TAG, "host reset (reason %d)", reason);
    s_synced = false;
}

void onSync() {
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_ownAddrType) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    s_synced = true;
    ESP_LOGI(TAG, "host synced (own addr type %u)", s_ownAddrType);
}

void hostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

BleKeyboardHost& BleKeyboardHost::getInstance() {
    static BleKeyboardHost instance;
    return instance;
}

bool BleKeyboardHost::begin(const char* hostName) {
    if (begun_) return true;
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }
    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    (void)hostName;  // central-only: no GAP service / advertising name
    nimble_port_freertos_init(hostTask);
    for (int i = 0; i < 200 && !s_synced; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (!s_synced) {
        ESP_LOGE(TAG, "host did not sync");
        nimble_port_stop();
        return false;
    }
    loadBonds(*this);
    begun_ = true;
    s_host = this;
    ESP_LOGI(TAG, "ready: %u bonded device(s)", bondCount_);
    return true;
}

bool BleKeyboardHost::end(uint32_t) {
    if (!begun_) return true;
    stopScan();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    nimble_port_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    nimble_port_deinit();
    begun_ = false;
    connected_ = false;
    connecting_ = false;
    autoReconnect_ = false;
    reconnectAtMs_ = 0;
    reconnectAttempts_ = 0;
    s_synced = false;
    return true;
}

void BleKeyboardHost::poll() {
    if (reconnectAtMs_ != 0 && !connected_ && !connecting_ &&
        static_cast<int32_t>(millis() - reconnectAtMs_) >= 0) {
        reconnectAtMs_ = 0;
        if (!connect(connAddr_) && reconnectAttempts_ < kMaxReconnectAttempts) {
            reconnectAttempts_ = reconnectAttempts_ + 1;
            reconnectAtMs_ = millis() + 4000;
        }
    }
    if (scanning_ && scanDeadlineMs_ != 0 && millis() >= scanDeadlineMs_) {
        stopScan();
    }
    // HID sends one report per state change; synthesize repeats while a key is
    // held, matching the reference host (500 ms delay, then ~12 Hz).
    if (s_heldUsage != 0 && connected_ && millis() - s_heldSinceMs >= 500 &&
        millis() - s_lastRepeatMs >= 80) {
        s_lastRepeatMs = millis();
        emitUsage(s_heldUsage, 0);
    }
}

void BleKeyboardHost::startScan(uint32_t ms) {
    if (!begun_) return;
    deviceCount_ = 0;
    struct ble_gap_disc_params params = {};
    // Active scan: passive/filtered reports omit the scan response, so the
    // list showed bare addresses. The reference host scans actively as well.
    params.passive = 0;
    params.filter_duplicates = 0;
    const int rc = ble_gap_disc(s_ownAddrType, static_cast<int32_t>(ms), &params, gapEvent, this);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan failed rc=%d", rc);
        return;
    }
    scanning_ = true;
    scanDeadlineMs_ = millis() + ms;
}

void BleKeyboardHost::stopScan() {
    if (!scanning_) return;
    ble_gap_disc_cancel();
    scanning_ = false;
}

const DiscoveredDevice& BleKeyboardHost::device(uint8_t i) const {
    static DiscoveredDevice empty;
    return i < deviceCount_ ? devices_[i] : empty;
}

void BleKeyboardHost::releaseScanResults() { deviceCount_ = 0; }

bool BleKeyboardHost::connect(const char* addr) {
    if (!begun_ || scanning_ || connected_ || connecting_) return false;
    ble_addr_t peer{};
    if (!parseAddr(addr, &peer)) return false;
    for (uint8_t i = 0; i < deviceCount_; ++i) {
        if (std::strcmp(devices_[i].addr, addr) == 0) peer.type = devices_[i].addrType;
    }
    for (uint8_t i = 0; i < bondCount_; ++i) {
        if (std::strcmp(bonds_[i].addr, addr) == 0) peer.type = bonds_[i].addrType;
    }
    std::snprintf(connAddr_, sizeof(connAddr_), "%s", addr);
    const char* name = addr;
    for (uint8_t i = 0; i < deviceCount_; ++i) {
        if (std::strcmp(devices_[i].addr, addr) == 0 && devices_[i].hasName) name = devices_[i].name;
    }
    for (uint8_t i = 0; i < bondCount_; ++i) {
        if (std::strcmp(bonds_[i].addr, addr) == 0 && bonds_[i].name[0] != '\0') name = bonds_[i].name;
    }
    std::snprintf(connName_, sizeof(connName_), "%s", name);
    connectFailed_ = false;
    connecting_ = true;
    const int rc = ble_gap_connect(s_ownAddrType, &peer, 10000, nullptr, gapEvent, this);
    if (rc != 0) {
        connecting_ = false;
        connectFailed_ = true;
        std::snprintf(connectFailure_, sizeof(connectFailure_), "connect rc=%d", rc);
        return false;
    }
    return true;
}

void BleKeyboardHost::disconnect() {
    autoReconnect_ = false;
    reconnectAtMs_ = 0;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    connected_ = false;
    connecting_ = false;
}

bool BleKeyboardHost::takeConnectFailure(char* out, size_t outLen) {
    if (!connectFailed_ || out == nullptr || outLen == 0) return false;
    std::snprintf(out, outLen, "%s", connectFailure_);
    connectFailed_ = false;
    return true;
}

const PairedHidDevice& BleKeyboardHost::paired(uint8_t i) const {
    static PairedHidDevice empty;
    return i < bondCount_ ? bonds_[i] : empty;
}

void BleKeyboardHost::forget(const char* addr) {
    if (addr != nullptr && std::strcmp(addr, connAddr_) == 0) {
        autoReconnect_ = false;
        reconnectAtMs_ = 0;
    }
    uint8_t write = 0;
    for (uint8_t i = 0; i < bondCount_; ++i) {
        if (std::strcmp(bonds_[i].addr, addr) != 0) bonds_[write++] = bonds_[i];
    }
    bondCount_ = write;
    persistBonds(*this);
}

bool BleKeyboardHost::popKey(KeyEvent& out) {
    if (ringTail_ == ringHead_) return false;
    out = ring_[ringTail_];
    ringTail_ = static_cast<uint8_t>((ringTail_ + 1) % kKeyQueueLen);
    return true;
}

}  // namespace freeink
