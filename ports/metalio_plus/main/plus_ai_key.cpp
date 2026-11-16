#include "plus_ai_key.h"

#include <Arduino.h>
#include <driver/gpio.h>

#include "plus_ai_key_sampler.h"

namespace plus_ai_key {
namespace {

constexpr gpio_num_t kPin = GPIO_NUM_61;
constexpr uint32_t kPollGapMs = 2000;

Sampler s_sampler;
Event s_injected = Event::None;
uint32_t s_lastPollMs = 0;

bool rawPressed() { return gpio_get_level(kPin) == 0; }

}  // namespace

void begin() {
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << static_cast<unsigned>(kPin);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    (void)gpio_config(&cfg);

    const uint32_t now = millis();
    // A key held at boot powered the unit on; do not turn that into a gesture.
    s_sampler.reset(rawPressed(), now, /*armed=*/!rawPressed());
    s_lastPollMs = now;
}

void clear() {
    const uint32_t now = millis();
    const bool down = rawPressed();
    s_sampler.reset(down, now, /*armed=*/!down);
    s_lastPollMs = now;
}

bool down() { return s_sampler.stableDown(); }

void debugInject(const Event event) { s_injected = event; }

Event poll(const uint32_t nowMs) {
    if (s_injected != Event::None) {
        const Event event = s_injected;
        s_injected = Event::None;
        s_lastPollMs = nowMs;
        return event;
    }
    if (s_lastPollMs != 0 && nowMs - s_lastPollMs > kPollGapMs) {
        // Sleep or a long blocking write: the press may belong to another
        // context. Re-arm only after a released sample.
        clear();
        s_lastPollMs = nowMs;
        return Event::None;
    }
    s_lastPollMs = nowMs;
    return s_sampler.update(rawPressed(), nowMs);
}

}  // namespace plus_ai_key
