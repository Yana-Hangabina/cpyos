#pragma once
// Battery state-of-charge display policy for the Metalio E-INK4 Plus.
//
// Ported from the vendor firmware (main/boards/common/power/bq27220_gauge.* and
// docs/battery-soc-and-charge.md). The vendor deliberately does not use the
// BQ27220's own SOC register (uncalibrated for this cell); it reads voltage and
// current and applies:
//
//   * discharging: the measured 424860-2000 discharge run (10 % points,
//     piecewise linear); charging: a linear 3.30-4.30 V estimate;
//   * a 60-sample moving average (one accepted sample per second) -> target;
//   * target capped at 95 % until the charger confirms full (>= 4.30 V for
//     30 s continuously), and held at 100 % while the pack stays above 4.28 V;
//   * 5 % quantisation with a one-way step of at most 5 % every 30 s, so the
//     status number never flickers; inserting/removing power restarts that
//     timer and clears the averaging window;
//   * a 3 s settle window after boot or a standby wake, during which the level
//     is reported unknown ("--") instead of a stale or fabricated number;
//   * charging = current > +5 mA and level < 100 (no bolt on a full pack).
//
// The state is pure logic so a host test can drive it; the gauge I/O stays in
// HalPowerManager.cpp.

#include <cstdint>

namespace plus_battery {

class SocDisplayPolicy {
 public:
  static constexpr uint16_t kEmptyMv = 3300;
  static constexpr uint16_t kFullConfirmMv = 4300;
  static constexpr uint16_t kFullExitMv = 4280;
  static constexpr int kStepPct = 5;
  static constexpr uint32_t kStepIntervalMs = 30000;
  static constexpr uint32_t kFullConfirmMs = 30000;
  static constexpr uint32_t kSettleMs = 3000;
  static constexpr uint32_t kMaxSampleGapMs = 2000;
  static constexpr int kFilterSize = 60;
  static constexpr int kChargeDeadbandMa = 5;
  static constexpr int kFailuresToUnknown = 3;
  // 424860-2000 step 6: about 1000 mA discharge to 3.3 V, normalised over the
  // 1870.7 mAh actually delivered; one point per 10 %.
  static constexpr uint16_t kDischargeMv[11] = {3300, 3481, 3565, 3624, 3663, 3702,
                                                3757, 3828, 3919, 4021, 4211};

  // Instantaneous estimate, matching Bq27220Gauge::VoltageToPercent().
  static float voltageToPercent(uint16_t mv, bool charging) {
    if (mv <= kEmptyMv) return 0.0f;
    if (charging) {
      if (mv >= kFullConfirmMv) return 100.0f;
      return static_cast<float>(mv - kEmptyMv) * 100.0f / static_cast<float>(kFullConfirmMv - kEmptyMv);
    }
    for (int i = 1; i < 11; ++i) {
      if (mv <= kDischargeMv[i]) {
        return static_cast<float>(i - 1) * 10.0f +
               static_cast<float>(mv - kDischargeMv[i - 1]) * 10.0f /
                   static_cast<float>(kDischargeMv[i] - kDischargeMv[i - 1]);
      }
    }
    return 100.0f;
  }

  // Boot: forget everything, wait for the load to settle, then recalibrate.
  void reset(uint32_t nowMs) { start(nowMs, /*keepFullConfirm=*/false); }

  // Standby wake: the pack may have charged or drained while the panel was
  // parked, so the displayed value is recalibrated (multi-step jump allowed)
  // after the settle window. The confirmed-full hysteresis survives the sleep,
  // as in the vendor firmware.
  void resetForWake(uint32_t nowMs) { start(nowMs, /*keepFullConfirm=*/true); }

