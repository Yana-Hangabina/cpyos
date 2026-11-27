#pragma once

// Host double for <esp_cpu.h>: the firmware reads the current stack pointer to
// decide whether it lives in PSRAM.  The test moves it by hand.
#include <cstdint>

namespace stacktest {
inline uint32_t& stackPointer() {
  static uint32_t value = 0;
  return value;
}
}  // namespace stacktest

inline uint32_t esp_cpu_get_sp() { return stacktest::stackPointer(); }
