#include "plus_ai_key_sampler.h"

namespace plus_ai_key {
namespace {
constexpr uint32_t kDebounceMs = 25;
constexpr uint32_t kLongMs = 500;
}  // namespace

void Sampler::reset(const bool rawDown, const uint32_t nowMs, const bool armed) {
  rawDown_ = rawDown;
  stableDown_ = rawDown;
  longFired_ = false;
  armed_ = armed;
  rawSince_ = nowMs;
  pressMs_ = rawDown ? nowMs : 0;
}

Event Sampler::update(const bool rawDown, const uint32_t nowMs) {
  if (rawDown != rawDown_) {
    rawDown_ = rawDown;
    rawSince_ = nowMs;
  }
  if (!armed_) {
    if (!rawDown && nowMs - rawSince_ >= kDebounceMs) {
      reset(false, nowMs, true);
    }
    return Event::None;
  }
  if (nowMs - rawSince_ < kDebounceMs) {
    if (stableDown_ && !longFired_ && nowMs - pressMs_ >= kLongMs) {
      longFired_ = true;
      return Event::Long;
    }
    return Event::None;
  }
  if (rawDown == stableDown_) {
    if (stableDown_ && !longFired_ && nowMs - pressMs_ >= kLongMs) {
      longFired_ = true;
      return Event::Long;
    }
    return Event::None;
  }

  stableDown_ = rawDown;
  if (rawDown) {
    pressMs_ = nowMs;
    longFired_ = false;
    return Event::None;
  }
  if (longFired_) {
    longFired_ = false;
    return Event::None;
  }
  return (nowMs - pressMs_) >= kDebounceMs ? Event::Short : Event::None;
}

}  // namespace plus_ai_key
