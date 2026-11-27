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
// Allocation-free panic message for the crash screen (mirrors the shared
// header). Plus installs no panic hooks, so there is nothing to copy.
size_t getPanicMessage(char* out, size_t capacity);
bool isRebootFromPanic();

// True when this boot followed a reset that the firmware itself (or a flashing
// tool) requested: USB/JTAG resets, ESP.restart, external reset, deep-sleep
// wake, etc. Those must not be mistaken for a reader crash. Power loss,
// brownout, panic and watchdog resets return false.
bool isRebootDeliberate();
bool isRebootUnexplained();

bool supportsUsbDownloadMode();
bool rebootToUsbDownload();

// --- crash context ---------------------------------------------------------------------
// Mirrors lib/hal/HalSystem.h, which is the source of truth: the shared
// main.cpp, ActivityManager.cpp and CrashActivity.cpp call these unconditionally,
// so a compat header that omits them is a compile error in the portable tree
// (the same trap as HalDisplay's getControllerName()). The port implements them
// inertly -- Plus installs no panic hooks yet -- and an empty record is the
// truthful answer, not a default-filled one.
struct CrashContext {
  bool hasPanicRecord = false;
  const char* task = "";
  int core = -1;
  uint32_t pc = 0;
  uint32_t lr = 0;
  uint32_t sp = 0;
  const char* exceptionName = "";
  const char* activity = "";
  const char* stage = "";
  uint32_t frames[12] = {};
  uint8_t frameCount = 0;
  uint32_t heapFree = 0;
  uint32_t heapLargest = 0;
  uint32_t psramFree = 0;
  uint32_t uptimeMs = 0;
};

CrashContext getCrashContext();

struct CrashHistory {
  static constexpr uint8_t kDepth = 4;
  uint32_t streak = 0;
  uint8_t count = 0;
  uint32_t pc[kDepth] = {};
  uint32_t uptimeMs[kDepth] = {};
  bool repeats() const { return count > 1 && pc[0] != 0 && pc[0] == pc[1]; }
};

CrashHistory getCrashHistory();

void noteActivityStage(const char* activityName, const char* stage);
void snapshotCrashAmbience();
void installCppFailureHandlers();
}  // namespace HalSystem
