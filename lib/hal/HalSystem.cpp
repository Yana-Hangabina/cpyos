#include "HalSystem.h"

#include <BoardConfig.h>

#include <string>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include <FaultLog.h>
#include <NewTrace.h>
#include <InvokeCallback.h>
#include "driver/temperature_sensor.h"
#include "esp_debug_helpers.h"
#include "esp_mac.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_cpu.h"
#include <exception>
#include <new>
#if !defined(SIMULATOR)
#include <cxxabi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#if !__riscv
#include <xtensa_context.h>  // XtExcFrame for the stack capture below
#endif
#if defined(CPYOS_PLUS_BOARD_PROFILE)
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#endif

#define MAX_PANIC_STACK_DEPTH 32
#define PANIC_CAPTURE_MAGIC 0x50414E49u

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];
// RTC_NOINIT is uninitialized on cold boot, so only this exact marker proves a
// panic diagnostic was captured before the reset.
RTC_NOINIT_ATTR volatile uint32_t panicCaptureMarker;
// Set by the first crash-context stamp this boot; gates reading the stamps at
// all, because RTC_NOINIT is garbage on a cold boot.
RTC_NOINIT_ATTR volatile uint32_t contextMarker = 0;

// --- crash context -----------------------------------------------------------
// Everything below exists so a *photograph of the screen* is enough to localize
// a field crash. The two 0.8.2 reports were an assert in
// `spi_flash_disable_interrupts_caches_and_other_cpu()` and
// `abort() was called at PC 0x423569f3`; neither could be pinned further because
// the capture path bailed out on PSRAM stacks (the render task!) and because
// `abort()` alone names only `__terminate`, not the exception.
RTC_NOINIT_ATTR char panicTaskName[20];
RTC_NOINIT_ATTR char panicExceptionName[32];
RTC_NOINIT_ATTR volatile uint32_t panicCoreId = 0xFF;
RTC_NOINIT_ATTR volatile uint32_t panicPc = 0;
RTC_NOINIT_ATTR volatile uint32_t panicLr = 0;
RTC_NOINIT_ATTR volatile uint32_t panicSp = 0;
RTC_NOINIT_ATTR uint32_t panicFrames[12];
RTC_NOINIT_ATTR volatile uint32_t panicFrameCount = 0;
// Live "where was I" stamps, kept separate from the panic record: they are also
// what a watchdog or lockup reset can report, when no panic hook ran at all.
RTC_NOINIT_ATTR char ctxActivity[20];
RTC_NOINIT_ATTR char ctxStage[12];
RTC_NOINIT_ATTR volatile uint32_t ctxHeapFree = 0;
RTC_NOINIT_ATTR volatile uint32_t ctxHeapLargest = 0;
RTC_NOINIT_ATTR volatile uint32_t ctxPsramFree = 0;
RTC_NOINIT_ATTR volatile uint32_t ctxUptimeMs = 0;

// --- crash history (reported, never acted on) ------------------------------------------
// Repeat field reports are the normal way a crash arrives here, and the only
// thing two of them can be compared on is the crash PC. So the last few crash
// sites and a consecutive-abnormal-boot counter are kept on-device, and
// `CMD:CRASHSTATS` plus the crash screen show them.
//
// Deliberately NOT a safe-boot trigger: the counter is a heuristic classifier
// (a deliberate restart, a brown-out and a watchdog all look alike), and a
// wrong "boot into recovery" destroys the reading position it was meant to
// protect, while a crash screen only costs the user a photo. Decided
// 2026-11-26 with the user; see docs/engineering/testing-and-debugging.md.
constexpr uint32_t kCrashHistoryDepth = 4;
static_assert(kCrashHistoryDepth == HalSystem::CrashHistory::kDepth,
              "the RTC ring and the reader-facing struct must be the same size or a report truncates");
