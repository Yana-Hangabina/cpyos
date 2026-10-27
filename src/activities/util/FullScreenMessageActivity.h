#pragma once
#include <EpdFontFamily.h>
#include <HalDisplay.h>

#include <string>
#include <utility>

#include "activities/Activity.h"

// Full-page message / error screen.
//
// The bare form (title only) is what the rest of the firmware used before.
// The retryable CPY form is the startup error path (SD card): design frame 19
// asks for a "!" mark, a title, an explanation and an action capsule. Without
// it a dead SD card left the device on a single English line with no input
// route at all -- the user could only power-cycle.
//
// The retry action is a callback so this activity stays free of storage
// knowledge: the SD error caller re-mounts the card and reboots on success.
class FullScreenMessageActivity final : public Activity {
  std::string text;
  std::string detail;
  EpdFontFamily::Style style;
  HalDisplay::RefreshMode refreshMode;
  bool retryable;
  Rect retryRect_{};

  void paint();

 public:
  explicit FullScreenMessageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text,
                                     const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                     const HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH,
                                     std::string detail = {}, bool retryable = false)
      : Activity("FullScreenMessage", renderer, mappedInput),
        text(std::move(text)),
        detail(std::move(detail)),
        style(style),
        refreshMode(refreshMode),
        retryable(retryable) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};
