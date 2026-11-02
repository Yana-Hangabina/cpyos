#pragma once

#include <cstdint>

// Host stand-in for the panel object the converters size covers against.
class HalDisplay {
 public:
  uint16_t getDisplayWidth() const { return 200; }
  uint16_t getDisplayHeight() const { return 120; }
};

// The converters reach the panel through this global on device.
extern HalDisplay display;
