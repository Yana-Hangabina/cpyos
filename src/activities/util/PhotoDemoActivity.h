#pragma once

#include "activities/Activity.h"

// Serial-only ghosting demo (CMD:PHOTODEMO <mode>).
//
// It runs a fixed, repeatable sequence so the local-clean A/B can be photographed
// without driving the UI by hand:
//   1. a test page (text line, 50% dither block, solid block) with a clean GC16;
//   2. twelve board-sized updates (each charges ghost debt) with the local clean
//      DISABLED (mode 0) or ENABLED (mode 1) -- so one mode flashes the whole
//      screen at the sweep and the other cleans only the changed rect;
//   3. a blank white page pushed WITHOUT a clean, leaving whatever residue the
//      run produced visible for the photo.
// Photograph the glass after `PHOTO demo done`. Compare the two modes at the same
// exposure: the decision the thresholds need is whether the rect clean keeps the
// residue at the whole-screen level.
class PhotoDemoActivity final : public Activity {
 public:
  PhotoDemoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int mode)
      : Activity("PhotoDemo", renderer, mappedInput), mode_(mode) {}

  void onEnter() override;
  void render(RenderLock&&) override {}
  // The moving box must go through the band path, exactly like a game move.
  bool wantsBandUpdates() const override { return true; }

 private:
  void drawTestPage() const;

  int mode_ = 0;
  bool prevLocalClean_ = false;
};
