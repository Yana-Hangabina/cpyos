#include "HalFrontlight.h"

#include <Logging.h>
#include <driver/ledc.h>

#include <algorithm>

HalFrontlight& HalFrontlight::getInstance() {
  static HalFrontlight instance;
  return instance;
}

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = kFrequency;
  timer.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&timer) != ESP_OK) {
    LOG_ERR("LIGHT", "frontlight timer unavailable");
    return;
  }
  for (int i = 0; i < 2; ++i) {
    ledc_channel_config_t ch = {};
    ch.gpio_num = i == 0 ? kCoolGpio : kWarmGpio;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = static_cast<ledc_channel_t>(i);
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 0;  // stay dark until begin() decides; a reset must not flash
    ch.hpoint = 0;
    if (ledc_channel_config(&ch) != ESP_OK) {
      LOG_ERR("LIGHT", "frontlight channel %d unavailable", i);
      return;
    }
  }
  begun = true;
  lastBrightness = brightness > 100 ? 100 : brightness;
  lastWarmth = warmth > 100 ? 100 : warmth;
  lit = on;
  apply();
  LOG_INF("LIGHT", "frontlight up: %u%% warm=%u%% %s (cool=%lu warm=%lu duty)", lastBrightness, lastWarmth,
          lit ? "on" : "off", static_cast<unsigned long>(lastCoolDuty),
          static_cast<unsigned long>(lastWarmDuty));
}

void HalFrontlight::apply() {
  if (!begun) return;
  const uint32_t brightness = lit ? lastBrightness : 0;
  // Vendor limit (metalio-vendor frontlight.cc): UI brightness 100 maps to 10 %
  // of the PWM full scale, `duty = FL_DUTY_MAX * b * 10 / 10000`. The previous
  // half-scale mapping drove the LEDs at 50 % (measured 511/1023 at 100 %), i.e.
  // five times the vendor's current, so it is replaced rather than kept as a
  // brighter default. Channel blending follows the same source: warmth 0/100 is
  // one channel only, 50 drives both channels at full mix.
  const uint32_t duty = brightness * kFullScale * kMaxOutputPercent / 10000;
  const uint32_t coolMix = std::min<uint32_t>(100, 2 * (100 - lastWarmth));
  const uint32_t warmMix = std::min<uint32_t>(100, 2 * lastWarmth);
  lastCoolDuty = duty * coolMix / 100;
  lastWarmDuty = duty * warmMix / 100;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, lastCoolDuty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, lastWarmDuty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  apply();
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  lastWarmth = warmPercent > 100 ? 100 : warmPercent;
  apply();
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) return;
  lit = on;
  apply();
}

void HalFrontlight::debugDuties(uint32_t& cool, uint32_t& warm) const {
  cool = lastCoolDuty;
  warm = lastWarmDuty;
}
