#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// NOT ROUTED since 2026-10-28: main.cpp clears a stale resume state and goes
// Home instead. The page interrupted the reader and added no copy protection.
// Kept only because its strings are still in the i18n pack; remove the class
// and its three translated keys together in a data-partition release.
//
// Shown at boot when the previous attempt to open a book died (docs/07 D8).
//
// Today that case silently lands on Home: main.cpp's router treats a non-zero
// readerActivityLoadCount as "do not retry the reader" and falls through to the
// home screen, so the user is moved somewhere else with no explanation and no
// way back to the book they were reading. This page names the book and offers
// the two things that actually help: try again, or drop the resume point.
//
// It owns no state of its own: the book path and the crash counter live in
// APP_STATE, and the page is only ever reached from that one routing branch, so
// a normal boot cannot land here.
class ReaderRecoveryActivity final : public Activity {
 public:
  explicit ReaderRecoveryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderRecovery", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  OptionPopup popup;

  // Retry keeps the counter incremented, so a second crash comes back here
  // instead of looping the boot. A book that opens resets it (ReaderActivity).
  void retryBook();
  // Drop the resume point so the next boot is a plain Home boot.
  void skipBook();
};
