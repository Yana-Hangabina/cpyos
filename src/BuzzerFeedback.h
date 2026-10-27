#pragma once

#include <cstdint>

// Short, low-volume click cues for boards whose buzzer is a passive tone
// device (Read Pico: GPIO2 -> AO3400A -> coil + flyback diode). This is a
// *sound* feedback path, not haptics: the panel has no vibration motor.
//
// The driver is deliberately lazy: every cue re-attaches LEDC, emits a short
// pulse, and update() detaches the channel again. Read Pico's display power
// hook parks GPIO2 with pinMode()/digitalWrite() before each frame, which
// silently detaches any LEDC channel left open across a push, so a one-shot
// begin() would go mute after the first page turn.
namespace BuzzerFeedback {

// PWM tone color used by the upper half of the strength range.
enum class Pitch : uint8_t { Deep = 0, Soft = 1, Crisp = 2, Count };

// Strength 0-50 is a single full-current pulse of 5-300 us: the quietest
// feedback the hardware can give, and the only range that goes below the
// coil's static-friction threshold (low-duty chopping does not move it at all).
// Strength 51-100 is a PWM tone at the selected pitch, 10-30% duty.
void cue(Pitch pitch, uint8_t strength);

// Settings preview: restarts immediately, ignoring the cue that the tap on the
// control itself just emitted. Without this, that first cue occupies the
// minimum-gap window and the new value would never be heard.
void preview(Pitch pitch, uint8_t strength);

// Non-blocking expiry: call once per main-loop pass.
void update();

// Stops a sounding cue and releases the LEDC channel.
void shutdown();

}  // namespace BuzzerFeedback
