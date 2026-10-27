#include "HalTiltSensor.h"

#include <BoardConfig.h>
#include <Logging.h>

HalTiltSensor halTiltSensor;  // Singleton instance

namespace {

// Map the sensor's in-plane axes to the forward/back tilt convention, which is the
// gyro's: portrait tilts about X and uses `primary`, landscape about Y and uses
// `secondary`, and the reversed motion mode flips the sign. Accelerometer boards pass
// the perpendicular axis as `primary`/`secondary` (see the call site).
float tiltAxisFor(const bool inverted, const uint8_t orientation, const float primary, const float secondary) {
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      return inverted ? -primary : primary;
    case CrossPointOrientation::INVERTED:
      return inverted ? primary : -primary;
    case CrossPointOrientation::LANDSCAPE_CW:
      return inverted ? secondary : -secondary;
    case CrossPointOrientation::LANDSCAPE_CCW:
      return inverted ? -secondary : secondary;
    default:
      return primary;
  }
}

// The shake gate is written against screen axes (horizontal = the direction a
// page turns). The board frame rotates with the reader orientation, so map the
// accelerometer sample onto horizontal/vertical/normal before feeding it.
// Only the horizontal sign matters for the direction; the other two are
// consumed as magnitudes.
void screenAxesFor(const uint8_t orientation, const Imu::Sample& sample, float& horizontal, float& vertical,
                   float& normal) {
  normal = sample.az;
  switch (orientation) {
    case CrossPointOrientation::INVERTED:
      horizontal = -sample.ax;
      vertical = -sample.ay;
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      horizontal = -sample.ay;
      vertical = sample.ax;
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      horizontal = sample.ay;
      vertical = -sample.ax;
      break;
    case CrossPointOrientation::PORTRAIT:
    default:
      horizontal = sample.ax;
      vertical = sample.ay;
      break;
  }
}

int mgFromG(const float g) { return static_cast<int>(lroundf(g * 1000.0f)); }

}  // namespace

void HalTiltSensor::begin() {
  _available = _sdkImu.begin();
  if (!_available) {
    LOG_ERR("GYR", "SDK IMU not found");
    return;
  }

  // The SC7A20H (Read Pico) is a 3-axis accelerometer with no gyroscope: read() fills
  // ax/ay/az but reports gx = gy = gz = 0, so the angular-rate gesture below could never
  // fire. The same flick is still measurable -- see RATE_THRESHOLD_GPS in the header --
  // by differentiating the gravity component, so the part is brought up like any other
  // and update() takes the accelerometer path. isAvailable() then reports true, which is
  // the gate SettingsList uses to offer "Tilt page turn".
#if FREEINK_DEVICE_READPICO
  _accelOnly = BoardConfig::ACTIVE.sensors.imuType == BoardConfig::ImuType::Sc7a20h;
#else
  _accelOnly = false;
#endif

  _initMs = millis();
  _lastPollMs = millis();
  // begin() leaves the sensors sampling; stand them by until tilt page turn actually
  // wakes them, so a disabled IMU doesn't drain the battery.
  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU standby failed");
  }
  LOG_INF("GYR", "SDK IMU initialized (%s)", _accelOnly ? "accelerometer only, gravity rate" : "gyroscope rate");
}

bool HalTiltSensor::wake() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.wake()) {
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }

  _lastPollMs = millis();
  _lastTiltMs = millis();
  _wakeMs = millis();
  _isAwake = true;
  if (_accelOnly) {
    _lastTiltGMs = 0;
    _inTilt = false;
    clearPendingEvents();
  }
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }

  clearPendingEvents();
  _inTilt = false;
  _isAwake = false;
  return true;
}

