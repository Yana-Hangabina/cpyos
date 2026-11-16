#pragma once

#include <cstdint>

// Reader line spacing resolution.
//
// The four-level `lineSpacing` setting maps to per-family compression factors
// in CrossPointSettings::getReaderLineCompression() (SD/vector faces get
// 0.95/1.0/1.3/1.6, the built-in serif 0.95/1.0/1.1/1.2, the built-in sans
// 0.90/0.95/1.0/1.05). `readerLineSpacingPercent` is an advanced override: when
// non-zero it replaces whichever factor the level/family pair produced with
// percent/100, so a reader can dial an exact density. 0 means "off" and keeps
// the factors untouched, which is also why an old settings file needs no
// migration: the missing key leaves the field at its 0 default.
//
// The section cache already stores the resolved float lineCompression in its
// header (Section.cpp) and compares it on reuse, so nothing else has to change
// when the override moves the value.
//
// Kept header-only and dependency-free so the host test can pin the mapping
// without pulling in ArduinoJson/Arduino headers.
namespace reader_line_spacing {

inline constexpr uint8_t PERCENT_OFF = 0;
inline constexpr uint8_t PERCENT_MIN = PERCENT_OFF;
inline constexpr uint8_t PERCENT_MAX = 200;
// 0 (off) then 10 % steps: 100/110/.../200 and the 130/160 points are all
// reachable, and the row cycles in a bounded number of presses.
inline constexpr uint8_t PERCENT_STEP = 10;

// `familyFactor` is the 4-level + per-family factor; `percent` overrides it
// when non-zero. Pure, so the host test can mutate it and see the mapping fail.
inline float resolve(const float familyFactor, const uint8_t percent) {
  return percent != PERCENT_OFF ? static_cast<float>(percent) / 100.0f : familyFactor;
}

}  // namespace reader_line_spacing
