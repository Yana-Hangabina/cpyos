#pragma once

// Host double for <esp_memory_utils.h>.  The address windows mirror the real
// ESP32-S3 map (PSRAM is mapped at 0x3C000000, internal SRAM at 0x3FC00000) so
// the boundary the guard is tested against is the boundary the hardware has.
//
// What this CANNOT prove: that a real PSRAM-stack task's SP actually lands in
// that window, or that IDF's own cache-disable check agrees with
// esp_ptr_external_ram().  Those are hardware facts; the device-side proof is
// CMD:CRASHTEST (bench build), see run_stack_safety_test.sh.
#include <cstdint>

#include "esp_cpu.h"

namespace stacktest {
constexpr uint32_t kPsramBase = 0x3C000000u;
constexpr uint32_t kPsramEnd = 0x3E000000u;    // exclusive
constexpr uint32_t kDramBase = 0x3FC00000u;
constexpr uint32_t kDramEnd = 0x3FD00000u;     // exclusive
}  // namespace stacktest

inline bool esp_ptr_external_ram(const void* address) {
  const uint32_t raw = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address));
  return raw >= stacktest::kPsramBase && raw < stacktest::kPsramEnd;
}
