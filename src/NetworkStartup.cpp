#include "NetworkStartup.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalMemory.h>
#include <Logging.h>
#include <WiFi.h>

#if !defined(SIMULATOR)
#include <Arduino.h>  // delay(), used by the gate's bounded waits and the teardown
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <esp_memory_utils.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>
#endif

#include <atomic>
#include <cassert>

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
// Registered at boot by the app entry point so non-Activity callers can reach
// setMode() with the render-memory reclaim intact. May legitimately stay null
// (simulator, ports without the shared boot path).
GfxRenderer* g_registeredRenderer = nullptr;

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

// ---------------------------------------------------------------------------
// Radio ownership -- see the block comment in NetworkStartup.h.
//
// The mutex is created at static-init time the way HalStorage creates its own,
// so no path can reach a radio command before it exists and nothing has to
// remember to initialise it.
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kRadioCommandWaitMs = 1500;

struct RadioGate {
#if !defined(SIMULATOR)
  SemaphoreHandle_t mutex = nullptr;
#endif
  // Counted inside the mutex and read without it: the question is "is a command
  // executing right now", and a stale yes only costs one extra poll.
  std::atomic<int> commandDepth{0};
  std::atomic<int> stationClaims{0};
  RadioGate() {
#if !defined(SIMULATOR)
    mutex = xSemaphoreCreateRecursiveMutex();
    assert(mutex != nullptr);
#endif
  }
};

RadioGate g_gate;

// One driver command. Held far shorter than any wait a caller could mistake for
// a hang, and never across prepare()/RenderLock -- that ordering is what would
// make this lock deadlocked instead of merely contended.
class CommandLock {
 public:
  explicit CommandLock(const uint32_t timeoutMs = kRadioCommandWaitMs) {
#if !defined(SIMULATOR)
    if (g_gate.mutex == nullptr) return;
    taken_ = xSemaphoreTakeRecursive(g_gate.mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    if (!taken_) {
      // A command holding the gate this long did not return. That is not slow
      // networking and must not be reported as it; proceeding anyway is the
      // lesser evil, because refusing here would turn one wedged caller into a
      // device that can never touch the radio again.
      LOG_ERR("NET", "radio command gate held over %lu ms; proceeding anyway",
              static_cast<unsigned long>(timeoutMs));
      return;
    }
    ++g_gate.commandDepth;
#else
    (void)timeoutMs;
#endif
  }
  CommandLock(const CommandLock&) = delete;
  CommandLock& operator=(const CommandLock&) = delete;
  ~CommandLock() {
#if !defined(SIMULATOR)
    if (!taken_) return;
    --g_gate.commandDepth;
    xSemaphoreGiveRecursive(g_gate.mutex);
#endif
  }

 private:
#if !defined(SIMULATOR)
  bool taken_ = false;
#endif
};

}  // namespace

// ---------------------------------------------------------------------------
// Radio event clock -- see the block comment in NetworkStartup.h.
// ---------------------------------------------------------------------------
namespace {

#if !defined(SIMULATOR) && !defined(CROSSPOINT_PLUS_PORT)
std::atomic<uint32_t> g_staDisconnectEvents{0};
std::atomic<uint8_t> g_lastDisconnectReason{0};

void ensureRadioEventClock() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      const uint8_t reason = static_cast<uint8_t>(info.wifi_sta_disconnected.reason);
      g_lastDisconnectReason.store(reason, std::memory_order_relaxed);
      g_staDisconnectEvents.fetch_add(1, std::memory_order_relaxed);
      LOG_DBG("NET", "station disconnected reason=%u events=%u", static_cast<unsigned>(reason),
              static_cast<unsigned>(g_staDisconnectEvents.load(std::memory_order_relaxed)));
    }
  });
}
#else
// The simulator has no radio, and the Plus compat layer logs the IDF reason
// inside its own WiFi.cpp without offering an event API, so nothing can be
// attributed to the join that is running. Callers are told, in the one place
// that decides, rather than being handed a count that would always say zero.
void ensureRadioEventClock() {}
#endif

}  // namespace

bool tracksRadioEvents() {
#if !defined(SIMULATOR) && !defined(CROSSPOINT_PLUS_PORT)
  return true;
#else
  return false;
#endif
}

