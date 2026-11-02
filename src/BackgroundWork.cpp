#include "BackgroundWork.h"

namespace background_work {
namespace {
std::atomic<uint16_t> activeCount{0};
}  // namespace

void begin() {
  uint16_t current = activeCount.load(std::memory_order_relaxed);
  while (current != UINT16_MAX) {
    if (activeCount.compare_exchange_weak(current, static_cast<uint16_t>(current + 1), std::memory_order_relaxed)) {
      return;
    }
  }
}

void end() {
  uint16_t current = activeCount.load(std::memory_order_relaxed);
  while (current != 0) {
    if (activeCount.compare_exchange_weak(current, static_cast<uint16_t>(current - 1), std::memory_order_relaxed)) {
      return;
    }
  }
}

bool active() { return activeCount.load(std::memory_order_relaxed) != 0; }

}  // namespace background_work
