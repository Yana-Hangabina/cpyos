#pragma once

#include "activities/Activity.h"

// Screen repair (docs/07 D1; the same recovery kiikoread ships in Settings > Upgrade
// and Recovery). Push the whole panel through white -> black -> white with FULL
// refreshes, then return to the settings page with one more full refresh.
//
// It only drives the existing frame buffer and refresh path: no waveform, VCOM
// or driver state is touched, so a panel that is already smeared cannot be made
// worse and a healthy panel just blinks three times.
class ScreenRepairActivity final : public Activity {
 public:
  explicit ScreenRepairActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ScreenRepair", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }  // the sequence blocks a few hundred ms per step

 private:
  enum Step : uint8_t { WhiteFirst, Black, WhiteSecond, Done };
  Step step = Step::WhiteFirst;
};
