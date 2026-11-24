#include "NetworkStartup.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalMemory.h>
#include <Logging.h>
#include <WiFi.h>

#if !defined(SIMULATOR)
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <esp_memory_utils.h>
#include <esp_wifi.h>
#include <sdkconfig.h>
#endif

#include <atomic>

#include "BackgroundWork.h"
#include "BleInput.h"
#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/RenderLock.h"

#if defined(CROSSPOINT_PLUS_PORT)
// Port-only: the cellular link aliases WIFI_STA in the Arduino compat layer, so
// releaseRadio() must be able to tell the modem apart from a real station.
#include <PlusCellular.h>
// Port-only: sound feedback warms the BH1098 I2S path at boot, whose DMA
// descriptors sit in the same internal pool esp_wifi_init needs.
#include <plus_audio.h>
#endif

namespace {

constexpr size_t MIN_FREE_PSRAM = 256 * 1024;
constexpr size_t MIN_FREE_INTERNAL = 64 * 1024;
constexpr size_t MIN_LARGEST_INTERNAL_BLOCK = 32 * 1024;

struct MemorySnapshot {
  size_t freePsram;
  size_t freeInternal;
  size_t largestInternalBlock;
};

constexpr bool shouldReleaseRenderMemory(const MemorySnapshot& memory) {
  return memory.freePsram < MIN_FREE_PSRAM || memory.freeInternal < MIN_FREE_INTERNAL ||
         memory.largestInternalBlock < MIN_LARGEST_INTERNAL_BLOCK;
}

static_assert(shouldReleaseRenderMemory({0, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK}));
static_assert(shouldReleaseRenderMemory({MIN_FREE_PSRAM - 1, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK}));
static_assert(shouldReleaseRenderMemory({MIN_FREE_PSRAM, MIN_FREE_INTERNAL - 1, MIN_LARGEST_INTERNAL_BLOCK}));
static_assert(shouldReleaseRenderMemory({MIN_FREE_PSRAM, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK - 1}));
static_assert(!shouldReleaseRenderMemory({MIN_FREE_PSRAM, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK}));

MemorySnapshot readMemorySnapshot() {
#if defined(SIMULATOR)
  return {MIN_FREE_PSRAM, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK};
#elif defined(BOARD_HAS_PSRAM) && defined(CONFIG_SPIRAM_USE_MALLOC) && CONFIG_SPIRAM_USE_MALLOC
  constexpr uint32_t INTERNAL_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return {heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_free_size(INTERNAL_CAPS),
          heap_caps_get_largest_free_block(INTERNAL_CAPS)};
#else
  return {};
#endif
}

}  // namespace

