#pragma once

#include "activities/Activity.h"

// Serial-only A/B page (CMD:FONTDEMO [pt]).
//
// The same face and point size are rendered three ways on one 16-level canvas:
//   1. old 2-bit -> 1-bit (25% threshold, the pre-change B/W path)
//   2. old 2-bit -> 4 levels (the pre-change reader LSB/MSB path)
//   3. new 4-bit -> 16 levels (the current gray16 path)
// The first two use a 2-bit preview face, the last a 4-bit preview face; both
// are registered from the system pack's streamed source, so the comparison is
// exactly the shipped font. The page is debug-only and does not change the
// reader's saved size.
class FontCompareActivity final : public Activity {
 public:
  FontCompareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int requestedPointSize)
      : Activity("FontCompare", renderer, mappedInput), requestedPointSize_(requestedPointSize) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void drawPage(bool grayscale) const;
  void drawLegacyFourLevel(int fontId, int x, int yTop, const char* text) const;

  int requestedPointSize_ = 0;
  uint8_t pointSize_ = 12;
  int preview2BitId_ = 0;
  int preview4BitId_ = 0;
  bool ready_ = false;
};
