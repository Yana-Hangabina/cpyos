#pragma once

#include "activities/Activity.h"

// The screen shown after a panic reboot.
//
// It must survive a heap that is already empty: the 0.8.4 field report died in
// Settings with `operator new` failing, rebooted into this screen, and this
// screen's own std::string/std::vector allocations failed in turn (the recorded
// `PNC-04` then carries `screen=Crash stage=enter`, i.e. the second death).
//
// So nothing here allocates after construction: the lines are fixed char arrays
// built in onEnter() with snprintf, wrapping is done into a caller-supplied
// buffer, and render() only draws. `scripts/tests/test_crash_screen_alloc.py`
// keeps it that way.
class CrashActivity final : public Activity {
 public:
  // Exposed so the allocation-free wrap helper in the .cpp can size its buffer
  // from the same constant the member arrays use.
  static constexpr uint8_t kMaxLines = 24;
  static constexpr uint8_t kLineChars = 96;

 private:

  // 0 = description, then reason/trail/context share the array in draw order.
  char description_[4][kLineChars] = {};
  uint8_t descriptionCount_ = 0;
  char reason_[5][kLineChars] = {};
  uint8_t reasonCount_ = 0;
  char context_[kMaxLines][kLineChars] = {};
  uint8_t contextCount_ = 0;
  char trail_[3][kLineChars] = {};
  uint8_t trailCount_ = 0;

  // The context block is paginated rather than cut off at the bottom of the
  // screen: a report that silently loses its last three lines is the bug this
  // screen was rewritten to fix. `contextMore` says a further page exists.
  int contextPage = 0;
  bool contextMore = false;

  void buildContextLines();

 public:
  explicit CrashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Crash", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};