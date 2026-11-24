#pragma once

#include "activities/Activity.h"
#include "components/Rect.h"

// CPYOS shutdown confirmation, raised by a 3 s power-key hold -- the same card
// the Metalio Plus port shows (parity requested 2026-11-23).  On Read Pico the
// confirm runs the PMU's cooperative power-off (HOST_REQUEST_OFF 0x0311 ->
// SHUTDOWN_READY, EN dropped); Back / Cancel / a tap outside the confirm capsule
// keeps the device running.
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
