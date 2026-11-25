#pragma once

#include "activities/Activity.h"

// Full-screen notice shown just before the SoC is reset into USB download
// mode. The e-ink image survives the reset, so this is what the user sees
// while the ROM loader waits for a host.
class UsbDownloadModeActivity final : public Activity {
  uint32_t startedAt_ = 0;
  bool switching_ = false;
  bool painted_ = false;
  void paint();

 public:
  UsbDownloadModeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UsbDownloadMode", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};
