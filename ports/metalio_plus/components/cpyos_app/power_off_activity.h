#pragma once

#include "activities/Activity.h"

// CPYOS-styled shutdown confirmation, raised by a 3 s power-key hold.
// Confirm runs the vendor P1.3 pulse train; Back / Cancel / touch outside the
// confirm capsule keeps the device running.
class PowerOffActivity final : public Activity {
 public:
  explicit PowerOffActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerOff", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  void debugConfirm() override;
  void debugCancel() override;

 private:
  void confirmOff();
  void drawShutdownScreen();
  void cancel();
  bool shuttingDown_ = false;
  Rect confirmRect_{};
  Rect cancelRect_{};
};
