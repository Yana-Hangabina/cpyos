#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

struct HeapInfo {
  uint32_t freeBytes;
  uint32_t totalBytes;
  uint32_t largestFreeBlockBytes;
  uint32_t freePsramBytes;
  uint32_t totalPsramBytes;
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

using DeviceId = std::array<uint8_t, 6>;
const char* getDeviceModel();
const char* getChipModel();
bool getDeviceId(DeviceId& out);
bool getWifiStationMac(DeviceId& out);
bool getChipTemperatureCelsius(float& out);
uint64_t getUptimeSeconds();
HeapInfo getHeapInfo();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();

// True when this boot followed a reset that the firmware itself (or a flashing
// tool) requested: USB/JTAG resets, ESP.restart, external reset, deep-sleep
// wake, etc. Those must not be mistaken for a reader crash. Power loss,
// brownout, panic and watchdog resets return false.
bool isRebootDeliberate();

// True when this build can reboot into the SoC ROM USB download mode.
// Verified on the Metalio Plus (ESP32-S31); Read Pico/ESP32-S3 has no exposed
// strap we can drive, so it returns false and the settings row is hidden.
bool supportsUsbDownloadMode();

// Set the SoC force-download strap and reset. Does not return on success: the
// app is replaced by the ROM loader until the user flashes or power-cycles.
bool rebootToUsbDownload();
}  // namespace HalSystem
