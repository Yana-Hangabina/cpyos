#pragma once

#include <cstdint>

// Horizontal-shake page-turn gate.
//
// Ported from read-pico-reader (kiikoread, Apache-2.0) main/book/book_policy.h
// (book_shake_feed). Inputs are screen-axis accelerations in mg. A slow tilt
// never fires: the gate tracks gravity and only reports an impulse that moves
// the horizontal axis by at least 210 mg while dominating both other axes.
// After a turn it stays disarmed for 800 ms, then needs 120 ms of calm before
// the next one, so a rebound cannot turn the page back.
//
// Kept free of Arduino/ESP dependencies so the host test compiles the same code
// the firmware runs.
class ShakePageTurnGate {
 public:
  // Returns +1 (forward), -1 (backward) or 0. `nowMs` is a monotonic
  // millisecond clock; a gap over 250 ms restarts the gravity baseline.
  int feed(const int xMg, const int yMg, const int zMg, const uint32_t nowMs) {
    if (!ready_ || nowMs - lastMs_ > kMaxSampleGapMs) {
      begin(xMg, yMg, zMg, nowMs);
      return 0;
    }
    lastMs_ = nowMs;

    const int dx = xMg - gravityX_;
    const int dy = yMg - gravityY_;
    const int dz = zMg - gravityZ_;
    const int ax = abs(dx);
    const int ay = abs(dy);
    const int az = abs(dz);
    const int jerk = abs(xMg - previousX_);
    previousX_ = xMg;

    if (armed_ && reached(nowMs, cooldownMs_) && ax >= kImpulseMg && jerk >= kJerkMg &&
        ax > ay * 3 / 2 + kAxisMarginMg && ax > az * 3 / 2 + kAxisMarginMg) {
      armed_ = false;
      quietMs_ = nowMs;
      cooldownMs_ = nowMs + kCooldownMs;
      return dx > 0 ? 1 : -1;
    }

    const bool quiet = ax < kQuietMg && ay < kQuietMg && az < kQuietMg;
    if (!quiet) quietMs_ = nowMs;
    if (!reached(nowMs, cooldownMs_) || ax >= kImpulseMg || ay >= kImpulseMg || az >= kImpulseMg) {
      armed_ = false;
    }
    if (quiet && reached(nowMs, cooldownMs_) && nowMs - quietMs_ >= kRearmQuietMs) {
      armed_ = true;
    }

    // Track posture while still (or during a slow tilt) so gravity itself never
    // counts as an impulse.
    if (quiet || jerk < kJerkMg) {
      gravityX_ += dx / 8;
      gravityY_ += dy / 8;
      gravityZ_ += dz / 8;
    }
    return 0;
  }

  void reset() {
    ready_ = false;
    armed_ = false;
  }

 private:
  static constexpr int kImpulseMg = 210;
  static constexpr int kJerkMg = 35;
  static constexpr int kQuietMg = 80;
  static constexpr int kAxisMarginMg = 30;
  static constexpr uint32_t kCooldownMs = 800;
  static constexpr uint32_t kRearmQuietMs = 120;
  static constexpr uint32_t kMaxSampleGapMs = 250;

  static int abs(const int v) { return v < 0 ? -v : v; }
  static bool reached(const uint32_t now, const uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
  }

  void begin(const int x, const int y, const int z, const uint32_t now) {
    ready_ = true;
    armed_ = false;
    gravityX_ = x;
    gravityY_ = y;
    gravityZ_ = z;
    previousX_ = x;
    lastMs_ = now;
    quietMs_ = now;
    cooldownMs_ = now;
  }

  bool ready_ = false;
  bool armed_ = false;
  int gravityX_ = 0;
  int gravityY_ = 0;
  int gravityZ_ = 0;
  int previousX_ = 0;
  uint32_t lastMs_ = 0;
  uint32_t quietMs_ = 0;
  uint32_t cooldownMs_ = 0;
};
