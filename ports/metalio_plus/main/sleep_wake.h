#pragma once

#include <cstdint>
#include <esp_sleep.h>

// IDF 6.1 deprecates the single-cause getter in favour of a bitmask. The port
// only reports one cause, so take the lowest set bit (causes are sequential
// enum values and the mask uses 1 << cause).
inline esp_sleep_wakeup_cause_t plusFirstWakeupCause() {
  uint32_t mask = esp_sleep_get_wakeup_causes();
  if (mask == 0) return ESP_SLEEP_WAKEUP_UNDEFINED;
  int index = 0;
  while ((mask & 1u) == 0 && index < 31) {
    mask >>= 1;
    ++index;
  }
  return static_cast<esp_sleep_wakeup_cause_t>(index);
}
