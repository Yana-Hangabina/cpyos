#pragma once

// Display-side state-of-charge filter for the Read Pico PMU.
//
// Ported from kiikoread rc110 (components/read_pico_pmu/pico_soc_filter.c,
// Apache-2.0). The CW32L010 already derives an SoC from its open-circuit
// voltage table (monotonic down when idle, held while charging, rebased on
// charge end or a >500 mV jump) and documents about +/-10 points of accuracy,
// so the host has nothing to gain from a second estimator. What it can do is
// keep the number steady and honest:
//
//   * the median of five one-second samples rejects single-sample wobble;
//   * above 85 % an unplugged pack only descends at about one point per second
//     of real sample time, so the voltage sag of a load step is not shown as
//     charge disappearing (and no debt is replayed after sleep or an outage);
//   * low, clearly-empty or PMU-confirmed-full samples bypass the filter;
//   * a powered/unpowered transition restarts the burst, and a gap of more than
//     a minute re-seeds from the fresh sample.
//
// Pure logic: no Arduino, no I2C, so the host test can drive it directly.

#include <cstdint>

class BatterySocFilter {
 public:
  struct Inputs {
    uint16_t socPermille = 0;  // PMU state of charge, 0..1000
    uint16_t millivolts = 0;   // pack voltage from the same PMU frame
    bool charging = false;     // PMU charge_state == charging
    bool full = false;         // PMU charge_state == full
    bool low = false;          // PMU low/critical flag or a clearly low pack
  };

  // A gap longer than this means the previous history is meaningless (sleep,
  // a stalled I2C bus); re-seed instead of smoothing across it.
  static constexpr uint32_t kSampleGapMs = 60000;
  // One accepted sample per second, like the source implementation.
  static constexpr uint32_t kMinSampleSpacingMs = 1000;

  uint16_t update(const Inputs& in, uint32_t nowMs);

  uint16_t shownPermille() const { return shown_; }
  bool valid() const { return valid_; }

 private:
  uint16_t shown_ = 0;
  uint32_t sampledMs_ = 0;
  uint32_t changedMs_ = 0;
  uint32_t budgetMs_ = 0;
  uint16_t samples_[5] = {};
  uint8_t next_ = 0;
  uint8_t count_ = 0;
  bool powered_ = false;
  bool valid_ = false;
};

inline uint16_t BatterySocFilter::update(const Inputs& in, const uint32_t nowMs) {
  const uint16_t raw = in.socPermille;
  if (raw > 1000) return raw;

  if (!valid_ || (nowMs - sampledMs_) > kSampleGapMs) {
    shown_ = raw;
    sampledMs_ = nowMs;
    changedMs_ = nowMs;
    powered_ = in.charging;
    count_ = 0;
    next_ = 0;
    budgetMs_ = 0;
    valid_ = true;
  }

  if (powered_ != in.charging) {
    powered_ = in.charging;
    changedMs_ = nowMs;
    count_ = 0;
    next_ = 0;
    budgetMs_ = 0;
  }

  // Never delay a reading that says the pack is low, essentially empty, or
  // confirmed full by the PMU itself.
  if (in.low || raw <= 200 || (in.millivolts != 0 && in.millivolts <= 3500) || (in.full && raw >= 995)) {
    shown_ = raw;
    sampledMs_ = nowMs;
    count_ = 0;
    next_ = 0;
    budgetMs_ = 0;
    return raw;
  }

  const uint32_t elapsed = nowMs - sampledMs_;
  if (elapsed < kMinSampleSpacingMs) return shown_;

  sampledMs_ = nowMs;
  samples_[next_] = raw;
  next_ = static_cast<uint8_t>((next_ + 1) % 5);
  if (count_ < 5) ++count_;

  uint16_t sorted[5];
  for (uint8_t i = 0; i < count_; ++i) sorted[i] = samples_[i];
  for (uint8_t i = 1; i < count_; ++i) {
    const uint16_t value = sorted[i];
    uint8_t j = i;
    while (j > 0 && sorted[j - 1] > value) {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = value;
  }
  const uint16_t target = sorted[count_ / 2];

  // Unplugged and in the upper range: a rising sample is the pack recovering
  // from load, not charge appearing.
  if (!powered_ && shown_ >= 850 && raw >= 800 && in.millivolts >= 3900 && target > shown_) {
    budgetMs_ = 0;
    return shown_;
  }

  const bool upper = shown_ >= 850 && raw >= 800 && target >= 800 && shown_ >= target &&
                     (shown_ - raw) <= 150 && (shown_ - target) <= 150 && in.millivolts >= 3900;
  if (!upper) {
    shown_ = raw;
    budgetMs_ = 0;
    return raw;
  }

  // Bounded descent: hold for 30 s after a power transition, then spend at most
  // one point per second of accumulated sample time.
  if ((nowMs - changedMs_) < 30000 || count_ < 3) {
    budgetMs_ = 0;
    return shown_;
  }
  if (target == shown_) {
    budgetMs_ = 0;
    return shown_;
  }
  budgetMs_ += elapsed > 2000 ? 2000 : elapsed;
  const uint16_t step = static_cast<uint16_t>(budgetMs_ / 1000);
  budgetMs_ %= 1000;
  const uint16_t drop = static_cast<uint16_t>(shown_ - target);
  shown_ = static_cast<uint16_t>(shown_ - (drop < step ? drop : step));
  return shown_;
}
