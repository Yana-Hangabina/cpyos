#pragma once
#include "activities/Activity.h"

class CrashActivity final : public Activity {
  std::string panicMessage;
  // Boot-stage trail of the boot that never painted (empty when that boot died
  // after its first frame, i.e. the crash was not a boot problem).
  std::string bootTrailMessage;

 public:
  explicit CrashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Crash", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