RTC_NOINIT_ATTR volatile uint32_t crashHistoryMarker = 0;
RTC_NOINIT_ATTR volatile uint32_t crashHistoryNext = 0;
RTC_NOINIT_ATTR volatile uint32_t crashHistoryCount = 0;
RTC_NOINIT_ATTR volatile uint32_t crashHistoryPc[kCrashHistoryDepth];
RTC_NOINIT_ATTR volatile uint32_t crashHistoryUptime[kCrashHistoryDepth];
// Consecutive boots that ended in a recorded failure. Reset by a boot the
// device asked for (USB, deep sleep, software restart) with no panic record.
RTC_NOINIT_ATTR volatile uint32_t crashStreak = 0;

namespace {

// Bounded, allocation-free, NUL-terminating copy. `strncpy` is deliberately not
// used: it is neither IRAM-safe in panic context nor guaranteed to terminate.
void copyBounded(char* dst, size_t capacity, const char* src) {
  if (capacity == 0) return;
  size_t i = 0;
  if (src != nullptr) {
    for (; i + 1 < capacity && src[i] != '\0'; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}

constexpr uint32_t CRASH_HISTORY_MAGIC = 0x43485331u;  // 'CHS1'

// One record per failing boot, from whichever hook notices it first. A crash
// reaches several hooks (`panic_abort` then `panic_print_backtrace`, or the C++
// terminate handler then abort), and counting each would turn one crash into a
// streak of three -- which is precisely the number a reader is asked to trust.
// The flag is plain RAM, not RTC_NOINIT, because "this boot" is exactly what a
// reset clears.
void noteCrashSite(uint32_t pc) {
#if defined(SIMULATOR)
  // The host build runs no IDF panic hooks, and its `millis()` is a shim; an
  // empty body is honest, recording a fake timestamp would not be.
  (void)pc;
#else
  static bool stampedThisBoot = false;
  if (stampedThisBoot) return;
  stampedThisBoot = true;
  if (crashHistoryMarker != CRASH_HISTORY_MAGIC) {
    crashHistoryMarker = CRASH_HISTORY_MAGIC;
    crashHistoryNext = 0;
    crashHistoryCount = 0;
    for (uint32_t i = 0; i < kCrashHistoryDepth; ++i) {
      crashHistoryPc[i] = 0;
      crashHistoryUptime[i] = 0;
    }
    crashStreak = 0;
  }
  const uint32_t slot = crashHistoryNext % kCrashHistoryDepth;
  crashHistoryPc[slot] = pc;
  crashHistoryUptime[slot] = millis();
  crashHistoryNext = slot + 1;
  if (crashHistoryCount < kCrashHistoryDepth) ++crashHistoryCount;
  ++crashStreak;
#endif
}

}  // namespace

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';
  // Classified into the fault ring while the text is in hand. FaultLog lives
  // outside IRAM on purpose: an assert or abort reaches this hook with the
  // caches still enabled -- the same assumption the task-name lookup below
  // makes, and the reason the ring is not charged against the Wi-Fi driver's
  // internal RAM. If that assumption ever breaks, this call moves to RTC_RAM.
  fault::notePanicAbort(panicMessage);
  // Who died, on which core, and where. abort()/assert() run on the offending
  // task, so the current context is the right subject. pcTaskGetName() is not
  // itself IRAM_ATTR: that is safe here because both failure modes reach this
  // hook while the caches are still enabled (the flash guard asserts *before*
  // disabling them), which is the same assumption IDF's own panic handler makes.
  panicCoreId = static_cast<uint32_t>(esp_cpu_get_core_id());
  panicSp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(esp_cpu_get_sp()));
  panicLr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
#if !defined(SIMULATOR)
  copyBounded(panicTaskName, sizeof(panicTaskName), pcTaskGetName(nullptr));
#endif
  // abort()/assert() have no exception frame, so the best address available is
  // the return address into the caller of panic_abort. The backtrace hook below
  // replaces it with the frame PC when there is one. Recording something here
  // is what makes two reports comparable: without it the crash screen showed
  // `pc 00000000` for every abort.
  if (panicPc == 0) panicPc = panicLr;
  noteCrashSite(panicPc);
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

