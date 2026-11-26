#include "StandbyActivity.h"

#if defined(CROSSPOINT_PDF_XIP_SD)
#include "pdf/PdfModuleRuntime.h"
#endif

#include <Arduino.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_wifi.h>

#if CROSSPOINT_EMULATED == 0
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <fcntl.h>
#endif

#include <algorithm>
#include <cstdio>

#include "booksource/BookSource.h"
#include "BackgroundWork.h"
#include "CrossPointSettings.h"
#include "NetworkStartup.h"
#include "StandbyImageFace.h"
#include "StandbyTime.h"
#include "fontIds.h"
#include "util/SleepBadge.h"
#include "util/TimeUtils.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#ifdef ENABLE_CHINESE_VERSION
#include "ChineseCalendarFace.h"
#endif
#include "DuckSleepFace.h"
#include "SloppyClockFace.h"

namespace standby_debug {
namespace {
bool g_showBadge = false;
}
void showSleepBadge(bool on) { g_showBadge = on; }
bool sleepBadgeShown() { return g_showBadge; }
}  // namespace standby_debug

namespace {

constexpr uint32_t kWifiTimeoutMs = 15000u;  // Same as WifiSelectionActivity
constexpr uint32_t kNtpTimeoutMs = 12000u;
constexpr uint32_t kSyncDelayMs = 1500u;

bool g_debugWakeRequested = false;

// Diagnostic trail for "the clock returned to the reader by itself": light
// sleep only happens while the cable is unplugged, so serial is gone. One
// append per actual wake (never per timer tick), read back with
// CMD:CAT /.crosspoint/wake.log.
void logStandbyWake(const char* reason) {
#if CROSSPOINT_EMULATED == 0
  char line[96];
  const int n = snprintf(line, sizeof(line), "%lu,%s\n", static_cast<unsigned long>(millis()), reason);
  if (n <= 0) return;
  HalFile file = Storage.open("/.crosspoint/wake.log", O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return;
  (void)file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
  file.flush();
  file.close();
#else
  (void)reason;
#endif
}

}  // namespace

StandbyFace* StandbyActivity::activeFace() const {
  if (phase_ == Phase::LightSleep && lightFace_) return lightFace_.get();
  return standbyFace_.get();
}

bool StandbyActivity::buildStandbyFace() {
  std::unique_ptr<StandbyFace> face;
  switch (SETTINGS.standbyStyle) {
    case CrossPointSettings::STANDBY_SLOPPY_CLOCK:
      face = makeUniqueNoThrow<SloppyClockFace>();
      break;
#ifdef ENABLE_CHINESE_VERSION
    case CrossPointSettings::STANDBY_CALENDAR:
      if (renderer.getScreenHeight() > renderer.getScreenWidth()) {
        face = makeUniqueNoThrow<ChineseCalendarFace>();
      }
      break;
#endif
    case CrossPointSettings::STANDBY_CUSTOM_IMAGE:
      face = makeUniqueNoThrow<StandbyImageFace>("/.crosspoint/standby.bmp");
      if (face && !static_cast<StandbyImageFace*>(face.get())->valid()) {
        face.reset();
      }
      break;
    case CrossPointSettings::STANDBY_DUCK_SLEEP:
    default:
      break;
  }
  if (!face) {
    // Missing custom image / unavailable calendar: the duck is the safe default.
    face = makeUniqueNoThrow<DuckSleepFace>(/*showClock=*/true);
  }
  if (!face) return false;
  face->onEnter();
  standbyFace_ = std::move(face);
  return true;
}

bool StandbyActivity::buildLightFace() {
  lightFace_.reset();
  if (SETTINGS.lightSleepStyle == CrossPointSettings::LIGHT_SLEEP_FOLLOW_STANDBY) {
    // Reuse the standby face (including its minute clock). activeFace() falls
    // back to standbyFace_ when lightFace_ is null.
    return standbyFace_ != nullptr;
  }
  std::unique_ptr<StandbyFace> face;
  if (SETTINGS.lightSleepStyle == CrossPointSettings::LIGHT_SLEEP_CUSTOM_IMAGE) {
    face = makeUniqueNoThrow<StandbyImageFace>("/.crosspoint/lightsleep.bmp");
    if (face && !static_cast<StandbyImageFace*>(face.get())->valid()) face.reset();
  }
  if (!face) face = makeUniqueNoThrow<DuckSleepFace>(/*showClock=*/false);
  if (!face) return false;
  face->onEnter();
  lightFace_ = std::move(face);
  return true;
}

void StandbyActivity::onEnter() {
  Activity::onEnter();
  // Any non-reader standby is a cache window: the service silently uses the
  // saved network and keeps the device awake (BackgroundWork) until the queue
  // finishes or the link fails. From the reader the reader flag is still set,
  // so this is a no-op.
  booksource::onStandby();
  logStandbyWake("enter");
  LOG_DBG("STANDBY", "onEnter free heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
  phase_ = Phase::Standby;
  if (!buildStandbyFace()) {
    LOG_ERR("STANDBY", "OOM allocating face");
    activityManager.goToApps();
    return;
  }
  lastInputMs_ = millis();
  powerArmed_ = false;
  powerArmAtMs_ = millis() + 350;
  g_debugWakeRequested = false;
  silentCandidates_.clear();
  silentCandidateIndex_ = 0;
  if (!TimeUtils::isClockValid() && SETTINGS.clockAutoSync) {
    syncState_ = SyncState::Delayed;
    syncStartMs_ = millis();
  }
  requestUpdate();
}

void StandbyActivity::onExit() {
  switch (syncState_) {
    case SyncState::Idle:
    case SyncState::Delayed:
    case SyncState::Unavailable:
      break;
    case SyncState::WifiConnecting:
    case SyncState::ClockSyncing:
      stopTimeSyncWifi();
      break;
  }
  silentCandidates_.clear();
  silentCandidateIndex_ = 0;
  syncState_ = SyncState::Idle;
  releaseSilentStation();
  if (lightFace_) {
    lightFace_->onExit();
    lightFace_.reset();
  }
  if (standbyFace_) {
    standbyFace_->onExit();
    standbyFace_.reset();
  }
  LOG_DBG("STANDBY", "onExit free heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
  Activity::onExit();
}

void StandbyActivity::startTimeSync() {
  if (TimeUtils::isClockValid() || !SETTINGS.clockAutoSync) return;
  // A background cache owns the radio; don't fight it for a clock sync.
  if (background_work::active()) {
    syncState_ = SyncState::Unavailable;
    requestUpdate();
    return;
  }
#if defined(CROSSPOINT_PLUS_PORT)
  // The 4G transport aliases WIFI_STA in the compat layer. Scanning or trying
  // saved SSIDs there would either block on the modem attach or tear it down.
  if (SETTINGS.networkType == CrossPointSettings::NETWORK_CELLULAR && SETTINGS.cellularPresent != 0) {
    syncState_ = SyncState::Unavailable;
    requestUpdate();
    return;
  }
#endif

  // Another activity (e.g. Settings → WiFi) may have already connected the
  // device. Skip our own WiFi.begin in that case and request a clock sync.
  if (WiFi.status() == WL_CONNECTED) {
    NetworkStartup::prepare(renderer);
    if (!silentNetworkHold_) {
      NetworkStartup::hold();
      silentNetworkHold_ = true;
    }
    LOG_DBG("STANDBY", "WiFi already connected, skipping silent attempt");
    beginClockSync();
    return;
  }

  // Default path: try saved credentials silently. The old version only tried
  // lastConnectedSsid and then pushed the Wi-Fi picker on top of the lock
  // screen when that access point happened to be out of range. Scan first and
  // fall back to every saved network that is actually visible.
  if (!trySilentWifiConnect()) {
    markTimeSyncUnavailable();
  }
}

bool StandbyActivity::trySilentWifiConnect() {
  // SD I/O shares SPI with the e-ink panel, so we hold a RenderLock for the
  // duration of the credential file access.
  std::vector<WifiCredential> saved;
  std::string last;
  {
    RenderLock lock(*this);
    if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
    last = WIFI_STORE.getLastConnectedSsid();
    const size_t count = WIFI_STORE.getCredentialCount();
    saved.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const auto cred = WIFI_STORE.getCredentialAt(i);
      if (cred) saved.push_back(*cred);
    }
  }
  if (saved.empty()) {
    LOG_DBG("STANDBY", "No saved WiFi credentials for silent connect");
    return false;
  }

  // Claim the station for the whole silent sequence -- mode change, scan and
  // the candidate joins below. The JJWXC queue is *designed* to start when the
  // device enters standby, so an unclaimed sequence here is exactly the
  // interleave the driver answers with ESP_ERR_WIFI_STATE, and this screen reads
  // that refusal as "this network failed" and skips a healthy AP.
  claimSilentStation();
  if (!silentNetworkHold_) {
    NetworkStartup::hold();
    silentNetworkHold_ = true;
  }
  WiFi.persistent(false);
  if (!NetworkStartup::setMode(renderer, WIFI_STA)) {
    LOG_ERR("STANDBY", "Station initialization failed for silent connect");
    releaseSilentStation();
    return false;
  }
  // Do not erase the AP before begin(): on a fresh mode change STA is not
  // started and Arduino's eraseap path returns "STA not started", which can
  // make this first association time out. begin() replaces the config in RAM.
  delay(100);

  struct VisibleNetwork {
    int32_t rssi = 0;
    const WifiCredential* cred = nullptr;
  };
  std::vector<VisibleNetwork> visible;
  // The scan stays a direct call: it blocks for seconds, and a gate held that
  // long would only make every other caller time out on it. The claim taken
  // above is what keeps the background queue off the radio for the whole
  // sequence, which is the interleave that actually produced refusals.
  const int scanCount = WiFi.scanNetworks(false, true);
  if (scanCount > 0) {
    visible.reserve(static_cast<size_t>(scanCount));
    for (int i = 0; i < scanCount; ++i) {
      const std::string scanned(WiFi.SSID(static_cast<uint8_t>(i)).c_str());
      if (scanned.empty()) continue;
      for (const auto& cred : saved) {
        if (cred.ssid != scanned) continue;
        const bool already = std::any_of(visible.begin(), visible.end(),
                                         [&cred](const VisibleNetwork& n) { return n.cred == &cred; });
        if (!already) visible.push_back({WiFi.RSSI(static_cast<uint8_t>(i)), &cred});
        break;
      }
    }
  }
  WiFi.scanDelete();

  std::sort(visible.begin(), visible.end(),
            [](const VisibleNetwork& a, const VisibleNetwork& b) { return a.rssi > b.rssi; });
  if (!last.empty() && !visible.empty()) {
    const auto lastIt = std::find_if(visible.begin(), visible.end(),
                                     [&last](const VisibleNetwork& n) { return n.cred->ssid == last; });
    if (lastIt != visible.end()) std::rotate(visible.begin(), lastIt, lastIt + 1);
  }

  silentCandidates_.clear();
  silentCandidateIndex_ = 0;
  for (const auto& network : visible) {
    silentCandidates_.emplace_back(network.cred->ssid, network.cred->password);
  }
  // If the scan failed or returned nothing, still give the last known network
  // one chance (it may be hidden). No other absent SSIDs are tried: on the Plus
  // a failed WiFi.begin can block the UI task for the full association timeout.
  if (silentCandidates_.empty() && !last.empty()) {
    const auto cred = std::find_if(saved.begin(), saved.end(),
                                   [&last](const WifiCredential& c) { return c.ssid == last; });
    if (cred != saved.end()) silentCandidates_.emplace_back(cred->ssid, cred->password);
  }

  LOG_DBG("STANDBY", "Silent WiFi candidates: %u (scan=%d, saved=%u)", static_cast<unsigned>(silentCandidates_.size()),
          scanCount, static_cast<unsigned>(saved.size()));
  if (silentCandidates_.empty()) {
    releaseSilentStation();
    return false;
  }
  return tryNextSilentCandidate();
}

bool StandbyActivity::tryNextSilentCandidate() {
  while (silentCandidateIndex_ < silentCandidates_.size()) {
    const auto& candidate = silentCandidates_[silentCandidateIndex_++];
    NetworkStartup::dropAssociation();
    delay(50);
    // Snapshot before the join: the core stores the status its last event wrote,
    // so a WL_CONNECT_FAILED left behind by the candidate before this one is
    // also what begin() reports -- and the old test skipped a network that was
    // fine. Only a driver refusal says anything about *this* attempt
    // (activities/network/WifiAttempt.h).
    //
    // joinStation() is the single entry point on purpose: it holds the radio command
    // claim, supplies the simulator stub, and returns int so the Plus compat layer's
    // begin() (which is not wl_status_t) compiles too. ui-contract's raw WiFi.begin()
    // here predates that and was superseded by the wifi line -- keeping both would give
    // this candidate loop a second, ungated way to command the radio.
    const wifi_attempt::Snapshot before = NetworkStartup::radioSnapshot();
    const int beginStatus =
        NetworkStartup::joinStation(candidate.first.c_str(),
                                    candidate.second.empty() ? nullptr : candidate.second.c_str(),
                                    /*connect=*/false);
    const wifi_attempt::Snapshot after{beginStatus, NetworkStartup::radioSnapshot().disconnectEvents};
    const wifi_attempt::Verdict verdict =
        wifi_attempt::classifyBegin(before, after, NetworkStartup::tracksRadioEvents());
    LOG_DBG("STANDBY", "Silent WiFi attempt: %s (%s)", candidate.first.c_str(),
            verdict == wifi_attempt::Verdict::DriverRefused ? "driver refused" : "started");
    if (verdict == wifi_attempt::Verdict::DriverRefused) continue;
    attemptStart_ = before;
    syncState_ = SyncState::WifiConnecting;
    syncStartMs_ = millis();
    requestUpdate();
    return true;
  }
  markTimeSyncUnavailable();
  return false;
}

// A claim, not an RAII StationClaim: this sequence spans several loop() ticks,
// and an Activity cannot keep a scope alive across them. claimSilentStation() is
// idempotent for exactly that reason -- the candidate loop re-enters the join
// while the first claim is still held.
void StandbyActivity::claimSilentStation() {
  if (silentStationClaimed_) return;
  NetworkStartup::claimStation();
  silentStationClaimed_ = true;
}

void StandbyActivity::releaseSilentStation() {
  if (!silentStationClaimed_) return;
  silentStationClaimed_ = false;
  NetworkStartup::releaseStation();
}

void StandbyActivity::markTimeSyncUnavailable() {
  silentCandidates_.clear();
  silentCandidateIndex_ = 0;
  stopTimeSyncWifi();
  syncState_ = SyncState::Unavailable;
  requestUpdate();
}

void StandbyActivity::beginClockSync() {
  if (!silentNetworkHold_) {
    NetworkStartup::hold();
    silentNetworkHold_ = true;
  }
  if (!halClock.requestSync()) {
    LOG_ERR("STANDBY", "Clock sync could not start");
    markTimeSyncUnavailable();
    return;
  }
  syncState_ = SyncState::ClockSyncing;
  syncStartMs_ = millis();
  LOG_DBG("STANDBY", "Clock sync started");
}

void StandbyActivity::pumpTimeSync() {
  if (syncState_ == SyncState::Idle || syncState_ == SyncState::Unavailable) return;
  const uint32_t elapsed = millis() - syncStartMs_;

  switch (syncState_) {
    case SyncState::Idle:
    case SyncState::Unavailable:
      return;
    case SyncState::Delayed:
      if (elapsed < kSyncDelayMs) return;
      syncState_ = SyncState::Idle;
      startTimeSync();
      return;
    case SyncState::WifiConnecting: {
      const wl_status_t status = WiFi.status();
      if (status == WL_CONNECTED) {
        silentCandidates_.clear();
        silentCandidateIndex_ = 0;
        beginClockSync();
        return;
      }
      // A failure status is evidence about this candidate only if the event
      // clock moved after its join; otherwise it is what the previous candidate
      // left in the same field. The timeout is evidence on its own.
      const bool liveFailure = wifi_attempt::polledFailureIsLive(attemptStart_, NetworkStartup::radioSnapshot(),
                                                                 NetworkStartup::tracksRadioEvents());
      if (elapsed < kWifiTimeoutMs && !liveFailure) return;

      LOG_DBG("STANDBY", "Silent WiFi attempt failed (status=%d live=%d t=%ums), trying next saved network",
              static_cast<int>(status), liveFailure ? 1 : 0, static_cast<unsigned>(elapsed));
      NetworkStartup::dropAssociation();
      tryNextSilentCandidate();
      return;
    }
    case SyncState::ClockSyncing:
      switch (halClock.syncState()) {
        case ClockSyncState::Succeeded:
          LOG_DBG("STANDBY", "Clock synchronized");
          completeTimeSync();
          return;
        case ClockSyncState::Failed:
          LOG_DBG("STANDBY", "Clock sync failed");
          markTimeSyncUnavailable();
          return;
        case ClockSyncState::Idle:
          if (elapsed < kNtpTimeoutMs) return;
          LOG_DBG("STANDBY", "Clock sync stopped");
          markTimeSyncUnavailable();
          return;
        case ClockSyncState::Syncing:
          if (elapsed < kNtpTimeoutMs) return;
          LOG_DBG("STANDBY", "Clock sync timed out");
          markTimeSyncUnavailable();
          return;
      }
      break;
  }
}

void StandbyActivity::stopTimeSyncWifi() {
  silentCandidates_.clear();
  silentCandidateIndex_ = 0;
  releaseSilentStation();
  if (silentNetworkHold_) {
    NetworkStartup::releaseHold();
    silentNetworkHold_ = false;
  }
  NetworkStartup::releaseRadio();
}

void StandbyActivity::completeTimeSync() {
  stopTimeSyncWifi();
  syncState_ = SyncState::Idle;
  lastInputMs_ = millis();
  requestUpdate();
}

void StandbyActivity::processFaceTick(const bool waitForUpdate) {
  StandbyFace* face = activeFace();
  if (!face) return;

  switch (face->tick()) {
    case StandbyFace::TickResult::None:
      return;
    case StandbyFace::TickResult::Redraw:
      break;
    case StandbyFace::TickResult::RedrawWithGhostCleanup:
#if CROSSPOINT_EMULATED == 0
#if FREEINK_DEVICE_READPICO
      // The face's own ~10-minute cleanup signal. Standby ticks present as
      // TransientUi, so this is the only residue cleanup a calm face gets on
      // this panel. Make it a real GC16 sweep.
      renderer.requestNextRefresh(HalDisplay::FULL_REFRESH);
#else
      if (gpio.isXteinkDevice()) renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
#endif
#endif
      break;
  }

  if (waitForUpdate)
    requestUpdateAndWait();
  else
    requestUpdate();
}

bool StandbyActivity::canEnterLightSleep() const {
#if CROSSPOINT_EMULATED == 0
  if (phase_ != Phase::Standby) return false;
  if (background_work::active()) return false;
  const bool syncActive = syncState_ == SyncState::WifiConnecting || syncState_ == SyncState::ClockSyncing;
  const bool wifiActive = WiFi.getMode() != WIFI_MODE_NULL;
#if defined(CROSSPOINT_PLUS_PORT)
  const bool usbBlocks = gpio.isUsbConnected() && !HalPowerManager::debugUsbSleepAllowed();
#else
  const bool usbBlocks = gpio.isUsbConnected();
#endif
  return !syncActive && !wifiActive && !usbBlocks && powerManager.canStandbyLightSleep(gpio);
#else
  return false;
#endif
}

void StandbyActivity::enterLightSleep() {
  if (!buildLightFace()) return;
  phase_ = Phase::LightSleep;
  // Paint the light-sleep face before the CPU stops; a custom image is a full
  // frame, while "follow standby" only adds the badge on the next tick.
  requestUpdateAndWait();
}

bool StandbyActivity::tryLightSleep(const uint32_t idleMs) {
#if CROSSPOINT_EMULATED == 0
  if (phase_ != Phase::LightSleep) return false;
  if (background_work::active()) return false;

  const bool syncActive = syncState_ == SyncState::WifiConnecting || syncState_ == SyncState::ClockSyncing;
  const bool wifiActive = WiFi.getMode() != WIFI_MODE_NULL;
#if defined(CROSSPOINT_PDF_XIP_SD)
  // The SD PDF engine runs from a PSRAM window; parking for light sleep may cut
  // the rails behind it and the S31 cannot unmap it safely. Stay awake after any
  // PDF session in this boot instead of risking a return into vanished code.
  if (pdf_module::isLoaded()) return false;
#endif
  const bool hardwareAllowed = powerManager.canStandbyLightSleep(gpio);
#if defined(CROSSPOINT_PLUS_PORT)
  const bool usbConnected = gpio.isUsbConnected() && !HalPowerManager::debugUsbSleepAllowed();
#else
  const bool usbConnected = gpio.isUsbConnected();
#endif
  if (!standby_time::shouldLightSleep(idleMs, syncActive, wifiActive, usbConnected, hardwareAllowed)) {
    return false;
  }

  StandbyFace* face = activeFace();
  if (!face) return false;
  const uint32_t seconds = std::clamp<uint32_t>(face->secondsUntilNextWake(), 1, 60);
  HalPowerManager::LightSleepWakeReason wakeReason;
  {
    RenderLock lock;
    wakeReason = powerManager.lightSleepFor(seconds);
  }
  switch (wakeReason) {
    case HalPowerManager::LightSleepWakeReason::Timer:
      // Static light-sleep images do not repaint; a clock-following style does.
      processFaceTick(/*waitForUpdate=*/true);
      return true;
    case HalPowerManager::LightSleepWakeReason::PowerButton:
      logStandbyWake("power");
      mappedInput.notePowerWakeFromSleep();
      finish();
      return true;
    case HalPowerManager::LightSleepWakeReason::Failed: {
      char reason[32];
      const int n = snprintf(reason, sizeof(reason), "failed(%u/%d)",
                             static_cast<unsigned>(HalPowerManager::lastLightSleepFailure()),
                             static_cast<int>(HalPowerManager::lastLightSleepError()));
      if (n > 0) logStandbyWake(reason);
      // Light sleep refused (USB/WiFi/hardware): return to the standby face
      // rather than leaving the light-sleep image on screen.
      phase_ = Phase::Standby;
      if (lightFace_) {
        lightFace_->onExit();
        lightFace_.reset();
      }
      lastInputMs_ = millis();
      requestUpdate();
      return true;
    }
  }
#else
  (void)idleMs;
#endif
  return false;
}

void StandbyActivity::debugWakeNow() { g_debugWakeRequested = true; }

void StandbyActivity::loop() {
  if (!powerArmed_ && millis() >= powerArmAtMs_ && !mappedInput.isPressed(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    powerArmed_ = true;
  }
  if (g_debugWakeRequested) {
    g_debugWakeRequested = false;
    logStandbyWake("debug");
    finish();
    return;
  }
  // Standby is an overlay: Back and the power key both return to the caller
  // (reader, settings page, home). There is no in-standby style cycling.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    logStandbyWake("back");
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    lastInputMs_ = millis();
    if (powerArmed_) {
      logStandbyWake("power-release");
      finish();
    }
    return;
  }

  const bool bgActive = background_work::active();
  if (bgActive != lastBackgroundActive_) {
    lastBackgroundActive_ = bgActive;
    requestUpdate();  // add/remove the "caching" badge
  }

  pumpTimeSync();

  const uint32_t idle = millis() - lastInputMs_;
  if (phase_ == Phase::Standby && idle >= standby_time::kLightSleepIdleMs && canEnterLightSleep()) {
    enterLightSleep();
    if (tryLightSleep(idle)) return;
    // enterLightSleep painted a face but light sleep was refused; the helper
    // above already restored Standby.
    return;
  }

  if (phase_ == Phase::LightSleep) {
    if (tryLightSleep(idle)) return;
    // Conditions changed (USB, radio, or a background job started): show the
    // standby face again and keep running.
    phase_ = Phase::Standby;
    if (lightFace_) {
      lightFace_->onExit();
      lightFace_.reset();
    }
    lastInputMs_ = millis();
    requestUpdate();
    return;
  }

  processFaceTick(false);
}

void StandbyActivity::render(RenderLock&&) {
  StandbyFace* face = activeFace();
  if (!face) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  int viewTop = 0;
  int viewRight = 0;
  int viewBottom = 0;
  int viewLeft = 0;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);
  const Rect faceViewport{viewLeft, viewTop, sw - viewLeft - viewRight, sh - viewTop - viewBottom};
  const int overlayTop = std::max(metrics.topPadding, viewTop);

  renderer.clearScreen();
  {
    const GfxRenderer::ClipScope clip(renderer, faceViewport.x, faceViewport.y, faceViewport.width,
                                      faceViewport.height);
    face->render(renderer, faceViewport);
  }

  const bool clockValid = TimeUtils::isClockValid();
  if (!clockValid && phase_ == Phase::Standby) {
    const bool syncing = syncState_ == SyncState::Delayed || syncState_ == SyncState::WifiConnecting ||
                         syncState_ == SyncState::ClockSyncing;
    const char* syncText =
        I18N.get(syncing ? StrId::STR_STANDBY_SYNCING : StrId::STR_CLOCK_SYNC_NO_WIFI);
    UITheme::drawCenteredText(renderer, faceViewport, SMALL_FONT_ID, overlayTop, syncText);
  }

  const bool debugBadge = standby_debug::sleepBadgeShown();
  if (debugBadge ||
      (phase_ == Phase::LightSleep && SETTINGS.sleepStateBadge != CrossPointSettings::SLEEP_BADGE_OFF)) {
    drawSleepStateBadge(renderer, tr(STR_LIGHT_SLEEP));
  }
  if (background_work::active()) {
    // A background cache is why light sleep is not being entered; say so
    // instead of looking like a frozen clock.
    drawSleepStateBadge(renderer, tr(STR_BACKGROUND_CACHING), /*topLeft=*/true);
  }

  if (phase_ == Phase::Standby) {
    // The battery capsule is standby chrome. Light sleep and a background job
    // keep it out of the face: the state badge is the only extra mark there.
    GUI.drawStatusBarBattery(renderer, sw - viewRight - metrics.batteryWidth - metrics.contentSidePadding,
                             overlayTop, /*showPercent=*/false);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
}

bool StandbyActivity::preventAutoSleep() {
  // While a background cache is actually running, auto deep sleep would suspend
  // the radio or kill the worker; keep the overlay awake and show the badge.
  // Do not consult the activity below: a finished-but-unreaped JJWXC job keeps
  // its own preventAutoSleep() true until it is polled, and that would block
  // sleep forever while the standby overlay is on top. The background guard
  // ends with the worker body, which is the real "still working" signal.
  return background_work::active();
}