void HalTiltSensor::update(const uint8_t motionMode, const uint8_t orientation, const bool inReader) {
  if (!_available) {
    return;
  }

  const bool tiltMode = motionMode == CrossPointMotionPageTurn::TILT ||
                        motionMode == CrossPointMotionPageTurn::TILT_REVERSED;
  const bool shakeMode = motionMode == CrossPointMotionPageTurn::SHAKE ||
                         motionMode == CrossPointMotionPageTurn::SHAKE_REVERSED;
  const bool tiltActive = tiltMode && inReader;
  const bool shakeActive = shakeMode && inReader;
  const bool shouldBeAwake = tiltActive || shakeActive;
  if (motionMode != _motionMode || orientation != _accelOrientation || shouldBeAwake != _accelReading) {
    _motionMode = static_cast<CrossPointMotionPageTurn::Value>(motionMode);
    _accelOrientation = static_cast<CrossPointOrientation::Value>(orientation);
    _accelReading = shouldBeAwake;
    _lastTiltGMs = 0;
    _inTilt = false;
    _shakeGate.reset();
    clearPendingEvents();
  }
  if (shouldBeAwake && !_isAwake) {
    _isAwake = wake();
    return;
  }
  if (!shouldBeAwake && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  if (!shouldBeAwake) {
    return;
  }

  const unsigned long now = millis();
  // Stabilization: discard readings during gyro startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  const unsigned long pollInterval = _accelOnly ? ACCEL_POLL_INTERVAL_MS : POLL_INTERVAL_MS;
  if ((now - _lastPollMs) < pollInterval) {
    return;
  }
  _lastPollMs = now;

  Imu::Sample sample;
  if (!_sdkImu.read(sample)) {
    _lastTiltGMs = 0;
    return;
  }

  if (shakeActive) {
    const int shake = feedShake(sample, motionMode, orientation, now);
    if (shake > 0) {
      _motionForwardEvent = true;
      _hadActivity = true;
      LOG_INF("GYR", "Shake trigger: forward");
    } else if (shake < 0) {
      _motionBackEvent = true;
      _hadActivity = true;
      LOG_INF("GYR", "Shake trigger: backward");
    }
  }

  if (!tiltActive) {
    return;
  }

  // Gyro boards read an angular rate directly; accelerometer boards differentiate the
  // gravity component, so the two arrive here in different units and against thresholds
  // converted from the same 270 dps. See RATE_THRESHOLD_GPS in the header.
  float tiltAxis;
  float rateThreshold;
  float neutralRate;
  if (_accelOnly) {
    // The gyro measures angular rate ABOUT the tilt axis; the accelerometer measures the
    // gravity component ALONG the axis perpendicular to it. The gesture is a LEFT/RIGHT
    // sway, so in portrait the device rolls about its up/down axis and gravity swings
    // along the left/right axis (ax); landscape swaps which axis is which, so it swings
    // along ay. Mapping the gyro axis straight across would fire on the wrong motion.
    //
    // The sign is flipped because the accelerometer's gravity swing runs opposite to the
    // gyro's rotation rate for the same physical sway; flipping here rather than inside
    // tiltAxisFor() leaves the gyro boards' convention exactly as it was.
    const float tiltSample = -tiltAxisFor(motionMode == CrossPointMotionPageTurn::TILT_REVERSED, orientation,
                                          sample.ax, sample.ay);
    if (_lastTiltGMs == 0) {
      _lastTiltG = tiltSample;
      _lastTiltGMs = now;
      return;
    }
    tiltAxis = (tiltSample - _lastTiltG) * 1000.0f / static_cast<float>(now - _lastTiltGMs);
    _lastTiltG = tiltSample;
    _lastTiltGMs = now;
    rateThreshold = RATE_THRESHOLD_GPS;
    neutralRate = NEUTRAL_RATE_GPS;
  } else {
    // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
    tiltAxis = tiltAxisFor(motionMode == CrossPointMotionPageTurn::TILT_REVERSED, orientation, sample.gx, sample.gy);
    rateThreshold = RATE_THRESHOLD_DPS;
    neutralRate = NEUTRAL_RATE_DPS;
  }

  if (_inTilt) {
    // Wait for device to return to neutral before allowing next trigger
    if (fabsf(tiltAxis) < neutralRate) {
      _inTilt = false;
    }
  } else {
    // Check for new tilt gesture (with cooldown)
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > rateThreshold) {
        _motionForwardEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Forward Trigger=(%.1f) %s", tiltAxis, _accelOnly ? "g/s" : "dps");
      } else if (tiltAxis < -rateThreshold) {
        _motionBackEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Backward Trigger=(%.1f) %s", tiltAxis, _accelOnly ? "g/s" : "dps");
      }
    }
  }
}

int HalTiltSensor::feedShake(const Imu::Sample& sample, const uint8_t motionMode, const uint8_t orientation,
                             const unsigned long now) {
  float horizontal;
  float vertical;
  float normal;
  screenAxesFor(orientation, sample, horizontal, vertical, normal);
  const bool inverted = motionMode == CrossPointMotionPageTurn::SHAKE_REVERSED;
  const int x = mgFromG(inverted ? -horizontal : horizontal);
  return _shakeGate.feed(x, mgFromG(vertical), mgFromG(normal), static_cast<uint32_t>(now));
}

bool HalTiltSensor::wasMotionForward() {
  const bool val = _motionForwardEvent;
  _motionForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasMotionBack() {
  const bool val = _motionBackEvent;
  _motionBackEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _motionForwardEvent = false;
  _motionBackEvent = false;
  _hadActivity = false;
  _shakeGate.reset();
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}