namespace NetworkStartup {

void logMemory(const char* stage) {
  const auto internal = HalMemory::getInternalHeap();
#if defined(SIMULATOR)
  const HalMemory::HeapStats dma{};  // No hardware DMA heap in the desktop simulator.
#else
  const auto dma = HalMemory::getInternalDmaHeap();
#endif
  const auto psram = HalMemory::getPsramHeap();
  LOG_INF("NET", "%s: internal free=%u largest=%u, DMA free=%u largest=%u, PSRAM free=%u", stage,
          static_cast<unsigned>(internal.freeBytes), static_cast<unsigned>(internal.largestBlockBytes),
          static_cast<unsigned>(dma.freeBytes), static_cast<unsigned>(dma.largestBlockBytes),
          static_cast<unsigned>(psram.freeBytes));
}

void prepare(GfxRenderer& renderer) {
#if defined(CROSSPOINT_PLUS_PORT)
  // The S31 panel feeder queues and the audio I2S DMA pool share internal RAM;
  // with the full shell the Wi-Fi driver cannot allocate unless the audio
  // channels are gone. A later cue re-creates them on demand.
  plus_audio_release_i2s();
#endif
  logMemory("before font reclaim");
  const MemorySnapshot before = readMemorySnapshot();
  if (!shouldReleaseRenderMemory(before)) return;

  {
    RenderLock lock;
    sdFontSystem.releaseLoadedFont(renderer);
    if (auto* fontCache = renderer.getFontCacheManager()) {
      // clearCache() only drops glyph references; the SD font arenas and the
      // decompressor's resident buffers stay allocated. Wi-Fi init needs
      // internal/DMA RAM, so release those too (the same call the browser
      // makes before its first handshake).
      fontCache->clearCache();
      fontCache->releaseSdFontCaches();
    }
  }
  logMemory("after font reclaim");
}

namespace {
int g_timeSyncPauseDepth = 0;
std::atomic<int> g_networkHolds{0};
std::atomic<uint32_t> g_lastNetworkActivityMs{0};
std::atomic<bool> g_idleReleaseEnabled{false};
std::atomic<uint32_t> g_psCheckMs{0};
std::atomic<bool> g_psVerified{false};

// Modem sleep is a policy, not a one-shot request.
//
// With PS on, IDF gates PHY power on/off around beacon sleep and that path calls
// `phy_track_pll_init()`, whose `esp_timer_create()` goes through
// `ESP_ERROR_CHECK` -- it aborts the machine when the internal heap cannot satisfy
// the allocation (measured on device: largest free block 820 B, `phy_common.c:118`,
// `ESP_ERR_NO_MEM`).  The timer is created at a moment chosen by the power manager,
// so no call site can guard it.
//
// Asking once at `WiFi.mode()` is not enough and the device proved it:
// `esp_wifi_set_ps()` is rejected before the driver is started, and Arduino's own
// connect path plus `WifiPowerSaveGuard`'s restore both write MIN_MODEM back later.
// So re-assert here, on the idle path that already owns radio policy, and treat "I
// asked for it once" as unverified.
void enforceNoModemSleep(const uint32_t nowMs) {
#if !defined(SIMULATOR)
  const uint32_t last = g_psCheckMs.load(std::memory_order_relaxed);
  if (static_cast<int32_t>(nowMs - last) < 1000) return;  // one driver call per second
  g_psCheckMs.store(nowMs, std::memory_order_relaxed);
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
  if (esp_wifi_get_ps(&ps) != ESP_OK) return;  // driver not up yet; try again next tick
  if (ps == WIFI_PS_NONE) {
    // Say it once per radio session: "no error line" is not evidence that a
    // mitigation is in force, and this one had already failed silently once.
    if (!g_psVerified.exchange(true, std::memory_order_relaxed))
      LOG_INF("NET", "modem sleep is off (verified on the idle path)");
    return;
  }
  g_psVerified.store(false, std::memory_order_relaxed);
  if (esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) {
    LOG_INF("NET", "modem sleep forced off (was %d)", static_cast<int>(ps));
  } else if (WiFi.status() == WL_CONNECTED) {
    LOG_ERR("NET", "cannot disable modem sleep; the PHY PLL timer abort stays possible");
  }
#endif
}
}  // namespace

void noteActivity() {
  g_lastNetworkActivityMs.store(millis(), std::memory_order_relaxed);
  // Idle auto-release stays off: every teardown variant tried so far panics.
  // g_idleReleaseEnabled.store(true, std::memory_order_relaxed);
}

void hold() {
  g_networkHolds.fetch_add(1, std::memory_order_relaxed);
  noteActivity();
}

void releaseHold() {
  int count = g_networkHolds.load(std::memory_order_relaxed);
  while (count > 0 && !g_networkHolds.compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) {
  }
  // Start the quiet period at the end of the operation, not at its start.
  noteActivity();
}

void pollIdle(const uint32_t nowMs) {
  enforceNoModemSleep(nowMs);
  if (!g_idleReleaseEnabled.load(std::memory_order_relaxed)) return;
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    g_idleReleaseEnabled.store(false, std::memory_order_relaxed);
    return;
  }
  // A web server / AP session owns the radio; never idle it out from under it.
  if ((WiFi.getMode() & WIFI_MODE_AP) != 0) return;
  if (g_networkHolds.load(std::memory_order_relaxed) > 0 || background_work::active()) return;
  const uint8_t timeoutSeconds = SETTINGS.wifiIdleTimeoutSeconds;
  if (timeoutSeconds == 0) return;  // 0 = never auto-release
  const uint32_t last = g_lastNetworkActivityMs.load(std::memory_order_relaxed);
  const int32_t elapsed = static_cast<int32_t>(nowMs - last);
  if (elapsed < static_cast<int32_t>(timeoutSeconds) * 1000) return;
  LOG_INF("NET", "WiFi idle %u s; releasing radio", static_cast<unsigned>(timeoutSeconds));
  releaseRadio();
}

