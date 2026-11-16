#pragma once

#include <cstdint>

namespace weread_time_sync {

// A gap longer than this between samples means the reader left the book
// (screen sleep, home, standby) instead of reading: idle gaps are not counted.
constexpr uint32_t kMaxGapMs = 60000;
// Periodic settle/upload cadence while a book is open.
constexpr uint32_t kTickMs = 30000;
// Below this the accumulated seconds are not worth a network request.
constexpr uint32_t kMinUploadSeconds = 60;

// Accumulates effective reading milliseconds from periodic samples. Pure
// logic with no HAL dependency so host tests can drive the clock.
class ReadingClock {
 public:
  void begin(const uint64_t nowMs) {
    lastSampleMs_ = nowMs;
    lastTickMs_ = nowMs;
    pendingMs_ = 0;
    started_ = true;
  }
  void stop() { started_ = false; }
  bool started() const { return started_; }

  // Adds the gap since the previous sample. Gaps longer than kMaxGapMs are
  // discarded instead of counted. Returns true when time was accumulated.
  bool sample(const uint64_t nowMs) {
    if (!started_) return false;
    const uint64_t delta = nowMs >= lastSampleMs_ ? nowMs - lastSampleMs_ : 0;
    lastSampleMs_ = nowMs;
    if (delta == 0 || delta > kMaxGapMs) return false;
    pendingMs_ += delta;
    return true;
  }

  // Moves the sample cursor without counting the elapsed gap (menu, overlay).
  void skipTo(const uint64_t nowMs) {
    if (started_) lastSampleMs_ = nowMs;
  }

  uint32_t pendingMs() const { return static_cast<uint32_t>(pendingMs_); }

  // Removes and returns the whole seconds accumulated so far. The sub-second
  // remainder stays in the clock so repeated flushes do not lose time.
  uint32_t takeSeconds() {
    const uint32_t seconds = static_cast<uint32_t>(pendingMs_ / 1000ULL);
    if (seconds > 0) pendingMs_ -= static_cast<uint64_t>(seconds) * 1000ULL;
    return seconds;
  }

  bool tickDue(const uint64_t nowMs) const {
    return started_ && nowMs >= lastTickMs_ && nowMs - lastTickMs_ >= kTickMs;
  }
  void markTick(const uint64_t nowMs) { lastTickMs_ = nowMs; }

 private:
  uint64_t lastSampleMs_ = 0;
  uint64_t lastTickMs_ = 0;
  uint64_t pendingMs_ = 0;
  bool started_ = false;
};

}  // namespace weread_time_sync
