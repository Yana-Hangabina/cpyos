#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect lock mode, clock transitions, and isLowPower

 public:
  enum class LightSleepWakeReason : uint8_t {
    Timer,        // the requested sleep window elapsed
    PowerButton,  // the PMU key produced an event
    Failed
  };

#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // True when this board can run a light-sleep cycle at all. Only the
  // runtime-selected X3 profiles and Read Pico have validated standby wake
  // wiring; every other target reports false. Read Pico's is valid whenever the
  // FCA9555 came up, and its wake set is armed in HalPowerManager.cpp (IOE INT#
  // GPIO41, low level) rather than from a GPIO power pin.
  bool canStandbyLightSleep(const HalGPIO& gpio) const;

  // Enter one light-sleep cycle. GPIO and timer wake sources are removed before returning.
  LightSleepWakeReason lightSleepFor(uint32_t seconds) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // Raw pack telemetry for diagnostics (mV / PMU SOC / charge state). `valid`
  // is false on boards without a PMU or when the read/CRC fails.
  struct BatteryTelemetry {
    bool valid = false;
    uint16_t millivolts = 0;
    uint16_t socPermille = 0;
    uint8_t chargeState = 0;
  };
  BatteryTelemetry readBatteryTelemetry() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;
    bool quiet_ = false;

   public:
    // `quiet` suppresses the "already held" error. Callers that can tolerate
    // running without the lock (a network request competing with a render pass)
    // use it instead of adding an error line to every collision.
    explicit Lock(bool quiet = false);
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
