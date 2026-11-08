#include "BleInput.h"

#include "BleRemotePolicy.h"

#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#if FREEINK_CAP_BLE_HID_HOST
#include <FontCacheManager.h>
#include <GfxRenderer.h>

#include "SdCardFontSystem.h"
#endif

#if FREEINK_CAP_BLE_HID_HOST && !defined(SIMULATOR)
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if CROSSPOINT_BLE_HOST_PSRAM
#include <esp_memory_utils.h>
#include <nimble/esp_port/port/include/esp_nimble_mem.h>
#endif
#endif

namespace bleinput {
namespace {

#if FREEINK_CAP_BLE_HID_HOST
struct MemorySnapshot {
  size_t freeInternal;
  size_t largestInternal;
  size_t minInternal;
  size_t freePsram;
  size_t largestPsram;
  size_t minPsram;
  size_t totalPsram;
};

MemorySnapshot readMemory() {
#if !defined(SIMULATOR)
  constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  constexpr uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  return {heap_caps_get_free_size(caps),
          heap_caps_get_largest_free_block(caps),
          heap_caps_get_minimum_free_size(caps),
          heap_caps_get_free_size(psramCaps),
          heap_caps_get_largest_free_block(psramCaps),
          heap_caps_get_minimum_free_size(psramCaps),
          heap_caps_get_total_size(psramCaps)};
#else
  return {};
#endif
}
#endif

const char* specialName(const uint8_t value) {
  switch (static_cast<freeink::SpecialKey>(value)) {
    case freeink::SpecialKey::Enter:
      return tr(STR_BT_KEY_ENTER);
    case freeink::SpecialKey::Backspace:
      return tr(STR_BT_KEY_BACKSPACE);
    case freeink::SpecialKey::Tab:
      return tr(STR_BT_KEY_TAB);
    case freeink::SpecialKey::Escape:
      return tr(STR_BT_KEY_ESCAPE);
    case freeink::SpecialKey::Delete:
      return tr(STR_BT_KEY_DELETE);
    case freeink::SpecialKey::Left:
      return tr(STR_DIR_LEFT);
    case freeink::SpecialKey::Right:
      return tr(STR_DIR_RIGHT);
    case freeink::SpecialKey::Up:
      return tr(STR_DIR_UP);
    case freeink::SpecialKey::Down:
      return tr(STR_DIR_DOWN);
    case freeink::SpecialKey::Home:
      return tr(STR_BT_KEY_HOME);
    case freeink::SpecialKey::End:
      return tr(STR_BT_KEY_END);
    case freeink::SpecialKey::PageUp:
      return tr(STR_BT_KEY_PAGE_UP);
    case freeink::SpecialKey::PageDown:
      return tr(STR_BT_KEY_PAGE_DOWN);
    case freeink::SpecialKey::None:
      return nullptr;
  }
  return nullptr;
}

#if FREEINK_CAP_BLE_HID_HOST
void logMemory(const char* phase, const MemorySnapshot& memory) {
#if CROSSPOINT_BLE_HOST_PSRAM
  constexpr const char* mode = "psram";
#else
  constexpr const char* mode = "internal";
#endif
  LOG_INF("BLE", "%s: mode=%s internal free/min/max=%u/%u/%u psram total/free/min/max=%u/%u/%u/%u", phase, mode,
          static_cast<unsigned>(memory.freeInternal), static_cast<unsigned>(memory.minInternal),
          static_cast<unsigned>(memory.largestInternal), static_cast<unsigned>(memory.totalPsram),
          static_cast<unsigned>(memory.freePsram), static_cast<unsigned>(memory.minPsram),
          static_cast<unsigned>(memory.largestPsram));
#if !defined(SIMULATOR)
  LOG_INF("BLE", "stack remaining: current=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#if INCLUDE_xTaskGetHandle
  for (const char* name : {"ipc0", "ipc1", "nimble_host", "ble-conn", "ActivityManagerRender"}) {
    // FreeRTOS truncates registered task names to this same fixed length.
    char taskName[configMAX_TASK_NAME_LEN];
    snprintf(taskName, sizeof(taskName), "%s", name);
    if (const auto task = xTaskGetHandle(taskName)) {
      LOG_INF("BLE", "stack remaining: %s=%u", taskName, static_cast<unsigned>(uxTaskGetStackHighWaterMark(task)));
    }
  }
#endif
#endif
}

enum class GateFailure : uint8_t { None, InternalFree, InternalBlock, Psram, HostStart };
GateFailure lastGateFailure = GateFailure::None;
StartContext lastGateContext = StartContext::Reader;

// Filled in by the first successful begin() of this boot: the HID host only loads
// its bond list from NVS while starting, so this is the earliest honest answer to
// "is a remote paired". Cleared only by a reboot.
BondState bondState;

// Reader idle-off bookkeeping (see armIdleClock()/tickIdle()). Millis-based, so it
// is reset by every start and by every reader entry rather than carried across.
uint32_t idleSinceMs = 0;
bool idleClockRunning = false;
bool idleStopLatched = false;

GateFailure gateFailure(const MemorySnapshot& memory, const StartContext context) {
  const size_t minFree = context == StartContext::Reader ? kReaderMinFreeInternal : kExplicitMinFreeInternal;
  const size_t minLargest = context == StartContext::Reader ? kReaderMinLargestInternal : kExplicitMinLargestInternal;
  if (memory.freeInternal < minFree) return GateFailure::InternalFree;
  if (memory.largestInternal < minLargest) return GateFailure::InternalBlock;
#if CROSSPOINT_BLE_HOST_PSRAM && !defined(SIMULATOR)
  if (memory.freePsram < kMinFreePsram || memory.largestPsram < kMinLargestPsram) {
    return GateFailure::Psram;
  }
#endif
  return GateFailure::None;
}

void reportStartFailure(const GateFailure failure, const StartContext context, const MemorySnapshot& before,
                        const MemorySnapshot& after) {
  if (failure == lastGateFailure && context == lastGateContext) return;
  lastGateFailure = failure;
  lastGateContext = context;
  logMemory("before start", before);
  logMemory("start rejected", after);
  LOG_ERR(
      "BLE", "Start rejected: reason=%u required internal=%u/%u", static_cast<unsigned>(failure),
      static_cast<unsigned>(context == StartContext::Reader ? kReaderMinFreeInternal : kExplicitMinFreeInternal),
      static_cast<unsigned>(context == StartContext::Reader ? kReaderMinLargestInternal : kExplicitMinLargestInternal));
#if CROSSPOINT_BLE_HOST_PSRAM
  if (failure == GateFailure::Psram) {
    LOG_ERR("BLE", "Required PSRAM=%u/%u", static_cast<unsigned>(kMinFreePsram),
            static_cast<unsigned>(kMinLargestPsram));
  }
#endif
}

#endif

}  // namespace

void logDiagnostics(const char* phase) {
#if FREEINK_CAP_BLE_HID_HOST
  logMemory(phase, readMemory());
#else
  (void)phase;
#endif
}

StartResult ensureStarted(GfxRenderer& renderer, const StartContext context) {
#if !FREEINK_CAP_BLE_HID_HOST
  (void)renderer;
  (void)context;
  return StartResult::Unavailable;
#else
  if (BleHid.isRunning()) return StartResult::AlreadyRunning;
  // A teardown that did not finish keeps begin() refused inside the host; report it
  // as its own state so the caller retries instead of counting a refusal (which would
  // also have spent a memory-gate pass and a log line on it).
  if (BleHid.isStopping()) return StartResult::Stopping;

  const auto before = readMemory();
  auto memory = before;
  if (gateFailure(memory, context) != GateFailure::None) {
    switch (context) {
      case StartContext::Reader:
        // Keep reading fonts registered; only discard rebuildable caches.
        if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
        break;
      case StartContext::Explicit:
        sdFontSystem.releaseLoadedFont(renderer);
        if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
        break;
    }
    memory = readMemory();
  }
  const auto failure = gateFailure(memory, context);
  if (failure != GateFailure::None) {
    reportStartFailure(failure, context, before, memory);
    return StartResult::LowMemory;
  }

#if CROSSPOINT_BLE_HOST_PSRAM && !defined(SIMULATOR)
  // A stack buffer cannot verify the linked NimBLE allocator. Its C API owns
  // this 16-byte diagnostic allocation, which is released before starting BLE.
  void* probe = nimble_platform_mem_malloc(16);
  if (!probe) {
    LOG_ERR("BLE", "NimBLE allocator probe failed (16 bytes)");
    return StartResult::Failed;
  }
  const bool external = esp_ptr_external_ram(probe);
  nimble_platform_mem_free(probe);
  if (!external) {
    LOG_ERR("BLE", "NimBLE allocator probe returned internal RAM; refusing start");
    return StartResult::Failed;
  }
  LOG_INF("BLE", "NimBLE allocator probe: PSRAM verified (16 bytes, released)");
#endif

  HalPowerManager::Lock powerLock;
  if (!BleHid.begin("CrossMux")) {
    reportStartFailure(GateFailure::HostStart, context, before, readMemory());
    return StartResult::Failed;
  }
  // The host has now read its bond list: from here on the reader knows whether
  // there is a remote worth keeping the radio up for (see readerRadioAllowed()).
  bondState.known = true;
  bondState.hasRemote = BleHid.pairedCount() > 0;
  // A fresh radio gets a fresh idle window and must be able to be stopped by idleness
  // again, even if the previous visit ended that way.
  idleClockRunning = false;
  idleStopLatched = false;
  LOG_INF("BLE", "bonded remotes: %u", static_cast<unsigned>(BleHid.pairedCount()));
  lastGateFailure = GateFailure::None;
  LOG_INF("BLE", "BLE HID host started");
  logDiagnostics("after start");
  return StartResult::Started;
#endif
}

void stop() {
#if FREEINK_CAP_BLE_HID_HOST
  // Any stop ends the idle episode: a later start (settings page, reconnect) must not
  // inherit "stopped for idleness" from the visit that ended.
  idleClockRunning = false;
  idleStopLatched = false;
  // A failed end() leaves the host "stopping" with the stack half down; a later end()
  // carries on from there, so a stop request must not be dropped just because the host
  // no longer reports itself running.
  if (!BleHid.isRunning() && !BleHid.isStopping()) return;
  HalPowerManager::Lock powerLock;
  logDiagnostics("before stop");
  BleHid.end();
  logDiagnostics("after stop");
#endif
}

void armIdleClock() {
#if FREEINK_CAP_BLE_HID_HOST
  idleClockRunning = false;
  idleStopLatched = false;
#endif
}

bool tickIdle() {
#if !FREEINK_CAP_BLE_HID_HOST
  return false;
#else
  if (!BleHid.isRunning()) {
    idleClockRunning = false;
    return false;
  }
  if (idleStopLatched) return false;

  const uint32_t now = static_cast<uint32_t>(millis());
  if (!idleClockRunning) {
    idleSinceMs = now;
    idleClockRunning = true;
    return false;
  }
  // A linked remote sits quiet between two presses for a long time; a scan or a
  // connect attempt is activity in its own right. Both restart the window, so a peer
  // that is being discovered right now is not cut off at the boundary.
  const bool linked = BleHid.isConnected();
  const bool busy = BleHid.isConnecting() || BleHid.isScanning();
  if (linked || busy) {
    idleSinceMs = now;
    return false;
  }
  const uint32_t idleMs = now - idleSinceMs;
  if (!radioIdleExpired(linked, busy, idleMs)) return false;

  LOG_INF("BLE", "radio idle for %ums with no link; stopping", static_cast<unsigned>(idleMs));
  stop();
  // stop() cleared the flag; this visit must not restart the radio on its own.
  idleStopLatched = true;
  return true;
#endif
}

bool idleStopped() {
#if FREEINK_CAP_BLE_HID_HOST
  return idleStopLatched;
#else
  return false;
#endif
}

bool readerRadioAllowed() {
#if FREEINK_CAP_BLE_HID_HOST
  if (BleHid.isRunning()) {
    // The host is up, so its bond list is current: pick up a remote paired (or
    // forgotten) since it started instead of waiting for the next reboot.
    bondState.known = true;
    bondState.hasRemote = BleHid.pairedCount() > 0;
  }
  return readerRadioAllowedFor(bondState);
#else
  return true;  // no BLE in this build: nothing to forbid
#endif
}

bool encodeKey(const freeink::KeyEvent& event, uint8_t& kind, uint8_t& value) {
  if (event.special != freeink::SpecialKey::None) {
    kind = 0;
    value = static_cast<uint8_t>(event.special);
    return true;
  }
  if (event.keycode == 0) return false;
  kind = 1;
  value = event.keycode;
  return true;
}

void describeKey(const uint8_t kind, const uint8_t value, char* out, const size_t outLen) {
  if (!out || outLen == 0) return;
  if (kind == 0) {
    if (const char* name = specialName(value)) {
      strncpy(out, name, outLen - 1);
      out[outLen - 1] = '\0';
      return;
    }
  }
  snprintf(out, outLen, tr(STR_BT_KEY_CODE), static_cast<unsigned>(value));
}

}  // namespace bleinput
