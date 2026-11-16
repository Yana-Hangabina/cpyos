#pragma once

#include <cstdint>

namespace freeink {

// Matches the data-only FreeInk Icons contract. The asset package itself is
// ESP32-only in PlatformIO metadata, while generated firmware icon headers are
// safe and useful in the native simulator.
struct Icon {
  uint16_t w;
  uint16_t h;
  int16_t opticalCenterY;
  const uint8_t *bits;
};

} // namespace freeink
