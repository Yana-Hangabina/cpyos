#pragma once

#include <cstdint>

#include "plus_ai_key.h"

// Pure short/long classifier for the Metalio Plus AI/BOOT key (GPIO61).
//
// Split out of plus_ai_key.cpp so the vendor/kiikoread timing rules can be
// exercised without a board: 25 ms debounce, 500 ms long threshold, a release
// after a long hold never produces Short, and a key already held at boot is
// ignored until it has been released once.
namespace plus_ai_key {

class Sampler {
 public:
  // armed=false means "this press powered the unit on": wait for a released
  // sample before accepting gestures.
  void reset(bool rawDown, uint32_t nowMs, bool armed);

  // Feed one raw sample (true = pressed). Call at least every 100 ms.
  Event update(bool rawDown, uint32_t nowMs);

  bool stableDown() const { return stableDown_; }
  bool armed() const { return armed_; }

 private:
  bool rawDown_ = false;
  bool stableDown_ = false;
  bool longFired_ = false;
  bool armed_ = false;
  uint32_t rawSince_ = 0;
  uint32_t pressMs_ = 0;
};

}  // namespace plus_ai_key
