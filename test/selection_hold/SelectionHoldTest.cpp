// Host regression for the shared press-and-hold gesture machine
// (src/activities/reader/SelectionHold.h).  Every branch here is a decision the
// two readers used to make twice, in slightly different ways -- and the
// difference was a real defect: the PDF reader cancelled a long press on a single
// dropped touch sample, which the Read Pico panel produces routinely while a
// finger sits still.
#include <cstdint>
#include <cstdio>
#include <string>

#include "SelectionHold.h"

namespace {

int checks = 0;
int failures = 0;

void check(const bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

using selection_hold::Contact;
using selection_hold::Tracker;
using selection_hold::Verdict;

const Contact kDown{false, true, 100, 200};

// 1. The timer: fires at exactly kPressMs, not before.
void testPressDurationBoundary() {
  {
    Tracker t;
    check(t.update(kDown, 1000) == Verdict::Nothing, "first contact starts tracking");
    check(t.tracking(), "press is live after the first frame");
    const Verdict v = t.update(kDown, 1000 + selection_hold::kPressMs - 1);
    check(v == Verdict::Nothing, "one millisecond short of the hold must not fire");
    check(t.update(kDown, 1000 + selection_hold::kPressMs) == Verdict::Firing,
          "the hold must fire at exactly kPressMs");
  }
  {
    Tracker t;
    t.update(kDown, 500);
    check(t.update(kDown, 500 + selection_hold::kPressMs / 2) == Verdict::Nothing, "half a hold is no hold");
  }
}

// 2. A quick tap is a tap: the release edge ends the press, and the reader must
//    still see the tap (CancelledLift is not a selection).
void testQuickTapStaysATap() {
  Tracker t;
  t.update(kDown, 1000);
  const Contact lift{true, false, 100, 200};
  check(t.update(lift, 1200) == Verdict::CancelledLift, "a release ends the press as a tap");
  check(!t.tracking(), "nothing is being tracked after the release");
}

// 3. Travel is a swipe, and the slop circle is measured from the FIRST point.
void testTravelCancels() {
  {
    Tracker t;
    t.update(kDown, 1000);
    const int r = selection_hold::kSlopPx;
    const Contact justInside{false, true, 100 + r, 200};
    check(t.update(justInside, 1100) == Verdict::Nothing, "travel to the slop radius keeps the press");
    const Contact outside{false, true, 100 + r + 1, 200};
    check(t.update(outside, 2000) == Verdict::CancelledTravel, "one pixel past the slop is a swipe");
    check(!t.tracking(), "a cancelled press is not still live");
  }
  {
    // The discriminating case for a circle vs a per-axis box: (50, 50) is inside
    // a 64 px box but outside a 64 px circle, and a reading page's diagonal drag
    // must be treated as the swipe it is.
    Tracker t;
    t.update(kDown, 1000);
    const Contact diagonal{false, true, 150, 250};
    check(t.update(diagonal, 1100) == Verdict::CancelledTravel,
          "diagonal travel past the slop circle must cancel even though each axis is inside it");
  }
  {
    // (44, 44) is inside the circle (44^2*2 < 64^2), so it must NOT cancel.
    Tracker t;
    t.update(kDown, 1000);
    const int d = static_cast<int>(selection_hold::kSlopPx / 1.45);
    const Contact insideCircle{false, true, 100 + d, 200 + d};
    check(t.update(insideCircle, 1100) == Verdict::Nothing, "a small diagonal wobble keeps the press");
  }
  {
    // The reference point never moves: creeping 60 px then another 60 px is travel.
    Tracker t;
    t.update(kDown, 1000);
    t.update(Contact{false, true, 160, 200}, 1100);
    check(t.update(Contact{false, true, 220, 200}, 1200) == Verdict::CancelledTravel,
          "the slop is measured from the first contact, not the previous frame");
  }
}

// 4. THE defect this file exists for: one dropped sample must not kill the press.
void testDroppedSampleKeepsThePress() {
  Tracker t;
  t.update(kDown, 1000);
  const Contact gap{false, false, 0, 0};
  check(t.update(gap, 1000 + selection_hold::kHoldGapMs - 1) == Verdict::Nothing,
        "a gap shorter than kHoldGapMs is a dropped sample, not a lift");
  const Verdict v = t.update(kDown, 1000 + selection_hold::kPressMs);
  check(v == Verdict::Firing, "the press must survive the dropped sample and still fire");
}

// 5. The panel can stop reporting a stationary finger altogether. The hold then
//    fires from the last known point, which is what the EPUB reader already did.
void testFiresFromLastKnownPoint() {
  Tracker t;
  t.update(kDown, 1000);
  const Contact moved{false, true, 130, 210};
  t.update(moved, 1100);
  const Contact gone{false, false, 0, 0};
  const uint32_t later = 1100 + selection_hold::kPressMs;
  check(t.update(gone, later) == Verdict::Firing, "a silent panel must still yield a selection");
  check(t.fireX() == 130 && t.fireY() == 210, "it must fire at the LAST reported point, not the first");
  check(!t.tracking(), "a fired press is finished");
}

// 6. A real lift while stationary must not be resurrected by the last-point rule.
void testRealLiftIsNotAFire() {
  Tracker t;
  t.update(kDown, 1000);
  const Contact gone{false, false, 0, 0};
  check(t.update(gone, 1000 + selection_hold::kHoldGapMs) == Verdict::Nothing,
        "a lift before the timer must not fire");
  check(!t.tracking(), "the press ended with the lift");
  check(t.update(gone, 1000 + selection_hold::kPressMs * 4) == Verdict::Nothing,
        "an ended press stays ended on later empty frames");
}

// 7. 32-bit millisecond wrap: millis() rolls over roughly every 49 days, and a
//    hold that straddles it must still fire.
void testClockWrap() {
  const uint32_t near = UINT32_MAX - 100;
  Tracker t;
  t.update(kDown, near);
  const uint32_t after = near + selection_hold::kPressMs;  // wraps
  check(t.update(kDown, after) == Verdict::Firing, "a hold that straddles the millis() wrap must fire");
  check(selection_hold::repaintDue(near + selection_hold::kDragRepaintMs, near),
        "the repaint gate must survive the wrap too");
}

// 8. The drag repaint gate.
void testRepaintGate() {
  const uint32_t last = 4000;
  check(!selection_hold::repaintDue(last + selection_hold::kDragRepaintMs - 1, last),
        "one millisecond early must not repaint");
  check(selection_hold::repaintDue(last + selection_hold::kDragRepaintMs, last), "the gate opens at the cap");
  check(!selection_hold::repaintDue(last, last), "a repaint that just happened is not due again");
}

// 9. After firing, the next contact is a new press (the caller suppresses the
//    finger that fired; this documents that the tracker itself holds no memory).
void testFiringReleasesTheTracker() {
  Tracker t;
  t.update(kDown, 1000);
  check(t.update(kDown, 1650) == Verdict::Firing, "hold fires");
  check(t.update(kDown, 1700) == Verdict::Nothing, "the next frame starts a fresh press");
  check(t.tracking(), "and is tracked");
  check(t.update(kDown, 2350) == Verdict::Firing, "the fresh press reaches its own 650 ms");
  t.reset();
  check(!t.tracking(), "reset() drops the press (leaving the reader / closing a selection)");
}

}  // namespace

// The action bar's opening-press window: shared by both readers after the PDF one was
// found to activate chip 0 on the very gesture that opened the bar.
static void testBarOpenSwallow() {
  using namespace selection_hold;
  check(barAcceptsInput(1000, 749), "250 ms after opening the bar accepts input");
  check(!barAcceptsInput(1000, 751), "one ms early it does not");
  check(!barAcceptsInput(1000, 1000), "the frame the bar opened in is swallowed");
  check(barAcceptsInput(1000, 0), "an untracked open time must not swallow forever");
  // Wrap: the clock passes 2^31 between opening and now.
  // opened = 0xFFFFFEF0, now = 100 -> 316 ms elapsed across the 2^32 boundary.
  check(barAcceptsInput(100, 0xFFFFFEF0u), "wrap-safe: long after opening");
  check(!barAcceptsInput(0xFFFFFFF0u, 0xFFFFFF00u), "wrap-safe: still inside the window");
  check(kBarOpenSwallowMs == 250, "the window the EPUB reader always used");
}

int main() {
  testBarOpenSwallow();
  testPressDurationBoundary();
  testQuickTapStaysATap();
  testTravelCancels();
  testDroppedSampleKeepsThePress();
  testFiresFromLastKnownPoint();
  testRealLiftIsNotAFire();
  testClockWrap();
  testRepaintGate();
  testFiringReleasesTheTracker();
  std::printf("selection hold: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
