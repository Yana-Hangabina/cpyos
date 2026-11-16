#pragma once

// BLE HID host for the Metalio Plus port.
//
// Same public surface as freeink-sdk's BleKeyboardHost, implemented directly on
// IDF NimBLE (the SDK's version needs NimBLE-Arduino). It scans for HID
// peripherals, connects as central with Just Works bonding, subscribes to the
// HID Report characteristic, and turns keyboard / consumer-control reports into
// KeyEvents. Raw button edges are not produced on this port.

#include <cstddef>
#include <cstdint>

namespace freeink {

enum class SpecialKey : uint8_t {
  None = 0,
  Enter,
  Backspace,
  Tab,
  Escape,
  Delete,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  PageUp,
  PageDown,
};

struct KeyEvent {
  char ch = 0;
  uint8_t keycode = 0;
  uint8_t mods = 0;
  SpecialKey special = SpecialKey::None;
  bool pressed = true;
};

struct RawButtonEvent {
  uint8_t reportId = 0;
  uint8_t byteIndex = 0;
  uint8_t value = 0;
  bool pressed = false;
  uint8_t keycode = 0;
  uint8_t mods = 0;
  bool wasRest = false;
  uint32_t atMs = 0;
  uint32_t code() const {
    return static_cast<uint32_t>(value) | static_cast<uint32_t>(byteIndex) << 8 |
           static_cast<uint32_t>(reportId) << 16;
  }
};

struct DiscoveredDevice {
  char addr[18] = {0};
  char name[32] = {0};
  int rssi = 0;
  uint8_t addrType = 0;
  bool hasName = false;
  bool hid = false;
  bool connectable = false;
};

struct PairedHidDevice {
  char addr[18] = {0};
  char name[32] = {0};
  uint8_t addrType = 0;
};
using PairedKeyboard = PairedHidDevice;

class BleKeyboardHost {
 public:
  static constexpr uint8_t kMaxDiscovered = 24;
  static constexpr uint8_t kMaxBonds = 4;
  static constexpr uint8_t kKeyQueueLen = 16;
  static constexpr uint8_t kRawQueueLen = 17;

  static BleKeyboardHost& getInstance();

  bool begin(const char* hostName = "FreeInk");
  bool end(uint32_t timeoutMs = 1000);
  bool isStopping() const { return false; }
  void poll();
  bool isRunning() const { return begun_; }

  void startScan(uint32_t ms = 5000);
  void stopScan();
  bool isScanning() const { return scanning_; }
  uint8_t deviceCount() const { return deviceCount_; }
  const DiscoveredDevice& device(uint8_t i) const;
  void releaseScanResults();

  bool connect(const char* addr);
  bool armSelectedPeerReconnect(const char*) { return false; }
  void disconnect();
  bool isConnected() const { return connected_; }
  bool isConnecting() const { return connecting_; }
  const char* connectedName() const { return connName_; }
  const char* connectedAddr() const { return connAddr_; }
  bool takeConnectFailure(char* out, size_t outLen);
  bool takePairingPasskey(uint32_t&) { return false; }

  uint8_t pairedCount() const { return bondCount_; }
  const PairedHidDevice& paired(uint8_t i) const;
  void forget(const char* addr);

  bool popKey(KeyEvent& out);
  bool popRawButton(RawButtonEvent&) { return false; }

  // Backend hooks (kept for API compatibility; the NimBLE backend writes the
  // rings directly).
  void onScanResultIngest(const char*, const char*, int, uint8_t, bool, bool) {}
  void onReportIngest(const uint8_t*, size_t) {}
  void onLinkUp(const char*, const char*, uint8_t) {}
  void onLinkDown() {}
  void onConnectFailed(const char*) {}
  void onPairingPasskey(uint32_t) {}

  // Backend state. Public because the NimBLE callbacks in the port's .cpp fill it;
  // construction is still private (singleton).
  BleKeyboardHost() = default;
  BleKeyboardHost(const BleKeyboardHost&) = delete;
  BleKeyboardHost& operator=(const BleKeyboardHost&) = delete;

  DiscoveredDevice devices_[kMaxDiscovered];
  uint8_t deviceCount_ = 0;
  PairedHidDevice bonds_[kMaxBonds];
  uint8_t bondCount_ = 0;
  KeyEvent ring_[kKeyQueueLen];
  volatile uint8_t ringHead_ = 0;
  volatile uint8_t ringTail_ = 0;
  char connName_[32] = {0};
  char connAddr_[18] = {0};
  char connectFailure_[48] = {0};
  bool connectFailed_ = false;
  volatile bool connected_ = false;
  volatile bool connecting_ = false;
  volatile bool scanning_ = false;
  bool begun_ = false;
  uint32_t scanDeadlineMs_ = 0;
  // Reconnect the last selected pager after a link drop. Cleared by an
  // explicit disconnect/forget/end; bounded so a pager left off cannot spin.
  bool autoReconnect_ = false;
  uint32_t reconnectAtMs_ = 0;
  uint8_t reconnectAttempts_ = 0;
  static constexpr uint8_t kMaxReconnectAttempts = 5;
};

}  // namespace freeink

#define BleHid ::freeink::BleKeyboardHost::getInstance()
#define BleKbd ::freeink::BleKeyboardHost::getInstance()
