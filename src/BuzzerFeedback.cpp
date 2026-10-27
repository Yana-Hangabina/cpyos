#include "BuzzerFeedback.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>

#if CROSSPOINT_CAP_BUZZER_FEEDBACK

namespace BuzzerFeedback {
namespace {

constexpr uint8_t kResolutionBits = 10;
constexpr uint32_t kMinGapMs = 60;
constexpr uint16_t kMaxDuty = (1u << kResolutionBits) - 1;

// One continuous strength control:
//   0-50  -> single full-current pulse, 5-300 us. The coil has static
//           friction; below a minimum peak force the diaphragm does not move
//           no matter how long average current is applied (25 kHz chopping at
//           6.3% and below was completely silent). Full force for less time is
//           therefore the only way down.
//   51-100 -> PWM tone at the selected pitch, 10-30% duty. The pitch only
//           exists in this range; a single pulse has no tone.
constexpr uint8_t kPulseStrengthMax = 50;
constexpr uint16_t kPulseMinUs = 5;
constexpr uint16_t kPulseMaxUs = 300;
constexpr uint16_t kToneMinDuty = 100;  // 9.8%
constexpr uint16_t kToneMaxDuty = 300;  // 29.3%

struct ToneSpec {
  uint16_t freqHz;
  uint16_t durationMs;
};

constexpr ToneSpec kTones[] = {
    {300, 12},   // Deep
    {2000, 10},  // Soft
    {2700, 8},   // Crisp
};
constexpr uint8_t kToneCount = sizeof(kTones) / sizeof(kTones[0]);

int8_t activePin = -1;
uint32_t busyUntilMs = 0;
uint32_t lastStartMs = 0;
bool loggedFailure = false;

void release() {
  if (activePin < 0) return;
  const uint8_t pin = static_cast<uint8_t>(activePin);
  ledcWrite(pin, 0);
  ledcDetach(pin);
  // Leave the gate actively low; the buzzer's 100k pulldown is a fallback, not
  // the idle state the display power hook expects.
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  activePin = -1;
}

}  // namespace

void cue(const Pitch pitch, const uint8_t strength) {
  const int8_t pin = BoardConfig::ACTIVE.audio.buzzer;
  if (pin < 0 || activePin >= 0) return;

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - lastStartMs) < static_cast<int32_t>(kMinGapMs)) return;

  const uint8_t s = strength > 100 ? 100 : strength;
  if (s <= kPulseStrengthMax) {
    const uint16_t pulseUs =
        kPulseMinUs + static_cast<uint32_t>(s) * (kPulseMaxUs - kPulseMinUs) / kPulseStrengthMax;
    pinMode(static_cast<uint8_t>(pin), OUTPUT);
    digitalWrite(static_cast<uint8_t>(pin), HIGH);
    delayMicroseconds(pulseUs);
    digitalWrite(static_cast<uint8_t>(pin), LOW);
    lastStartMs = now;
    busyUntilMs = now;  // already silent; update() has nothing to release
    LOG_DBG("BZR", "feedback GPIO%d pulse=%uus strength=%u", pin, static_cast<unsigned>(pulseUs),
            static_cast<unsigned>(s));
    return;
  }

  uint8_t index = static_cast<uint8_t>(pitch);
  if (index >= kToneCount) index = 1;
  const ToneSpec& spec = kTones[index];
  const uint16_t duty =
      kToneMinDuty + static_cast<uint32_t>(s - kPulseStrengthMax) * (kToneMaxDuty - kToneMinDuty) /
                         (100 - kPulseStrengthMax);

  // Re-attach per cue: ReadPicoPower parks GPIO2 with pinMode() before every
  // frame, which drops any LEDC channel attached earlier.
  if (!ledcAttach(static_cast<uint8_t>(pin), spec.freqHz, kResolutionBits)) {
    if (!loggedFailure) {
      loggedFailure = true;
      LOG_ERR("BZR", "LEDC attach failed on GPIO%d", pin);
    }
    return;
  }

  activePin = pin;
  busyUntilMs = now + spec.durationMs;
  lastStartMs = now;
  ledcWrite(static_cast<uint8_t>(pin), duty > kMaxDuty ? kMaxDuty : duty);
  LOG_DBG("BZR", "feedback GPIO%d tone=%uHz duty=%u/1024 %ums strength=%u", pin,
          static_cast<unsigned>(spec.freqHz), static_cast<unsigned>(duty), static_cast<unsigned>(spec.durationMs),
          static_cast<unsigned>(s));
}

void preview(const Pitch pitch, const uint8_t strength) {
  release();
  busyUntilMs = 0;
  lastStartMs = 0;  // bypass the gap left by the control tap
  cue(pitch, strength);
}

void update() {
  if (activePin < 0) return;
  if (static_cast<int32_t>(millis() - busyUntilMs) < 0) return;
  release();
}

void shutdown() {
  release();
  busyUntilMs = 0;
}

}  // namespace BuzzerFeedback

#else

namespace BuzzerFeedback {
void cue(Pitch, uint8_t) {}
void preview(Pitch, uint8_t) {}
void update() {}
void shutdown() {}
}  // namespace BuzzerFeedback

#endif  // CROSSPOINT_CAP_BUZZER_FEEDBACK
