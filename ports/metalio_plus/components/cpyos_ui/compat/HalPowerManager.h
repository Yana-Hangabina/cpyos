#pragma once

#include <Arduino.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager; // Singleton

class HalPowerManager {
  int normalFreq = 0; // MHz
  bool isLowPower = false;

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr; // Protect access to currentLockMode

public:
  static constexpr int LOW_POWER_FREQ = 10; // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;
  static constexpr unsigned long IDLE_DOWNCLOCK_MS = 500;
  static constexpr unsigned long IDLE_LIGHT_SLEEP_MS = 1000;
  // The vendor SOC policy averages one sample per second; poll at that cadence.
  static constexpr unsigned long BATTERY_POLL_MS = 1000;
  static constexpr unsigned long LIGHT_SLEEP_SLICE_MS = 50;
  static constexpr unsigned long BUSY_SLEEP_SLICE_MS = 20;

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Standby light sleep (Plus port).
  //
  // The wake source is the TCA9555 INT# on GPIO2 (not an ESP power-key GPIO):
  // the expander asserts it for the P1.7 power key, and the SC7A20H shares the
  // same wired-OR line. The real sequence lives in HalPowerManager.cpp and
  // mirrors the vendor power policy: park SD, TPS sleep, hold the EPD bus low,
  // mask the expander, arm EXT1 ANY_LOW + timer, cut the P5/P14 rails, sleep.
  enum class LightSleepWakeReason : uint8_t { Timer, PowerButton, Failed };

  // Distinct small codes for StandbyActivity's `failed(n/e)` log line. `n` is
  // one of these, `e` is the esp_err_t of the failing step (0 when the step
  // returns bool).
  enum class LightSleepFailure : uint8_t {
    None = 0,
    NotAllowed = 1,   // precondition rejected (no expander, timer 0, ...)
    SdPark = 2,       // SD could not be flushed/closed
    TpsSleep = 3,     // TPS65185 did not enter SLEEP
    BusHold = 4,      // reserved: the bus park API cannot report failure
    EpdPowerOff = 5,  // reserved: epd_poweroff() returns void
    IoExpMask = 6,    // expander could not be masked for sleep
    PowerWakePin = 7, // P1.7 input could not be configured/released
    IntLineLow = 8,   // GPIO2 never returned high after masking
    WakeConfig = 9,   // EXT1 ANY_LOW on GPIO2 rejected
    TimerConfig = 10, // timer wake rejected
    RailCut = 11,     // P5/P14 rail cut failed
    SleepStart = 12,  // esp_light_sleep_start() returned an error
    Restore = 13,     // a wake-restore step failed (rails/bus/SD)
    NotPorted = 14,   // lightSleep(): no generic path on this board
  };

  // True only when this board can actually park: expander alive, no USB, radio
  // idle, no background worker, and SD quiescence possible (no card, or no
  // transaction in flight; open book files are parked and reopened).
  bool canStandbyLightSleep(const HalGPIO& gpio) const;
  // Bench only: let Standby park even while USB is attached, so the caller
  // path can be reproduced without unplugging. Product code never sets this.
  static void setDebugAllowUsbSleep(bool allow);
  static bool debugUsbSleepAllowed();
  const char* debugModeMutexHolderName() const { return "(none)"; }
  // Parks the hardware, sleeps for `seconds`, restores it, and reports why it
  // woke. Never sleeps when a park step fails: everything is restored first and
  // Failed is returned (see lastLightSleepFailure()).
  LightSleepWakeReason lightSleepFor(uint32_t seconds) const;
  static uint8_t lastLightSleepFailure();
  static int lastLightSleepError();
  // Monotonic counter + reason of the last light-sleep attempt that actually
  // left the CPU (including a refused park). The UI loop appends one line to
  // /.crosspoint/power.log per new sequence, which is the only persistent
  // evidence available after a USB-less wake.
  static uint32_t wakeEventSequence();
  static uint8_t lastWakeReason();
  // Last park/sleep step reached. It lives in RTC no-init memory, so a panic
  // reset keeps the number and the next boot can say which step died.
  static uint32_t sleepStage();
  static void clearSleepStage();
  // Consumes the "a sleep returned successfully" record left in RTC memory.
  // Returns false when the last boot happened before any sleep completion
  // (including a panic inside the park).
  static bool pendingWake(uint32_t* stage, uint8_t* reason);
  // No generic "sleep a bit" path exists on this board (the vendor always goes
  // through the full park sequence above), so this reports failure.
  bool lightSleep(const HalGPIO& gpio) const;
  bool onEinkBusyWaitSlice(int8_t, uint8_t) { return false; }
  void noteMainLoopIteration() {}
  void noteRenderWaitBegin() {}
  void noteRenderWaitEnd() {}

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO &gpio) const;

  // Get battery percentage (range 0-100). Polls the gauge when the 1 s throttle
  // allows; the UI loop is its normal caller. The chrome must use
  // batteryDisplayPercent() so sampling never depends on a repaint.
  uint16_t getBatteryPercentage() const;

  // Cached display level, no sampling: safe to call from the render task.
  uint16_t batteryDisplayPercent() const;

  // False only until the first valid level has been seen in this session (or a
  // standby wake is recalibrating after a level was already shown). The chrome
  // then draws an empty capsule with no digits instead of a placeholder.
  bool batteryPercentageKnown() const;

  // True while the gauge reports a charging current and the pack is not full
  // (vendor rule: no bolt at 100 %).
  bool isBatteryCharging() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for
  // example when running a task that needs full performance. When the Lock
  // instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

  public:
    explicit Lock(bool quiet = false);
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
    Lock(Lock &&) = delete;
    Lock &operator=(Lock &&) = delete;
  };
};