wifi_attempt::Snapshot radioSnapshot() {
  ensureRadioEventClock();
  wifi_attempt::Snapshot snap;
#if defined(SIMULATOR)
  snap.status = wifi_attempt::kStatusDisconnected;
#else
  snap.status = static_cast<int>(WiFi.status());
#if !defined(CROSSPOINT_PLUS_PORT)
  snap.disconnectEvents = g_staDisconnectEvents.load(std::memory_order_relaxed);
#endif
#endif
  return snap;
}

uint8_t lastDisconnectReason() {
#if !defined(SIMULATOR) && !defined(CROSSPOINT_PLUS_PORT)
  return g_lastDisconnectReason.load(std::memory_order_relaxed);
#else
  return 0;
#endif
}

bool radioCommandActive() { return g_gate.commandDepth.load(std::memory_order_relaxed) > 0; }

bool stationClaimed() { return g_gate.stationClaims.load(std::memory_order_relaxed) > 0; }

// Deliberately only the command test, not the claim: pollIdle() re-asserts
// "modem sleep off" once a second, and a screen may hold a claim for minutes
// while a user reads a network list. Gating it on the claim would silently stop
// re-asserting the policy -- the failure mode AGENTS section 85.2 already caught
// once ("no error line" is not evidence a mitigation is in force). An
// interleaved esp_wifi_set_ps() at worst returns ESP_ERR_WIFI_STATE and the next
// second's pass asks again, which is recoverable.
bool radioIdle() { return !radioCommandActive(); }

bool waitForRadioQuiet(const uint32_t timeoutMs) {
#if defined(SIMULATOR)
  (void)timeoutMs;
  return true;
#else
  const uint32_t start = millis();
  while (radioCommandActive()) {
    if (static_cast<uint32_t>(millis() - start) >= timeoutMs) return false;
    delay(10);
  }
  return true;
#endif
}

void claimStation(const uint32_t quietTimeoutMs) {
  // Wait for an in-flight command before claiming so the sequence starts on a
  // quiet driver; the claim itself is never refused, because a screen must not
  // lose the station to a queue.
  if (!waitForRadioQuiet(quietTimeoutMs)) {
    LOG_INF("NET", "station claimed while a radio command was still executing");
  }
  ++g_gate.stationClaims;
}

void releaseStation() {
  // Clamped, and the imbalance is reported: releasing a station nobody holds
  // would leave background work believing a screen still owns it, which is the
  // silent direction of this bug -- a queue parked for the rest of the session
  // looks exactly like a network problem.
  if (g_gate.stationClaims.load(std::memory_order_relaxed) <= 0) {
    LOG_ERR("NET", "station release with no claim held; ignoring");
    return;
  }
  --g_gate.stationClaims;
}

StationClaim::StationClaim(const uint32_t quietTimeoutMs) { claimStation(quietTimeoutMs); }

StationClaim::~StationClaim() { releaseStation(); }

int joinStation(const char* ssid, const char* password, const bool connect) {
#if defined(SIMULATOR)
  (void)ssid;
  (void)password;
  (void)connect;
  return static_cast<int>(WL_DISCONNECTED);
#else
  if (ssid == nullptr || ssid[0] == '\0') return static_cast<int>(WL_DISCONNECTED);
  ensureRadioEventClock();
  // The five-argument form, so an empty passphrase is an open network rather
  // than a literal empty string, and so a screen that runs its own connect
  // state machine can ask the driver only to configure the station.
  CommandLock lock;
  // int, not wl_status_t: the Plus compatibility layer's begin() returns int and
  // an int does not convert implicitly to an unscoped enum, so naming the enum
  // type here would break that target while compiling fine on Arduino.
  const int status = WiFi.begin(ssid, (password != nullptr && password[0] != '\0') ? password : nullptr,
                                 /*channel=*/0, /*bssid=*/nullptr, connect);
  return status;
#endif
}

bool scanStationAsync() {
#if defined(SIMULATOR)
  return true;
#else
  CommandLock lock;
  return WiFi.scanNetworks(true) != WIFI_SCAN_FAILED;
#endif
}

