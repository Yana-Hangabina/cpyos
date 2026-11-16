#pragma once

#include <cstdint>

/*
 * Physical AI/BOOT key on the Metalio E-INK4 Plus (GPIO61).
 *
 * The vendor and kiikoread both treat this pin as active-low with a 25 ms
 * debounce and a 500 ms short/long split; a release after a long hold never
 * produces a short event. This sampler is level-driven by the UI loop, so it
 * adds no task and no interrupt. It is disarmed at boot (the key may be the one
 * the user held to power on) and re-arms after the first released sample.
 */
namespace plus_ai_key {

enum class Event : uint8_t { None, Short, Long };

void begin();
// Poll at least every ~100 ms. Gaps longer than 2 s (light sleep, a long SD
// write) discard the gesture rather than replaying it on wake.
Event poll(uint32_t nowMs);
bool down();
void clear();
// Bench only: deliver one synthetic event through poll(), so the console can
// exercise the real routing (blocklists, page toggles) without a hand on the
// key. Never used by product code.
void debugInject(Event event);

}  // namespace plus_ai_key
