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

// ---------------------------------------------------------------------------
// Crash context: what a field report can carry without a serial console.
//
// A user can photograph the crash screen but cannot attach a backtrace, and
// until now the screen showed only the panic *sentence*. Two facts made that
// useless for the two crash classes reported against 0.8.2:
//   * `__wrap_panic_print_backtrace()` skipped the capture whenever the stack
//     was not in DRAM -- and the render task's stack is in PSRAM, so a panic
//     there left nothing on the device at all;
//   * `abort()` from `std::terminate` reports only a PC, which is
//     `__cxxabiv1::__terminate` and therefore says nothing about *which*
//     exception was thrown or where.
// So the record now carries the throwing task, its core, the live activity and
// lifecycle stage, a heap snapshot, decodable return addresses, and the
// exception type name captured by the terminate/new handlers.
struct CrashContext {
  bool hasPanicRecord = false;   // the marker proves this boot's record is real
  const char* task = "";         // owning task of the panic
  int core = -1;
  uint32_t pc = 0;
  uint32_t lr = 0;
  uint32_t sp = 0;
  const char* exceptionName = "";  // mangled libstdc++ name, "" when not a C++ throw
  const char* activity = "";       // the screen that was current when it died
  const char* stage = "";          // enter / loop / render / exit
  uint32_t frames[12] = {};        // words from the stack that are executable addresses
  uint8_t frameCount = 0;
  uint32_t heapFree = 0;
  uint32_t heapLargest = 0;
  uint32_t psramFree = 0;
  uint32_t uptimeMs = 0;
};

CrashContext getCrashContext();

// The last few failing boots, most recent first. `streak` counts consecutive
// boots that ended in a recorded failure and is cleared by any boot the device
// asked for (restart, flash, deep-sleep wake). It is reported, never acted on:
// a wrong "safe boot" can destroy reading progress, while a crash screen only
// costs the user a photo. See docs/engineering/testing-and-debugging.md.
struct CrashHistory {
  static constexpr uint8_t kDepth = 4;
  uint32_t streak = 0;
  uint8_t count = 0;
  uint32_t pc[kDepth] = {};
  uint32_t uptimeMs[kDepth] = {};
  // True when the two newest records are the same address, which is how a
  // repeat report is recognised without asking the user for anything.
  bool repeats() const { return count > 1 && pc[0] != 0 && pc[0] == pc[1]; }
};

CrashHistory getCrashHistory();

// Stamped by ActivityManager around each lifecycle step. Copies short strings
// rather than keeping pointers: the Activity can be destroyed before the crash
// is read back.
void noteActivityStage(const char* activityName, const char* stage);

// Stamped once per second from the main loop. Reading the heap here costs a
// mutex, so it must not move onto the render task; the panic path only reads
// the last snapshot.
void snapshotCrashAmbience();

// Installs the terminate/new handlers. Idempotent; call early in setup().
void installCppFailureHandlers();

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
