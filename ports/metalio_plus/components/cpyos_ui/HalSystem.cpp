#include "HalSystem.h"

#include <FaultLog.h>

#include <exception>
#include <new>

#include <Arduino.h>

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

namespace HalSystem {

namespace {

// The crash hooks the shared tree installs in lib/hal/HalSystem.cpp. This port
// cannot use that file (it links the Arduino wrappers and IDF panic hooks), but
// leaving the hooks uninstalled meant a `bad_alloc` or an uncaught type aborted
// with no record at all -- the 0.8.4 field report class, on the other device.
void portTerminateHandler() {
  fault::noteCppFailure("terminate");
  abort();
}

void portNewHandler() {
  fault::noteCppFailure("operator new failed (bad_alloc)");
  abort();
}

}  // namespace

void begin() {}

void installCppFailureHandlers() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  std::set_terminate(portTerminateHandler);
  std::set_new_handler(portNewHandler);
  // A panic/watchdog/brown-out boot keeps the fault ring (the shared CrashActivity
  // prints it); a deliberate reset discards the previous session's trail.
  fault::onBoot(isRebootDeliberate());
}
void restart() { esp_restart(); }
void checkPanic() {}
void clearPanic() {}
std::string getPanicInfo(bool /*full*/) { return {}; }
size_t getPanicMessage(char* out, size_t capacity) {
  if (out != nullptr && capacity > 0) out[0] = '\0';
  return 0;
}

// --- crash context, as far as this port can take it -------------------------------------
// The shared build keeps its panic record in RTC_NOINIT and hooks
// panic_abort/panic_print_backtrace through the linker wrappers in
// lib/hal/HalSystem.cpp, which this port does not use (see the empty
// checkPanic()/getPanicInfo() above: Plus has no panic reporting yet). The five
// entry points below exist because the shared main.cpp, ActivityManager.cpp and
// CrashActivity.cpp call them unconditionally; without them the port would fail
// to link, which is a build error, not a missing feature.
//
// They are deliberately *inert* rather than faked: an empty CrashContext and a
// zero-count CrashHistory make the crash screen show the version and the panic
// reason and nothing else, which is the truth. Filling `streak` or `activity`
// with defaults would put invented evidence on a screen whose whole purpose is
// to be believed. Closing the gap needs IDF panic hooks registered in this
// port's own startup; that work is open, not done here.
// Activity/stage are kept in RTC memory so the crash screen can name what was on
// screen when the device died; the shared HalSystem does the same, but this port
// owns its own copy because it does not link that file.
namespace {
RTC_NOINIT_ATTR char gActivity[24];
RTC_NOINIT_ATTR char gStage[16];
RTC_NOINIT_ATTR uint32_t gHeapFree;
RTC_NOINIT_ATTR uint32_t gHeapLargest;
RTC_NOINIT_ATTR uint32_t gPsramFree;
RTC_NOINIT_ATTR uint32_t gUptimeMs;
RTC_NOINIT_ATTR uint32_t gContextMagic;
constexpr uint32_t kContextMagic = 0x504C4358;  // 'PLCX'
}  // namespace

void noteActivityStage(const char* activityName, const char* stage) {
  if (activityName != nullptr) snprintf(gActivity, sizeof(gActivity), "%s", activityName);
  if (stage != nullptr) snprintf(gStage, sizeof(gStage), "%s", stage);
  gContextMagic = kContextMagic;
}

void snapshotCrashAmbience() {
  gHeapFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  gHeapLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  gPsramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  gUptimeMs = static_cast<uint32_t>(millis());
  if (gContextMagic != kContextMagic) gContextMagic = kContextMagic;
}

CrashContext getCrashContext() {
  CrashContext ctx;
  if (gContextMagic == kContextMagic) {
    ctx.activity = gActivity;
    ctx.stage = gStage;
    ctx.heapFree = gHeapFree;
    ctx.heapLargest = gHeapLargest;
    ctx.psramFree = gPsramFree;
    ctx.uptimeMs = gUptimeMs;
  }
  return ctx;
}

CrashHistory getCrashHistory() { return {}; }

bool isRebootFromPanic() { return esp_reset_reason() == ESP_RST_PANIC; }

bool isRebootUnexplained() {
    const esp_reset_reason_t reason = esp_reset_reason();
    return reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
           reason == ESP_RST_BROWNOUT;
}

bool isRebootDeliberate() {
    switch (esp_reset_reason()) {
        case ESP_RST_USB:
        case ESP_RST_JTAG:
        case ESP_RST_SW:
        case ESP_RST_EXT:
        case ESP_RST_DEEPSLEEP:
            return true;
        default:
            return false;
    }
}

bool supportsUsbDownloadMode() { return true; }

bool rebootToUsbDownload() {
    REG_WRITE(LP_SYSTEM_REG_SYS_CTRL_REG,
              REG_READ(LP_SYSTEM_REG_SYS_CTRL_REG) | LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    esp_restart();
    return false;
}

const char* getDeviceModel() { return "Metalio E-INK4 Plus"; }

const char* getChipModel() {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    switch (info.model) {
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32C3: return "ESP32-C3";
        default: return "ESP32-S31";
    }
}

bool getDeviceId(DeviceId& out) {
    out.fill(0);
    return esp_read_mac(out.data(), ESP_MAC_WIFI_STA) == ESP_OK;
}

bool getWifiStationMac(DeviceId& out) { return getDeviceId(out); }

bool getChipTemperatureCelsius(float& /*out*/) {
    // No temperature sensor is wired on the Plus BSP.
    return false;
}

uint64_t getUptimeSeconds() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
}

HeapInfo getHeapInfo() {
    HeapInfo info{};
    info.freeBytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
    info.totalBytes = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_8BIT));
    info.largestFreeBlockBytes = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    info.freePsramBytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    info.totalPsramBytes = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    return info;
}

}  // namespace HalSystem