  // One successful gauge sample (called at about 1 Hz).
  void update(uint16_t mv, int32_t ma, uint32_t nowMs) {
    if (static_cast<int32_t>(nowMs - readyAtMs_) < 0) return;  // settling

    errors_ = 0;
    const bool rawCharging = ma > kChargeDeadbandMa;
    const bool rawDischarging = ma < -kChargeDeadbandMa;
    const int8_t direction = rawCharging ? 1 : (rawDischarging ? -1 : 0);

    // Any charge/discharge/idle transition (typically plugging or unplugging
    // USB) restarts the step timer; switching between the charging and
    // discharging curves also clears the averaging window so the two are never
    // mixed, and the first post-change step cannot fire immediately.
    if (direction != direction_) {
      direction_ = direction;
      lastStepMs_ = nowMs;
      ringCount_ = 0;
      ringNext_ = 0;
      ringSum_ = 0.0f;
    }
    chargingMode_ = rawCharging;

    // Full-charge confirmation needs 30 s of continuous >= 4.30 V; a gap in the
    // samples breaks the "continuous" claim.
    const uint32_t sinceSample = nowMs - lastSampleMs_;
    if (nowMs != 0 && sinceSample > kMaxSampleGapMs) fullSinceMs_ = 0;
    if (mv >= kFullConfirmMv) {
      if (fullSinceMs_ == 0) fullSinceMs_ = nowMs;
      if ((nowMs - fullSinceMs_) >= kFullConfirmMs) fullConfirmed_ = true;
    } else {
      fullSinceMs_ = 0;
      if (mv < kFullExitMv) fullConfirmed_ = false;
    }

    if (nowMs != 0 && lastSampleMs_ != 0 && sinceSample < 500 && ringCount_ > 0) {
      // The poll loop may call faster than 1 Hz; keep the averaging window at
      // one sample per second like the vendor.
      charging_ = rawCharging && shown_ < 100;
      discharging_ = rawDischarging;
      return;
    }
    lastSampleMs_ = nowMs;

    const float sample = voltageToPercent(mv, chargingMode_);
    if (ringCount_ < kFilterSize) {
      ++ringCount_;
    } else {
      // Full ring: the slot about to be overwritten holds the oldest sample.
      ringSum_ -= ring_[ringNext_];
    }
    ring_[ringNext_] = sample;
    ringSum_ += sample;
    ringNext_ = (ringNext_ + 1) % kFilterSize;

    float target = ringSum_ / static_cast<float>(ringCount_);
    if (fullConfirmed_) {
      target = 100.0f;
    } else if (target > 95.0f) {
      target = 95.0f;  // only the charger's full confirmation may reach 100
    }
    int targetPct = static_cast<int>(target);
    targetPct = (targetPct / kStepPct) * kStepPct;

    if (shown_ < 0) {
      // First calibration after boot/wake may cross several steps at once.
      shown_ = targetPct;
      lastStepMs_ = nowMs;
      known_ = true;
    } else if ((nowMs - lastStepMs_) >= kStepIntervalMs) {
      if (chargingMode_) {
        if (targetPct > shown_) {
          shown_ = (shown_ + kStepPct > targetPct) ? targetPct : shown_ + kStepPct;
          lastStepMs_ = nowMs;
        }
      } else if (targetPct < shown_) {
        shown_ = (shown_ - kStepPct < targetPct) ? targetPct : shown_ - kStepPct;
        lastStepMs_ = nowMs;
      }
    }
    known_ = true;
    charging_ = rawCharging && shown_ < 100;
    discharging_ = rawDischarging;
  }

  // The gauge did not answer. One or two misses keep the last value; three in a
  // row mark it unknown so the chrome shows "--" instead of a stale number.
  void noteReadFailure() {
    if (errors_ < 255) ++errors_;
    known_ = known_ && errors_ < kFailuresToUnknown;
  }

  bool known() const { return known_; }
  int level() const { return shown_ < 0 ? 0 : shown_; }
  bool charging() const { return charging_; }
  bool discharging() const { return discharging_; }

 private:
  void start(uint32_t nowMs, bool keepFullConfirm) {
    shown_ = -1;
    ringCount_ = 0;
    ringNext_ = 0;
    ringSum_ = 0.0f;
    lastSampleMs_ = nowMs;
    lastStepMs_ = nowMs;
    fullSinceMs_ = 0;
    if (!keepFullConfirm) fullConfirmed_ = false;
    chargingMode_ = false;
    direction_ = 0;
    charging_ = false;
    discharging_ = false;
    known_ = false;
    errors_ = 0;
    readyAtMs_ = nowMs + kSettleMs;
  }

  int shown_ = -1;
  uint32_t readyAtMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint32_t lastStepMs_ = 0;
  uint32_t fullSinceMs_ = 0;
  bool fullConfirmed_ = false;
  bool chargingMode_ = false;
  int8_t direction_ = 0;
  bool charging_ = false;
  bool discharging_ = false;
  bool known_ = false;
  uint8_t errors_ = 0;
  float ring_[kFilterSize] = {};
  int ringCount_ = 0;
  int ringNext_ = 0;
  float ringSum_ = 0.0f;
};

}  // namespace plus_battery
