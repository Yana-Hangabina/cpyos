// Host check for font_restore::due() (src/FontRestorePolicy.h).
//
// Every term of the conjunction gets a case where flipping that one term alone
// changes the answer; a conjunction tested only as a whole can pass with any
// term deleted. The case that shipped broken is the first one: pending with a
// quiet network must restore, because that is the state a JJWXC or WeRead list
// is in when it draws tofu for characters the data partition carries.

#include <cstdio>

#include "FontRestorePolicy.h"

namespace {

int failures = 0;
int checks = 0;

void check(const bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

using font_restore::Situation;

Situation quiet() {
  Situation s;
  s.pending = true;
  s.radioIdle = true;
  s.stationClaimed = false;
  s.networkHeld = false;
  s.elapsedMs = font_restore::kMinQuietMs;
  return s;
}

}  // namespace

int main() {
  check(font_restore::due(quiet()), "a settled session restores the fonts");

  {  // nothing was reclaimed: never act
    Situation s = quiet();
    s.pending = false;
    check(!font_restore::due(s), "not pending -> no restore");
  }
  {  // a driver command is executing
    Situation s = quiet();
    s.radioIdle = false;
    check(!font_restore::due(s), "driver command in flight -> wait");
  }
  {  // someone is between mode and begin
    Situation s = quiet();
    s.stationClaimed = true;
    check(!font_restore::due(s), "bring-up claim -> wait");
  }
  {  // a request or download holds the session
    Situation s = quiet();
    s.networkHeld = true;
    check(!font_restore::due(s), "network hold -> wait");
  }
  {  // too soon after the reclaim
    Situation s = quiet();
    s.elapsedMs = font_restore::kMinQuietMs - 1;
    check(!font_restore::due(s), "below the quiet floor -> wait");
    s.elapsedMs = font_restore::kMinQuietMs;
    check(font_restore::due(s), "at the quiet floor -> restore");
  }
  {  // two terms at once must still be enough to block
    Situation s = quiet();
    s.networkHeld = true;
    s.stationClaimed = true;
    check(!font_restore::due(s), "hold plus claim -> wait");
  }
  {  // a long-elapsed, fully idle session is the field case
    Situation s = quiet();
    s.elapsedMs = 60000;
    check(font_restore::due(s), "one minute after a quiet reclaim -> restore");
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
