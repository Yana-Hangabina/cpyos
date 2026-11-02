#include "StandbyActivity.h"

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

#include "activities/apps/jjwxc/JjwxcCacheService.h"
#include "BackgroundWork.h"
#include "CrossPointSettings.h"
#include "NetworkStartup.h"
#include "StandbyImageFace.h"
#include "StandbyTime.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
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

namespace {

constexpr uint32_t kWifiTimeoutMs = 15000u;  // Same as WifiSelectionActivity
constexpr uint32_t kNtpTimeoutMs = 12000u;
constexpr uint32_t kSyncDelayMs = 1500u;

// Once per power-on, Standby may push the WiFi selection UI to help the user
// get online for clock sync. After it has been shown (regardless of whether the
// user connected or cancelled), subsequent Standby entries within the same
// session will not re-prompt — the explicit way to retry is Settings → WiFi.
bool g_promptedForWifiThisSession = false;
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
  jjwxc_cache_service::onStandbyEntered();
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
      break;
    case SyncState::WifiConnecting:
    case SyncState::ClockSyncing:
      stopTimeSyncWifi();
      break;
  }
  syncState_ = SyncState::Idle;
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
  if (background_work::active()) return;

  // Another activity (e.g. Settings → WiFi) may have already connected the
  // device. Skip our own WiFi.begin in that case and request a clock sync.
  if (WiFi.status() == WL_CONNECTED) {
    NetworkStartup::prepare(renderer);
    LOG_DBG("STANDBY", "WiFi already connected, skipping silent attempt");
    beginClockSync();
    return;
  }

  // Default path: try the saved "last connected" credentials silently.
  if (trySilentWifiConnect()) {
    return;
  }

  // No credentials available to try. Push the WiFi selection UI (once per
  // session) so the user can connect manually.
  if (!g_promptedForWifiThisSession) {
    promptForWifi();
  }
}

bool StandbyActivity::trySilentWifiConnect() {
  // SD I/O shares SPI with the e-ink panel, so we hold a RenderLock for the
  // duration of the credential file access.
  std::string ssid;
  std::string pass;
  {
    RenderLock lock(*this);
    if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
    const std::string last = WIFI_STORE.getLastConnectedSsid();
    if (last.empty()) {
      LOG_DBG("STANDBY", "No lastConnectedSsid for silent connect");
      return false;
    }
    const auto cred = WIFI_STORE.findCredential(last);
    if (!cred) {
      LOG_DBG("STANDBY", "lastConnectedSsid '%s' has no saved credential", last.c_str());
      return false;
    }
    ssid = cred->ssid;
    pass = cred->password;
  }

  WiFi.persistent(false);
  NetworkStartup::setMode(renderer, WIFI_STA);
  // Fresh-slate disconnect (radio off + erase cached AP) so the next begin()
  // latches the exact creds we just loaded, not whatever the radio retained
  // from a previous session.
  WiFi.disconnect(true, true);
  delay(100);
  if (pass.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  syncState_ = SyncState::WifiConnecting;
  syncStartMs_ = millis();
  LOG_DBG("STANDBY", "Silent WiFi connect start: %s", ssid.c_str());
  return true;
}

void StandbyActivity::promptForWifi() {
  g_promptedForWifiThisSession = true;
  LOG_DBG("STANDBY", "Prompting WiFi selection UI");
  auto activity = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, /*autoConnect=*/false,
                                                           /*keepRadioOnExit=*/false);
  if (!activity) {
    LOG_ERR("STANDBY", "OOM: WifiSelectionActivity (%u bytes)", static_cast<unsigned>(sizeof(WifiSelectionActivity)));
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult& result) { onWifiResult(result); });
}

