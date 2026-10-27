#pragma once

#include <Arduino.h>
#include <Imu.h>

#include "ShakePageTurnGate.h"

// TODO: Move enums into new header and share with CrossPointSettings.h
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointMotionPageTurn {
enum Value : uint8_t { OFF = 0, TILT = 1, TILT_REVERSED = 2, SHAKE = 3, SHAKE_REVERSED = 4 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

class HalTiltSensor {
  bool _available = false;
  mutable Imu _sdkImu;

  // Motion gesture state machine
  bool _motionForwardEvent = false;  // Consumed by wasMotionForward()
  bool _motionBackEvent = false;     // Consumed by wasMotionBack()
  bool _hadActivity = false;       // Non-consuming flag for sleep timer
  bool _inTilt = false;            // Currently tilted past threshold
  bool _isAwake = false;           // Tracks power state
  unsigned long _initMs = 0;       // Timestamp of sensor init
  unsigned long _lastTiltMs = 0;   // Debounce / cooldown
  unsigned long _wakeMs = 0;       // Timestamp of last wake() for stabilization

  // Tuning constants
  static constexpr float RATE_THRESHOLD_DPS = 270.0f;  // Deg/sec speed to trigger flick
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;     // Must stop moving below this rate before next trigger
  // Boards whose IMU has no gyroscope (SC7A20H, Read Pico) drive the same gesture from
  // the accelerometer's gravity component. Rotating about the tilt axis gives
  // a = g*sin(theta), so da/dt = g*cos(theta)*omega and at small angles da/dt = g*omega.
  // Dividing by g cancels it, so 1 dps == pi/180 g/s and the gyro thresholds convert
  // exactly: 270 dps -> 4.71 g/s, 50 dps -> 0.87 g/s. Same flick, same feel.
  static constexpr float DPS_TO_GPS = 0.0174533f;
  static constexpr float RATE_THRESHOLD_GPS = RATE_THRESHOLD_DPS * DPS_TO_GPS;
  static constexpr float NEUTRAL_RATE_GPS = NEUTRAL_RATE_DPS * DPS_TO_GPS;
  static constexpr unsigned long COOLDOWN_MS = 600;            // Minimum ms between triggers
  static constexpr unsigned long POLL_INTERVAL_MS = 50;        // 20 Hz polling
  static constexpr unsigned long ACCEL_POLL_INTERVAL_MS = 80;  // SC7A20H samples at 12.5 Hz
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;      // Ignore readings after wake

  mutable unsigned long _lastPollMs = 0;

  // Set when the active IMU is an accelerometer with no gyroscope, so the tilt axis is
  // differentiated from gravity instead of read as an angular rate.
  bool _accelOnly = false;
  float _lastTiltG = 0.0f;  // Previous gravity-component sample, for the rate
  unsigned long _lastTiltGMs = 0;
  CrossPointMotionPageTurn::Value _motionMode = CrossPointMotionPageTurn::OFF;
  CrossPointOrientation::Value _accelOrientation = CrossPointOrientation::PORTRAIT;
  bool _accelReading = false;

  // Shake page-turn gate (kiikoread book_policy.h): independent of the tilt gesture
  // and fed from the same accelerometer samples.
  ShakePageTurnGate _shakeGate;

  int feedShake(const Imu::Sample& sample, const uint8_t motionMode, const uint8_t orientation, const unsigned long now);

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // Enables tilt polling state
  bool wake();

  // Puts tilt polling state to sleep
  bool deepSleep();

  // True if an IMU is present on this device
  bool isAvailable() const { return _available; }

  // Poll the IMU and update the motion page-turn gesture state.
  // `motionMode` is CrossPointMotionPageTurn::Value, `orientation`
  // CrossPointOrientation::Value.
  void update(const uint8_t motionMode, const uint8_t orientation, const bool inReader);

  // Returns true once per forward motion gesture (next page direction).
  // Consumed on read — subsequent calls return false until next gesture.
  bool wasMotionForward();

  // Returns true once per backward motion gesture (previous page direction).
  // Consumed on read.
  bool wasMotionBack();

  // Non-consuming: true if any tilt activity occurred since last call.
  // Used to reset the auto-sleep inactivity timer.
  bool hadActivity();

  // Discard any pending tilt events (call when leaving reader or disabling tilt).
  void clearPendingEvents();
};
