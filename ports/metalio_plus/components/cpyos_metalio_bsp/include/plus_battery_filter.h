#pragma once
// Battery percentage filter for the Metalio Plus.
//
// The BQ27220 reports whole percent, and the voltage fallback steps by 1 % per
// 10 mV, so a reading parked on a boundary flickers between two values every
// poll. The vendor firmware applies a 5 % hysteresis to its voltage-derived
// state of charge; this port prefers the gauge's own SOC and applies a small
// (2 point) hysteresis plus hard snaps at the ends, which keeps the status bar
// steady without hiding a real drop. Pure logic so it can be host-tested.
#include <cstdlib>

namespace plus_battery {

class PercentFilter {
 public:
  static constexpr int kHysteresis = 2;

  int update(int rawPercent) {
    if (rawPercent < 0) rawPercent = 0;
    if (rawPercent > 100) rawPercent = 100;
    if (shown_ < 0) {
      shown_ = rawPercent;
      return shown_;
    }
    // Snap at the ends so "full" and a critical pack are never masked by the
    // dead band.
    if (rawPercent <= 3 || rawPercent >= 99) {
      shown_ = rawPercent;
      return shown_;
    }
    if (std::abs(rawPercent - shown_) >= kHysteresis) shown_ = rawPercent;
    return shown_;
  }

  int shown() const { return shown_; }

 private:
  int shown_ = -1;
};

}  // namespace plus_battery
