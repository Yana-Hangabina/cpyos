#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

class CrashActivity final : public Activity {
  std::string panicMessage;
  // Boot-stage trail of the boot that never painted (empty when that boot died
  // after its first frame, i.e. the crash was not a boot problem).
  std::string bootTrailMessage;
  // Pre-formatted crash context lines: throwing task, core, the screen and
  // lifecycle step that were live, the exception type name, decodable return
  // addresses and the last heap snapshot. Built once in onEnter() so render()
  // allocates nothing -- this screen is shown *because* something went wrong.
  std::vector<std::string> contextLines;
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