void suspendTimeSync() {
  if (g_timeSyncPauseDepth++ != 0) return;
  halClock.setAutoSyncEnabled(false);
#if !defined(SIMULATOR)
  if (esp_sntp_enabled()) esp_sntp_stop();
#endif
}

void resumeTimeSync() {
  if (g_timeSyncPauseDepth == 0) return;
  if (--g_timeSyncPauseDepth != 0) return;
  halClock.setAutoSyncEnabled(SETTINGS.clockAutoSync != 0);
}

void releaseRadio() {
  g_idleReleaseEnabled.store(false, std::memory_order_relaxed);
  // New radio session: the next verified tick has to say so again.
  g_psVerified.store(false, std::memory_order_relaxed);
  g_psCheckMs.store(0, std::memory_order_relaxed);
  g_lastNetworkActivityMs.store(millis(), std::memory_order_relaxed);
#if !defined(SIMULATOR)
#if defined(CROSSPOINT_PLUS_PORT)
  // 4G: the compat layer reports the modem link as WIFI_STA (so the Arduino API
  // behaves), which means the teardown below would stop the data path the caller
  // is about to use. An immediate "release on exit" would silently kill the
  // modem on exit. The modem is the configured transport and the boot path
  // starts it, so releasing the radio leaves it up.
  if (plus_cellular_active()) {
    LOG_INF("NET", "releaseRadio: cellular link active, keeping the modem");
    return;
  }
#endif
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  LOG_INF("NET", "releasing radio (mode=%d)", static_cast<int>(WiFi.getMode()));
  // Drop the association only.  Both esp_wifi_deinit() and WiFi.mode(WIFI_OFF)
  // can tear down state that lwIP still references, and the failure is a panic
  // inside tcpip_thread (`assert xQueueSemaphoreTake ((pxQueue))`, frames
  // sys_mutex_lock <- tcpip_thread, reproduced 2026-11-20 with each variant).
  // Staying associated-but-idle costs power; a dead network stack costs the
  // session, so this stays conservative until a real drain condition exists.
  // Without this the station re-associates within a second (Arduino's
  // auto-reconnect), so the "release" measured status=6 forever.  setMode()
  // turns it back on for the next feature that needs the network.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  LOG_INF("NET", "radio released (association dropped, stack intact), mode=%d",
          static_cast<int>(WiFi.getMode()));
  // The radio session ends here, so the SNTP hold that setMode() took can go.
  resumeTimeSync();
#endif
}

static bool gDeepReleasePending = false;

void requestDeepRelease() { gDeepReleasePending = true; }

bool flushDeepRelease() {
#if defined(SIMULATOR)
  gDeepReleasePending = false;
  return false;
#else
  if (!gDeepReleasePending) return false;
  if (esp_ptr_external_ram(esp_cpu_get_sp())) return false;
  gDeepReleasePending = false;
  return releaseRadioDeep();
#endif
}

bool releaseRadioDeep() {
#if defined(SIMULATOR)
  return false;
#else
  const uint32_t beforeFree =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t beforeLargest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (WiFi.getMode() == WIFI_MODE_NULL) return true;
  g_idleReleaseEnabled.store(false, std::memory_order_relaxed);
  g_psVerified.store(false, std::memory_order_relaxed);
  g_psCheckMs.store(0, std::memory_order_relaxed);
  // Order matters: disconnect drops sockets/DHCP first, mode(WIFI_OFF) stops and
  // deinits the driver and releases the netif, and the explicit esp_wifi_deinit()
  // covers the case where mode() short-circuits because it already believed off.
  // ★ This is a diagnostic/bench path now: the PDF reader stopped using it after
  // a reproducible `Cache error` panic (2026-11-22) and went back to
  // releaseRadio().  It remains callable from tasks whose stack is in PSRAM
  // (CMD:WIFIDEINIT runs it from the main loop; the storage switch below exists
  // because onEnter used to call it from the render task).  Internal-flash
  // writes from a PSRAM stack trip
  // IDF's cache guard (`esp_task_stack_is_sane_cache_disabled`), and Arduino's
  // WiFi.mode() persists the mode to NVS while storage is FLASH.  Switching the
  // driver to RAM storage first keeps this teardown entirely off internal flash;
  // the mode is re-established by whatever network feature runs next anyway.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  WiFi.disconnect(false, false);  // association only; wifioff=true would re-enter mode()
  WiFi.mode(WIFI_OFF);
  esp_wifi_deinit();
  // Put the driver back on flash storage: RAM storage applies to EVERY later
  // config write, so leaving it on would silently stop new Wi-Fi credentials
  // from being saved for the rest of the session (a connected-function bug that
  // would only show up the next boot).  Setting it before the next init is fine.
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  if (esp_ptr_external_ram(esp_cpu_get_sp())) {
    LOG_INF("NET", "deep radio release ran on a PSRAM stack; storage was RAM, no flash write");
  }
  resumeTimeSync();
  const uint32_t afterFree =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t afterLargest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  LOG_INF("NET", "deep radio release: mode=%d free %u->%u largest %u->%u",
          static_cast<int>(WiFi.getMode()), static_cast<unsigned>(beforeFree),
          static_cast<unsigned>(afterFree), static_cast<unsigned>(beforeLargest),
          static_cast<unsigned>(afterLargest));
  return true;
#endif
}

