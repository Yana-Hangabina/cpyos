#pragma once

#include <cstdint>
#include <string>

// The queue half of the bottom-capsule toast: no renderer, no theme, no HAL, so
// it can be exercised by a host test. Toast.h adds the painting.
//
// Why a queue: a job can emit several acknowledgements back to back (deleted,
// cached, queued). Overwriting the first one dropped messages silently. Three
// slots is enough for that burst and small enough to stay inside the PSRAM
// budget; duplicates of the visible message refresh its timer instead of
// stacking a second identical capsule the user cannot tell apart.
class ToastQueue {
 public:
  static constexpr uint32_t kDefaultMs = 1600;
  static constexpr int kMaxQueued = 3;

  void show(const char* text, const uint32_t nowMs, const uint32_t durationMs = kDefaultMs) {
    if (text == nullptr || text[0] == '\0') return;
    if (current_ == text) {
      untilMs_ = nowMs + durationMs;
      return;
    }
    if (current_.empty()) {
      current_ = text;
      untilMs_ = nowMs + durationMs;
      return;
    }
    if (count_ >= kMaxQueued) {
      // Drop the oldest waiting message: the newest one is what the user just
      // made happen, and the queue is a burst buffer, not a log.
      head_ = (head_ + 1) % kMaxQueued;
      --count_;
    }
    queue_[(head_ + count_) % kMaxQueued] = text;
    ++count_;
  }

  // Drops the visible message once it expires and promotes the next one.
  // Returns true when the frame should be repainted.
  bool tick(const uint32_t nowMs) {
    if (!current_.empty() && static_cast<int32_t>(nowMs - untilMs_) >= 0) {
      if (count_ > 0) {
        current_ = queue_[head_];
        head_ = (head_ + 1) % kMaxQueued;
        --count_;
        untilMs_ = nowMs + kDefaultMs;
      } else {
        current_.clear();
        untilMs_ = 0;
      }
      return true;
    }
    return false;
  }

  bool active(const uint32_t nowMs) const {
    return !current_.empty() && static_cast<int32_t>(nowMs - untilMs_) < 0;
  }

  void clear() {
    current_.clear();
    count_ = 0;
    head_ = 0;
    untilMs_ = 0;
  }

  const std::string& text() const { return current_; }
  int pending() const { return count_; }

 private:
  std::string current_;
  std::string queue_[kMaxQueued];
  int head_ = 0;
  int count_ = 0;
  uint32_t untilMs_ = 0;
};