  // The faulting PC comes from the frame itself, which is readable even when
  // the stack window below is not, so it is taken first. C-style cast, like the
  // frame access further down this function: IDF declares `pc` as a pointer on
  // one architecture and as an integer on the other, and a static_cast of the
  // member silently mis-parses into a cast of the frame pointer.
#if __riscv
  panicPc = (uint32_t)((RvExcFrame*)frame)->pc;
#else
  panicPc = (uint32_t)((XtExcFrame*)frame)->pc;
#endif
  noteCrashSite(panicPc);

  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Stack window dump, mirroring components/esp_system/port/arch/*/panic_arch.c.
  // Hardware exceptions never reach __wrap_panic_abort, so on both
  // architectures this dump is the only diagnostic a crash leaves on-device.
#if __riscv
  const uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
#else
  const uint32_t sp = (uint32_t)((XtExcFrame*)frame)->a1;
#endif
  constexpr uint32_t captureBytes = 1024;
  // The window must be readable, not in DRAM. Requiring DRAM used to skip the
  // whole capture for the render task, whose stack is deliberately in PSRAM
  // (CFF glyph rasterisation needs 32-64 KB) -- i.e. exactly the task that
  // panics most. esp_stack_ptr_is_sane() still bounds the pointer to the
  // current task's own stack, so the read cannot run off into unrelated memory.
  if (!esp_stack_ptr_is_sane(sp) || sp > UINT32_MAX - captureBytes) {
    __real_panic_print_backtrace(frame, core);
    return;
  }
  if (panicCoreId == 0xFF) panicCoreId = static_cast<uint32_t>(core);
  panicFrameCount = 0;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < captureBytes; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      const uint32_t word = spp[y];
      panicStack[depth].spp[y] = word;
      // A return address, if it points at executable memory. These are the
      // values a photo of the crash screen can turn back into functions with
      // `addr2line`, which is the whole point of the record.
      if (panicFrameCount < sizeof(panicFrames) / sizeof(panicFrames[0]) && word != 0 &&
          esp_ptr_executable(reinterpret_cast<const void*>(word))) {
        panicFrames[panicFrameCount++] = word;
      }
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_print_backtrace(frame, core);
}
}

namespace HalSystem {

namespace {
// Snapshot taken before begin() clears or checkPanic() consumes the panic
// marker. Reader recovery routing needs a stable answer later in setup(),
// after checkPanic() has already zeroed the live marker.
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
bool bootHadPanicMarker = false;
}  // namespace

static const char* resetReasonName(esp_reset_reason_t reason);

void begin() {
  bootResetReason = esp_reset_reason();
  bootHadPanicMarker = (panicCaptureMarker == PANIC_CAPTURE_MAGIC);
  // A session the device asked to end left no evidence worth keeping; showing its
  // last activity after a normal power-on would only be a stale line.
  if (isRebootDeliberate()) contextMarker = 0;
  // The streak answers "is this the third crash in a row", so it must be broken
  // by any boot that was not a crash: a user who restarts to check, or a flash
  // or a brown-out, resets it. Only a recorded failure keeps it alive.
  if (!bootHadPanicMarker) crashStreak = 0;
  // The fault ring is cleared or named here, once per boot: a restart the device
  // asked for ends the previous session's evidence, while a watchdog or brown-out
  // -- deaths no panic hook ever sees -- can only be named from the reset reason.
  fault::onBoot(isRebootDeliberate());
  LOG_INF("SYS", "Reset reason: %s (%d)", resetReasonName(bootResetReason), static_cast<int>(bootResetReason));

  // On a panic reboot, preserve diagnostics until checkPanic() has tried to write them to the SD card.
  // Ordinary boots clear any stale retained diagnostics.
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  // An unexplained reset has no panic message but the fault ring is exactly the
  // evidence the next report needs; write it before the ring can be overwritten.
  if (isRebootFromPanic() || isRebootUnexplained()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      const size_t written = file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      if (written == panicInfo.size()) {
        // Keep the crash data for CrashActivity, but mark it consumed so a
        // later watchdog reset cannot be mistaken for this panic.
        panicCaptureMarker = 0;
        LOG_INF("SYS", "Dumped panic info to SD card");
      } else {
        LOG_ERR("SYS", "Failed to write complete crash report (%zu of %zu bytes)", written, panicInfo.size());
      }
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing; panic follows on serial");
      LOG_ERR("SYS", "%s", panicInfo.c_str());
    }
  }
}

void clearPanic() {
  panicCaptureMarker = 0;
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  panicExceptionName[0] = '\0';
  panicTaskName[0] = '\0';
  panicCoreId = 0xFF;
  panicPc = panicLr = panicSp = 0;
  panicFrameCount = 0;
  for (size_t i = 0; i < sizeof(panicFrames) / sizeof(panicFrames[0]); i++) panicFrames[i] = 0;
  clearLastLogs();
}

namespace {

// The stamps are kept out of clearPanic() on purpose: they are the only
// evidence a watchdog or CPU-lockup reset can offer, because no panic hook runs
// on those paths. They are dropped on a boot the device asked for, where the
// previous session ended cleanly and the words would only be stale.
constexpr uint32_t CONTEXT_MAGIC = 0x43545831u;  // 'CTX1'

std::terminate_handler previousTerminateHandler = nullptr;
std::new_handler previousNewHandler = nullptr;

// Shared stamping for the two C++-runtime hooks.
void stampCppFailure(const char* exceptionName) {
  copyBounded(panicExceptionName, sizeof(panicExceptionName), exceptionName);
  // The ring records the C++ cause, and remembers that it did so: the abort
  // that follows reaches __wrap_panic_abort with a generic message, and one
  // death must not be counted as two faults.
  fault::noteCppFailure(exceptionName);
  copyBounded(panicTaskName, sizeof(panicTaskName), pcTaskGetName(nullptr));
  panicCoreId = static_cast<uint32_t>(esp_cpu_get_core_id());
  panicSp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(esp_cpu_get_sp()));
  panicLr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  // Same reasoning as __wrap_panic_abort: these handlers have no exception
  // frame of their own, so the return address is the address worth keeping.
  if (panicPc == 0) panicPc = panicLr;
  noteCrashSite(panicPc);
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;
  contextMarker = CONTEXT_MAGIC;
}

void crashTerminateHandler() {
  // The exception type is the one fact that separates the three ways this build
  // can reach abort() -- an empty std::function (bad_function_call), a container
  // asked for an absurd size (length_error), a failed allocation (bad_alloc).
  // Without it every report looks identical: `abort() was called at PC
  // 0x423569f3`, that PC being __cxxabiv1::__terminate.
  const std::type_info* type = abi::__cxa_current_exception_type();
  stampCppFailure(type != nullptr && type->name() != nullptr ? type->name() : "terminate(no active exception)");
  if (previousTerminateHandler != nullptr) previousTerminateHandler();
  abort();
}

void crashNewHandler() {
  // operator new() calls the handler when the heap is gone and expects it to
  // either fix the situation or terminate; returning would loop forever.
  //
  // The size of the request is the one fact a photograph cannot otherwise carry:
  // 0.8.4 reported `PNC-04` with a PC that symbolizes to this handler, so the
  // caller stayed unknown. newtrace records the size in the wrapped operator new;
  // the heap numbers go into a separate code so neither number crowds the other.
  const std::size_t requested = newtrace::take();
  stampCppFailure("operator new failed (bad_alloc)");
  fault::noteCppFailure("operator new failed (bad_alloc)",
                        requested > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(requested));
#if !defined(SIMULATOR)
  {
    // heap_caps_* returns 0 when the pool is gone; the packing is 16 bits of KB
    // each, which covers every size this board can have.
    const uint32_t freeKb = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10);
    const uint32_t largestKb = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10);
    fault::raise(fault::Code::HeapAtAllocFailure,
                 (freeKb > 0xFFFFu ? 0xFFFFu : freeKb) | ((largestKb > 0xFFFFu ? 0xFFFFu : largestKb) << 16));
  }
#endif
  abort();
}

}  // namespace

void installCppFailureHandlers() {
  if (previousTerminateHandler == nullptr) previousTerminateHandler = std::set_terminate(crashTerminateHandler);
  if (previousNewHandler == nullptr) previousNewHandler = std::set_new_handler(crashNewHandler);
}

void noteActivityStage(const char* activityName, const char* stage) {
  copyBounded(ctxActivity, sizeof(ctxActivity), activityName);
  copyBounded(ctxStage, sizeof(ctxStage), stage);
  contextMarker = CONTEXT_MAGIC;
}

void snapshotCrashAmbience() {
  const HeapInfo heap = getHeapInfo();
  ctxHeapFree = heap.freeBytes;
  ctxHeapLargest = heap.largestFreeBlockBytes;
  ctxPsramFree = heap.freePsramBytes;
  ctxUptimeMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  contextMarker = CONTEXT_MAGIC;
}

CrashContext getCrashContext() {
  CrashContext out;
  out.hasPanicRecord = (panicCaptureMarker == PANIC_CAPTURE_MAGIC);
  if (contextMarker != CONTEXT_MAGIC && !out.hasPanicRecord) return out;

  if (out.hasPanicRecord) {
    out.task = panicTaskName[0] != '\0' ? panicTaskName : "";
    out.core = panicCoreId == 0xFF ? -1 : static_cast<int>(panicCoreId);
    out.pc = panicPc;
    out.lr = panicLr;
    out.sp = panicSp;
    out.exceptionName = panicExceptionName[0] != '\0' ? panicExceptionName : "";
    const uint32_t count = panicFrameCount;
    out.frameCount = static_cast<uint8_t>(count < sizeof(out.frames) / sizeof(out.frames[0]) ? count
                                                                                               : sizeof(out.frames) /
                                                                                                                      sizeof(out.frames[0]));
    for (uint8_t i = 0; i < out.frameCount; i++) out.frames[i] = panicFrames[i];
  }
  if (contextMarker == CONTEXT_MAGIC) {
    out.activity = ctxActivity[0] != '\0' ? ctxActivity : "";
    out.stage = ctxStage[0] != '\0' ? ctxStage : "";
    out.heapFree = ctxHeapFree;
    out.heapLargest = ctxHeapLargest;
    out.psramFree = ctxPsramFree;
    out.uptimeMs = ctxUptimeMs;
  }
  return out;
}

CrashHistory getCrashHistory() {
  CrashHistory out;
  if (crashHistoryMarker != CRASH_HISTORY_MAGIC) return out;  // nothing recorded yet
  out.streak = crashStreak;
  const uint32_t count = crashHistoryCount;
  out.count = count > kCrashHistoryDepth ? kCrashHistoryDepth : count;
  // Newest first: a reader comparing two reports reads the top line first.
  for (uint32_t i = 0; i < out.count; ++i) {
    const uint32_t slot = (crashHistoryNext + kCrashHistoryDepth - 1 - i) % kCrashHistoryDepth;
    out.pc[i] = crashHistoryPc[slot];
    out.uptimeMs[i] = crashHistoryUptime[slot];
  }
  return out;
}

const char* getDeviceModel() { return BoardConfig::ACTIVE.name; }

const char* getChipModel() { return ESP.getChipModel(); }

bool getDeviceId(DeviceId& out) {
  out.fill(0);
  if (esp_efuse_mac_get_default(out.data()) != ESP_OK) {
    LOG_ERR("SYS", "Failed to read eFuse device ID");
    return false;
  }
  return true;
}

bool getWifiStationMac(DeviceId& out) {
  out.fill(0);
  if (esp_read_mac(out.data(), ESP_MAC_WIFI_STA) != ESP_OK) {
    LOG_ERR("SYS", "Failed to read Wi-Fi station MAC address");
    return false;
  }
  return true;
}

bool getChipTemperatureCelsius(float& out) {
  out = 0.0f;
  temperature_sensor_handle_t sensor = nullptr;
  const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

  // ESP-IDF owns two short-lived allocations here (about 112 bytes of payload);
  // its opaque handle has no supported stack/static alternative.
  if (temperature_sensor_install(&config, &sensor) != ESP_OK) {
    LOG_ERR("SYS", "Failed to install chip temperature sensor");
    return false;
  }
  if (temperature_sensor_enable(sensor) != ESP_OK) {
    LOG_ERR("SYS", "Failed to enable chip temperature sensor");
    if (temperature_sensor_uninstall(sensor) != ESP_OK) {
      LOG_ERR("SYS", "Failed to uninstall chip temperature sensor after enable failure");
    }
    return false;
  }

  const bool readOk = temperature_sensor_get_celsius(sensor, &out) == ESP_OK;
  if (!readOk) LOG_ERR("SYS", "Failed to read chip temperature");

  const bool disableOk = temperature_sensor_disable(sensor) == ESP_OK;
  if (!disableOk) LOG_ERR("SYS", "Failed to disable chip temperature sensor");

  const bool uninstallOk = temperature_sensor_uninstall(sensor) == ESP_OK;
  if (!uninstallOk) LOG_ERR("SYS", "Failed to uninstall chip temperature sensor");

  return readOk && disableOk && uninstallOk;
}

uint64_t getUptimeSeconds() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000000ULL; }

HeapInfo getHeapInfo() {
  return {static_cast<uint32_t>(ESP.getFreeHeap()), static_cast<uint32_t>(ESP.getHeapSize()),
          static_cast<uint32_t>(ESP.getMaxAllocHeap()), static_cast<uint32_t>(ESP.getFreePsram()),
          static_cast<uint32_t>(ESP.getPsramSize())};
}

static const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC:
      return "PANIC (exception/abort)";
    case ESP_RST_CPU_LOCKUP:
      return "CPU_LOCKUP";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT (other)";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_EFUSE:
      return "EFUSE";
    case ESP_RST_UNKNOWN:
      return "UNKNOWN";
    default:
      return "OTHER";
  }
}

size_t getPanicMessage(char* out, size_t capacity) {
  if (out == nullptr || capacity == 0) return 0;
  const size_t length = strlen(panicMessage);
  const size_t copied = length < capacity - 1 ? length : capacity - 1;
  memcpy(out, panicMessage, copied);
  out[copied] = '\0';
  return copied;
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;
    // A lockup or hardware watchdog resets without running any panic hook, so
    // the reason and stack come back empty; the reset cause is then the only
    // way to tell those apart from a true panic.
    info += "\n\nReset reason: " + std::string(resetReasonName(esp_reset_reason()));
    info += "\n\nPanic reason: " + std::string(panicMessage);
    // The fault ring: what was going wrong before the death, in the same codes
    // the screen shows, plus one explanatory line per code so a report can be
    // read without a tool. The assert's file:line is last because it is the
    // single most actionable token in the file.
    {
      char codes[fault::kLineCapacity];
      fault::formatLine(codes, sizeof(codes), fault::kCapacity, nullptr);
      info += "\n\nFaults: ";
      info += codes[0] != '\0' ? codes : "none recorded";
      fault::Slot slots[fault::kCapacity];
      const uint8_t recorded = fault::copy(slots, fault::kCapacity);
      for (uint8_t i = 0; i < recorded; ++i) {
        char detail[160];
        fault::formatDetail(detail, sizeof(detail), slots[i]);
        info += "\n  ";
        info += detail;
      }
      for (size_t i = 0; i < callbacks::kKnownTags; ++i) {
        const char* tag = callbacks::siteTagAt(i);
        if (tag[0] == '\0') break;
        info += "\n  cb site " + std::to_string(i) + " " + tag;
      }
      if (fault::assertLocation()[0] != '\0') {
        info += "\n  assert at ";
        info += fault::assertLocation();
      }
    }
    info += "\n\nLast logs:\n" + getLastLogs();
    const CrashContext ctx = getCrashContext();
    info += "\n\nCrash context:";
    info += "\n  task=" + std::string(ctx.task) + " core=" + std::to_string(ctx.core);
    info += "\n  activity=" + std::string(ctx.activity) + " stage=" + std::string(ctx.stage);
    info += "\n  exception=" + std::string(ctx.exceptionName);
    {
      char hex[12];
      snprintf(hex, sizeof(hex), "\n  pc=%08X lr=%08X sp=%08X",
               static_cast<unsigned>(ctx.pc), static_cast<unsigned>(ctx.lr),
               static_cast<unsigned>(ctx.sp));
      info += hex;
    }
    info += "\n  heap free=" + std::to_string(ctx.heapFree) + " largest=" + std::to_string(ctx.heapLargest) +
            " psram=" + std::to_string(ctx.psramFree) + " uptime=" + std::to_string(ctx.uptimeMs) + "ms";
    info += "\n  frames:";
    for (uint8_t i = 0; i < ctx.frameCount; ++i) {
      char hex[12];
      snprintf(hex, sizeof(hex), " %08X", static_cast<unsigned>(ctx.frames[i]));
      info += hex;
    }
    // The previous failures, so a second report can be matched to the first
    // without asking anyone for a log.
    const CrashHistory history = getCrashHistory();
    info += "\n  history streak=" + std::to_string(history.streak);
    for (uint8_t i = 0; i < history.count; ++i) {
      char hex[28];
      snprintf(hex, sizeof(hex), " pc=%08X@%ums", static_cast<unsigned>(history.pc[i]),
               static_cast<unsigned>(history.uptimeMs[i]));
      info += hex;
    }
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootUnexplained() {
  const esp_reset_reason_t reason = esp_reset_reason();
  return reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
         reason == ESP_RST_BROWNOUT;
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP) {
    return true;
  }

  const bool watchdogReset =
      resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
  return watchdogReset && panicCaptureMarker == PANIC_CAPTURE_MAGIC;
}

bool supportsUsbDownloadMode() {
  // Verified only on the S31. The S3 RTC force bit and a driven GPIO0 strap
  // both rebooted SPI flash on Read Pico (boot:0x8), so do not offer a fake
  // entry there; its browser flasher already performs the USB-JTAG reset.
#if defined(CPYOS_PLUS_BOARD_PROFILE)
  return true;
#else
  return false;
#endif
}

bool rebootToUsbDownload() {
#if defined(CPYOS_PLUS_BOARD_PROFILE)
  // S31 exposes the ROM force-download bit in the LP system register block.
  // It survives the CPU reset and makes the next ROM boot stay in download
  // mode, so the host can flash without the timed USB-JTAG reset sequence.
  REG_WRITE(LP_SYSTEM_REG_SYS_CTRL_REG,
            REG_READ(LP_SYSTEM_REG_SYS_CTRL_REG) | LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
  esp_restart();
#else
  (void)0;
#endif
  return false;
}

bool isRebootDeliberate() {
  // A captured panic wins even when the chip reports the final reset as SW:
  // that state must stay recoverable, not be mistaken for a flash/OTA reset.
  if (bootHadPanicMarker) return false;

  switch (bootResetReason) {
    case ESP_RST_USB:
    case ESP_RST_JTAG:
    case ESP_RST_EXT:
    case ESP_RST_SW:
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_SDIO:
    case ESP_RST_EFUSE:
      return true;
    default:
      // POWERON, BROWNOUT, PWR_GLITCH, PANIC, CPU_LOCKUP, WDT*, UNKNOWN:
      // treat as an interruption and keep the reader recovery path.
      return false;
  }
}

}  // namespace HalSystem