bool canAffiliate(const bool verbose) {
#if defined(SIMULATOR)
  (void)verbose;
  return true;  // no esp heap caps under the native stub
#else
  // What this is measured against, and what it is not:
  //   * granted, and the association then succeeded, at internal free=22,511 with
  //     largest=7,156 and DMA largest=5,620 (2026-11-20, after four PDF open/close
  //     cycles in one session);
  //   * `esp_wifi_init` returned ESP_ERR_NO_MEM at largest=7,668 while the reader
  //     still held its page caches -- a *larger* largest block than the success case.
  // So largest-free-block does not predict this, and every threshold built on it has
  // been wrong twice, including one that refused a download whose radio was already
  // connected. What the driver needs is total internal room for many small
  // internal/DMA allocations, so that is what is measured. 12 KiB sits below every
  // observed success and above the state where the driver had nothing to work with;
  // it is an interpolation, not a boundary, and the driver's own error stays the
  // authority -- a refused attempt is recoverable, a wrong guess here is only a
  // slower "can't connect".
  constexpr size_t kMinInternalFree = 12 * 1024;
  const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeInternal >= kMinInternalFree) return true;
  if (verbose) {
    LOG_ERR("NET", "too little internal RAM for the radio: free=%u B (want %u B), largest=%u B, "
                   "DMA largest=%u B",
            static_cast<unsigned>(freeInternal), static_cast<unsigned>(kMinInternalFree),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
  }
  return false;
#endif
}

bool setMode(GfxRenderer& renderer, const wifi_mode_t mode) {
  bleinput::stop();
  logMemory("after BLE stop");
  prepare(renderer);
  // After prepare(): the reclaim above is frequently what makes room for the driver,
  // so deciding before it refuses work that would have succeeded.
  if ((mode & WIFI_MODE_STA) != 0 && !canAffiliate()) {
    logMemory("radio refused");
    return false;
  }
  // A pending SNTP DNS callback is what makes lwIP assert when association
  // clears the DNS cache; hold the clock off for the whole radio session.
  const bool wasOff = WiFi.getMode() == WIFI_MODE_NULL;
  if (wasOff) suspendTimeSync();
  WiFi.setAutoReconnect(true);  // releaseRadio() disables it; every session re-enables
  const bool started = WiFi.mode(mode);
  if (started && (mode & WIFI_MODE_STA) != 0) {
    // Kill the modem-sleep path before it can kill the device: with PS enabled,
    // IDF toggles PHY power gating and calls phy_track_pll_init(), which creates
    // its timer through ESP_ERROR_CHECK -- at a time chosen by the power manager,
    // not by us.  Measured on device: that landed inside a PDF session holding
    // 820 B of largest internal block and aborted the whole machine
    // (`ESP_ERR_NO_MEM ... phy_common.c:118`).  Keeping PS off removes the code
    // path entirely.  This call is only a head start -- it is rejected while the
    // driver is still down, which is why enforceNoModemSleep() re-asserts it on the
    // idle path.
    WiFi.setSleep(false);
  }
  if (!started && wasOff) resumeTimeSync();
  if (started) noteActivity();
  logMemory(started ? "WiFi mode ready" : "WiFi mode failed");
  return started;
}

}  // namespace NetworkStartup
