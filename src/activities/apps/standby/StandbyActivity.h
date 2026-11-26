#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "StandbyFace.h"
#include "activities/Activity.h"
// Pure header (fixed-width ints only): the silent connect needs the same
// attempt-attribution rules the Wi-Fi sheet uses, without pulling Arduino
// radio types into this header.
#include "activities/network/WifiAttempt.h"


/* Bench-only: paint the light-sleep badge on the standby face without parking
 * the CPU, so the badge's text and placement can be screenshotted over USB. */
namespace standby_debug {
void showSleepBadge(bool on);
bool sleepBadgeShown();
}  // namespace standby_debug

class StandbyActivity final : public Activity {
 public:
  explicit StandbyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Standby", renderer, mappedInput) {}

  // Serial test harness: behave as if the power key was pressed in standby.
  static void debugWakeNow();

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  void onExit() override;
  // The face repaints once a minute just to move the clock digits. The scan time
  // is fixed, but a band push drives only the changed strip: less panel current
  // and no re-driving of the whole face every minute.
  bool wantsBandUpdates() const override { return true; }
  // The standby face is full-bleed art sized for the physical panel; UI scaling
  // must not shrink it into a letterboxed card.
  UiLayoutMode uiLayoutMode() const override { return UiLayoutMode::Native; }

  // Standby manages its own timer/GPIO light-sleep cycles. Automatic deep sleep
  // is allowed once the device has reached light sleep, but never while a
  // background job (JJWXC comment cache, a book download) is still running: the
  // worker would be suspended or killed mid-write. The caller is the activity
  // below the standby overlay, so stackPreventAutoSleep() is part of the check.
  bool preventAutoSleep() override;
  bool skipLoopDelay() override {
    return syncState_ == SyncState::WifiConnecting || syncState_ == SyncState::ClockSyncing;
  }

 private:
  enum class SyncState : uint8_t {
    Idle,
    Delayed,
    WifiConnecting,
    ClockSyncing,
    // Silent connection was attempted and failed (or no saved network was
    // visible). The face says so instead of showing a permanent "Syncing…".
    Unavailable,
  };

  enum class Phase : uint8_t {
    Standby,   // short power press: selected style, battery visible
    LightSleep // 35s idle: light-sleep style + state badge
  };

  std::unique_ptr<StandbyFace> standbyFace_;
  std::unique_ptr<StandbyFace> lightFace_;
  Phase phase_ = Phase::Standby;
  SyncState syncState_ = SyncState::Idle;
  uint32_t syncStartMs_ = 0;
  uint32_t lastInputMs_ = 0;
  // The first ~350 ms after entry are a release barrier so the press that
  // launched this activity cannot immediately close it.
  bool powerArmed_ = false;
  uint32_t powerArmAtMs_ = 0;
  bool lastBackgroundActive_ = false;

  StandbyFace* activeFace() const;
  bool buildStandbyFace();
  bool buildLightFace();
  void enterLightSleep();
  void processFaceTick(bool waitForUpdate);
  bool tryLightSleep(uint32_t idleMs);

  bool canEnterLightSleep() const;

  void startTimeSync();
  // The silent connect owns the station for as long as it is cycling candidates;
  // see the comment on the definitions.
  void claimSilentStation();
  void releaseSilentStation();
  bool trySilentWifiConnect();
  bool tryNextSilentCandidate();
  void beginClockSync();
  void pumpTimeSync();
  void stopTimeSyncWifi();
  void completeTimeSync();
  void markTimeSyncUnavailable();

  // Candidate networks discovered during the standby-only silent connect.
  // Stored as plain pairs so this header does not pull the credential store
  // (and ArduinoJson) into every translation unit.
  std::vector<std::pair<std::string, std::string>> silentCandidates_;
  size_t silentCandidateIndex_ = 0;
  bool silentNetworkHold_ = false;
  bool silentStationClaimed_ = false;
  // Where the candidate being tried now started, so a failure status seen later
  // can be told apart from the one the previous candidate left behind.
  wifi_attempt::Snapshot attemptStart_;
};
