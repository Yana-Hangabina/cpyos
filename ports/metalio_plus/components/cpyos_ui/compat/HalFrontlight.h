#pragma once

#include <cstdint>

// Plus frontlight: a warm/cool LED pair on GPIO6 (cool) / GPIO7 (warm), PWM
// through LEDC (10-bit, 20 kHz), driven with the reference firmware's power
// limit. FrontlightManager is NOT used here: its perceptual curve maps 100 % to
// full duty, while the vendor clamps UI 100 % to 10 % of the PWM full scale
// (`duty = full * brightness * 10 / 10000`, metalio-vendor frontlight.cc).
// Over-driving the LEDs is the one thing this hardware must not do, so the
// port owns the duty math.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance();

  void begin(uint8_t brightness, uint8_t warmth, bool on);

  void setBrightness(uint8_t percent);
  void setWarmth(uint8_t warmPercent);
  void setOn(bool on);

  // Duty actually written to each channel, for the serial bench (`light`).
  void debugDuties(uint32_t& cool, uint32_t& warm) const;

  // Some E-Ink4 Plus units ship without the LED pair. There is no electrical
  // readback for the YX6016N, so presence is a persisted user choice (设置 →
  // 显示 → 本机带前光); hiding the rows is what the user asked for on such
  // units. Default true so every unit with LEDs works out of the box.
  void setPresentOverride(bool present) { presentOverride_ = present; }
  bool present() const { return begun && presentOverride_; }
  bool hasColorTemperature() const { return true; }

  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return lastWarmth; }
  bool isOn() const { return lit; }

 private:
  HalFrontlight() = default;

  void apply();

  // Reference mapping: 100 % brightness == half of the 10-bit range.
  static constexpr uint32_t kFullScale = 1023;
  // Vendor FL_OUTPUT_MAX_PERCENT: UI 100 % == 10 % of the PWM full scale.
  static constexpr uint32_t kMaxOutputPercent = 10;
  static constexpr uint8_t kCoolGpio = 6;
  static constexpr uint8_t kWarmGpio = 7;
  static constexpr uint32_t kFrequency = 20000;
  static constexpr uint8_t kResolutionBits = 10;

  bool begun = false;
  uint8_t lastBrightness = 60;
  uint8_t lastWarmth = 50;
  bool lit = false;
  bool presentOverride_ = true;
  uint32_t lastCoolDuty = 0;
  uint32_t lastWarmDuty = 0;
};

#define Frontlight HalFrontlight::getInstance()