void dropAssociation() {
#if !defined(SIMULATOR)
  CommandLock lock;
  WiFi.disconnect(false);
#endif
}

void requestNoModemSleep() {
#if !defined(SIMULATOR)
  CommandLock lock;
  // WiFi.setSleep(false) is esp_wifi_set_ps(): a driver command. Rejected while
  // the driver is down, which is why this is a request and not a guarantee --
  // enforceNoModemSleep() on the idle path is the guarantee.
  WiFi.setSleep(false);
#endif
}

void shutdownStation() {
#if !defined(SIMULATOR)
  CommandLock lock;
  // The three calls the screens used to write out by hand, in the same order:
  // the association first (sockets and DHCP close on top of it), then the mode
  // off, then the deinit that Arduino's mode() short-circuits past when it
  // already believes itself off.
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  esp_wifi_deinit();
#endif
}

void powerDownRadio() {
#if !defined(SIMULATOR)
  CommandLock lock;
  WiFi.mode(WIFI_OFF);
#endif
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
  {
    CommandLock lock;
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false);
  }
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
  // One gate for the entire teardown: a half-applied one -- driver deinit'd
  // while another task still believes the station is up, or storage left on RAM
  // because a second caller interleaved -- is worse than a slow one.
  CommandLock teardownLock;
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

namespace {

// The one bring-up sequence. A null renderer means the caller is a background
// worker that never registered one: every rule below still applies, and only the
// render-memory reclaim is skipped, because that is the step needing the object.
bool setModeInternal(GfxRenderer* renderer, const wifi_mode_t mode) {
  ensureRadioEventClock();
  bleinput::stop();
  logMemory("after BLE stop");
  if (renderer != nullptr) {
    prepare(*renderer);
  } else {
    static bool warned = false;
    if (!warned) {
      warned = true;
      LOG_INF("NET", "no renderer registered; skipping the render-memory reclaim for this bring-up");
    }
    logMemory("before radio bring-up");
  }
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
  // The mode change and its modem-sleep setting are one pair of driver calls:
  // both go inside the gate, and the retry sleeps *outside* it so a refused
  // attempt never occupies the gate while it waits.
  const auto applyMode = [&]() {
    CommandLock lock;
    WiFi.setAutoReconnect(true);  // releaseRadio() disables it; every session re-enables
    const bool ok = WiFi.mode(mode);
    if (ok && (mode & WIFI_MODE_STA) != 0) {
      // Kill the modem-sleep path before it can kill the device: with PS enabled,
      // IDF toggles PHY power gating and calls phy_track_pll_init(), which creates
      // its timer through ESP_ERROR_CHECK -- at a time chosen by the power manager,
      // not by us.  Measured on device: that landed inside a PDF session holding
      // 820 B of largest internal block and aborted the whole machine
      // (`ESP_ERR_NO_MEM ... phy_common.c:118`).  Keeping PS off removes the code
      // path entirely.  This call is only a head start -- it is rejected while the
      // driver is still down, which is why enforceNoModemSleep() re-asserts it on
      // the idle path.
      WiFi.setSleep(false);
    }
    return ok;
  };

  bool started = applyMode();
  if (!started && (mode & WIFI_MODE_STA) != 0) {
    // A refused mode change is what the driver answers while another task's
    // radio command is still executing (ESP_ERR_WIFI_STATE), and every screen in
    // the project reaches the radio through here -- so this is the one place
    // that can absorb it instead of each caller guessing which command was in
    // flight. Bounded to a single retry: a driver that still refuses has a real
    // problem and the caller should say so.
    LOG_INF("NET", "station mode refused; retrying once");
#if !defined(SIMULATOR)
    delay(400);
#endif
    started = applyMode();
  }
  if (!started && wasOff) resumeTimeSync();
  if (started) noteActivity();
  logMemory(started ? "WiFi mode ready" : "WiFi mode failed");
  return started;
}

}  // namespace

void registerRenderer(GfxRenderer* renderer) { g_registeredRenderer = renderer; }

bool setMode(GfxRenderer& renderer, const wifi_mode_t mode) { return setModeInternal(&renderer, mode); }

bool setMode(const wifi_mode_t mode) { return setModeInternal(g_registeredRenderer, mode); }

}  // namespace NetworkStartup