void StandbyActivity::onWifiResult(const ActivityResult& result) {
  if (result.isCancelled) {
    LOG_DBG("STANDBY", "WiFi UI cancelled; clock remains unavailable");
    stopTimeSyncWifi();
    syncState_ = SyncState::Idle;
    return;
  }
  if (TimeUtils::isClockValid()) {
    LOG_DBG("STANDBY", "WiFi UI returned with a trustworthy clock");
    completeTimeSync();
    return;
  }

  LOG_DBG("STANDBY", "WiFi UI returned connected; requesting clock sync");
  beginClockSync();
  requestUpdate();  // refresh header back to "Syncing…"
}

void StandbyActivity::beginClockSync() {
  if (!halClock.requestSync()) {
    LOG_ERR("STANDBY", "Clock sync could not start");
    stopTimeSyncWifi();
    syncState_ = SyncState::Idle;
    return;
  }
  syncState_ = SyncState::ClockSyncing;
  syncStartMs_ = millis();
  LOG_DBG("STANDBY", "Clock sync started");
}

void StandbyActivity::pumpTimeSync() {
  if (syncState_ == SyncState::Idle) return;
  const uint32_t elapsed = millis() - syncStartMs_;

  switch (syncState_) {
    case SyncState::Idle:
      return;
    case SyncState::Delayed:
      if (elapsed < kSyncDelayMs) return;
      syncState_ = SyncState::Idle;
      startTimeSync();
      return;
    case SyncState::WifiConnecting: {
      const wl_status_t status = WiFi.status();
      if (status == WL_CONNECTED) {
        beginClockSync();
        return;
      }
      if (status != WL_CONNECT_FAILED && status != WL_NO_SSID_AVAIL && elapsed < kWifiTimeoutMs) return;

      LOG_DBG("STANDBY", "Silent WiFi sync failed (status=%d, t=%ums)", static_cast<int>(status),
              static_cast<unsigned>(elapsed));
      stopTimeSyncWifi();
      syncState_ = SyncState::Idle;
      if (!g_promptedForWifiThisSession)
        promptForWifi();
      else
        requestUpdate();
      return;
    }
    case SyncState::ClockSyncing:
      switch (halClock.syncState()) {
        case ClockSyncState::Succeeded:
          g_promptedForWifiThisSession = false;
          LOG_DBG("STANDBY", "Clock synchronized");
          completeTimeSync();
          return;
        case ClockSyncState::Failed:
          LOG_DBG("STANDBY", "Clock sync failed");
          stopTimeSyncWifi();
          syncState_ = SyncState::Idle;
          return;
        case ClockSyncState::Idle:
          if (elapsed < kNtpTimeoutMs) return;
          LOG_DBG("STANDBY", "Clock sync stopped");
          stopTimeSyncWifi();
          syncState_ = SyncState::Idle;
          return;
        case ClockSyncState::Syncing:
          if (elapsed < kNtpTimeoutMs) return;
          LOG_DBG("STANDBY", "Clock sync timed out");
          stopTimeSyncWifi();
          syncState_ = SyncState::Idle;
          return;
      }
      break;
  }
}

void StandbyActivity::stopTimeSyncWifi() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  esp_wifi_deinit();
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
  const bool syncActive = syncState_ != SyncState::Idle;
  const bool wifiActive = WiFi.getMode() != WIFI_MODE_NULL;
  return !syncActive && !wifiActive && !gpio.isUsbConnected() && powerManager.canStandbyLightSleep(gpio);
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

  const bool syncActive = syncState_ != SyncState::Idle;
  const bool wifiActive = WiFi.getMode() != WIFI_MODE_NULL;
  const bool hardwareAllowed = powerManager.canStandbyLightSleep(gpio);
  if (!standby_time::shouldLightSleep(idleMs, syncActive, wifiActive, gpio.isUsbConnected(), hardwareAllowed)) {
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
    UITheme::drawCenteredText(renderer, faceViewport, SMALL_FONT_ID, overlayTop, tr(STR_STANDBY_SYNCING));
  }

  if (phase_ == Phase::LightSleep && SETTINGS.sleepStateBadge != CrossPointSettings::SLEEP_BADGE_OFF) {
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
