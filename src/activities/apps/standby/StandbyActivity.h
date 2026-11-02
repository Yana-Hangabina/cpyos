#pragma once

#include <cstdint>
#include <memory>

#include "StandbyFace.h"
#include "activities/Activity.h"

struct ActivityResult;

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
  bool trySilentWifiConnect();
  void promptForWifi();
  void onWifiResult(const ActivityResult& result);
  void beginClockSync();
  void pumpTimeSync();
  void stopTimeSyncWifi();
  void completeTimeSync();
};
