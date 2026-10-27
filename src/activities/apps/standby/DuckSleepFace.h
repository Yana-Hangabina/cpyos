#pragma once

#include <cstdint>

#include "StandbyFace.h"

// "Duck sleep" — the CPY default Standby face: Songti extra-bold HH:MM with a
// language-neutral date line (the design's 锁屏时间 treatment), the sleeping
// duck floating on a dithered water band. The sloppy hand-drawn clock stays
// with the upstream clock face, which remains available as another face.
class DuckSleepFace final : public StandbyFace {
 public:
  void onEnter() override;
  void onExit() override;
  TickResult tick() override;
  void render(GfxRenderer& renderer, const Rect& viewport) override;
  StrId titleId() const override;
  uint32_t secondsUntilNextWake() const override;

 private:
  uint32_t startMs_ = 0;  // millis() anchor for the pre-sync fallback display
  int32_t lastMin_ = -1;  // last rendered minute tick
  int16_t lastTenMinuteBucket_ = -1;
};
