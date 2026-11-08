#include <HalTiltSensor.h>

#include <cassert>

unsigned long clockMs = 1000;
Imu::Sample Imu::sample{};
int Imu::reads = 0;
bool Imu::readOk = true;
bool Imu::hasGyro = HAS_GYRO != 0;
bool Imu::beginOk = true;
bool Imu::sleepCalled = false;
bool Imu::capabilityQueriedBeforeSleep = false;

int main() {
  HalTiltSensor sensor;
  sensor.begin();
  // A part without a gyroscope is still a usable motion source: begin() succeeding is
  // what isAvailable() reports, and the gesture falls back to the gravity derivative.
  assert(sensor.isAvailable());
  assert(!Imu::capabilityQueriedBeforeSleep);

  auto update = [&](unsigned long time, uint8_t mode = CrossPointMotionPageTurn::TILT,
                    uint8_t orientation = CrossPointOrientation::PORTRAIT, bool reader = true) {
    clockMs = time;
    sensor.update(mode, orientation, reader);
  };
  auto noEvent = [&] { assert(!sensor.wasMotionForward() && !sensor.wasMotionBack()); };
  update(1000);

  if (!Imu::hasGyro) {
    // Accelerometer-only: the tilt axis is the differentiated gravity component and the
    // sample cadence is the part's (80 ms), not the 50 ms gyro cadence.
    Imu::sample.ax = 1;
    update(1700);  // first sample after wake establishes the baseline
    noEvent();
    const int first = Imu::reads;
    update(1750);  // sensor has not reached its next 80 ms sample
    assert(Imu::reads == first);
    Imu::sample.ax = 0.5f;
    update(1780);
    assert(sensor.wasMotionForward());
    noEvent();     // one event per flick
    update(1860);  // repeated gravity sample is neutral
    noEvent();
    Imu::sample.ay = -1;
    update(2600, CrossPointMotionPageTurn::TILT, CrossPointOrientation::LANDSCAPE_CW);
    noEvent();  // changing axis must not differentiate it against the previous axis
    update(2680, CrossPointMotionPageTurn::TILT_REVERSED, CrossPointOrientation::LANDSCAPE_CW);
    noEvent();
    update(2800, CrossPointMotionPageTurn::TILT, CrossPointOrientation::PORTRAIT, false);
    Imu::sample.ax = -1;
    update(4000);
    update(4700);
    noEvent();  // changed pose while asleep is a new baseline
    Imu::sample.ax = -0.5f;
    update(4780);
    assert(sensor.wasMotionBack());
    update(4860);
    Imu::readOk = false;
    update(6000);
    Imu::readOk = true;
    Imu::sample.ax = 1;
    update(6080);
    noEvent();  // a missing sample invalidates the derivative baseline
    update(7000, CrossPointMotionPageTurn::OFF);
    Imu::sample.ax = -1;
    update(8000);
    update(8700);
    noEvent();
  } else {
    // Gyroscope part: unchanged angular-rate path and 50 ms cadence.
    Imu::sample.gx = 400;
    update(1700);
    assert(sensor.wasMotionForward());
    const int first = Imu::reads;
    update(1749);
    assert(Imu::reads == first);
    Imu::sample.gx = 0;
    update(1750);
    assert(Imu::reads == first + 1);  // retain existing 50 ms gyro cadence
    Imu::sample.gx = -400;
    update(2400);
    assert(sensor.wasMotionBack());
    noEvent();
  }

  // begin() failing still means "no motion source", exactly as before.
  Imu::beginOk = false;
  Imu::sleepCalled = false;
  HalTiltSensor absent;
  absent.begin();
  assert(!absent.isAvailable());
}
