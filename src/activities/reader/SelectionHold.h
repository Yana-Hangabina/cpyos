#pragma once

#include <cstdint>

// Pure press-and-hold gesture machine shared by the EPUB reader and the PDF
// reflow reader.  `SelectionGeometry.h` made the *arithmetic* of a selection one
// implementation instead of two; this makes the *gesture* one implementation.
//
// It exists because the two readers had drifted, and the drift was a bug, not a
// style difference.  The EPUB version tolerates a missing touch sample and can
// fire the hold from the last known contact point, because the Read Pico's touch
// panel really does stop reporting a stationary finger (that is what the
// kHoldGapMs / last-point branches were written for, on device, after long
// presses silently did nothing).  The PDF version cancelled the press the moment
// `isScreenTouchHeld` returned false, so the same panel behaviour made a
// long-press-to-select fail there while it worked here.
//
// No renderer, no settings, no input manager: the caller reports what it observed
// this frame plus a millisecond clock, and gets back one verdict, so a host test
// can pin every branch (test/selection_hold/run_selection_hold_test.sh).
namespace selection_hold {

// Deliberately 650 ms, not the SDK's 500 ms classifier: on a reading page an
// ordinary hold can fall in the 500-600 ms band, and consuming the SDK's
// classifier would also swallow the contact before this longer timer could
// decide (which is what made a slow page turn look like a selection).
constexpr uint32_t kPressMs = 650;
// A gap this short between two reported contacts is a dropped sample, not a lift.
constexpr uint32_t kHoldGapMs = 150;
// Travel allowance from the first contact point, in pixels (squared compare).
constexpr int kSlopPx = 64;
// E-ink cannot follow a finger: repaint a drag at most this often.
constexpr uint32_t kDragRepaintMs = 320;
// A selection that closed this recently must not start a new one under the same
// finger (that is what made "delete highlight" appear to select a neighbouring word).
constexpr uint32_t kClosedGuardMs = 350;
// How long the selection action bar ignores taps and chip keys after it opens.  The gesture
// that OPENS the bar also produces the release the input layer reports as a screen tap, so
// without this window the first chip fires the instant the bar appears: a Lookup -- or a
// Delete on an annotation being edited -- that nobody asked for.
constexpr uint32_t kBarOpenSwallowMs = 250;

// Does an open bar accept input yet?  `openedMs == 0` means "opened before anyone tracked
// it", which must not read as "swallow forever".
constexpr bool barAcceptsInput(const uint32_t now, const uint32_t openedMs) {
  return openedMs == 0 ||
         static_cast<int32_t>(now - openedMs) >= static_cast<int32_t>(kBarOpenSwallowMs);
}

// True when enough time has passed since `since` to repaint.  32-bit millisecond
// wrap is handled by the signed difference, which is why this is a function of the
// clock and not a subtraction in the caller.
constexpr bool repaintDue(const uint32_t now, const uint32_t last) {
  return static_cast<int32_t>(now - last) >= static_cast<int32_t>(kDragRepaintMs);
}

// What the caller observed this frame, before any judgement.
struct Contact {
  // A release edge or a completed tap arrived this frame: the finger is gone.
  bool ended = false;
  // The panel reports a finger down at (x, y).
  bool held = false;
  int x = 0;
  int y = 0;
};

enum class Verdict {
  Nothing,          // keep waiting; this frame is not a selection and not a tap
  Firing,           // the hold timer elapsed -- the caller must open a selection at (x, y)
  CancelledTravel,  // the finger moved: it is a swipe, let the page-turn logic see it
  CancelledLift,    // a real release ended the press before the timer
};

class Tracker {
 public:
  // One call per UI frame, and only while no selection is open (once a selection
  // exists, the drag handles own the touch).  `tracking()` says whether a press is
  // still live, which the caller uses to keep swallowing the frame.
  Verdict update(const Contact& c, const uint32_t now) {
    // A real lift ends the press immediately. A quick tap that lands while the
    // reader is blocked in a page render/scan would otherwise look like a 650 ms
    // hold on the next pass (the release event arrives after the block).
    if (c.ended) {
      tracking_ = false;
      return Verdict::CancelledLift;
    }
    if (c.held) {
      if (!tracking_) {
        tracking_ = true;
        startMs_ = lastHeldMs_ = now;
        startX_ = lastX_ = c.x;
        startY_ = lastY_ = c.y;
        return Verdict::Nothing;
      }
      lastHeldMs_ = now;
      lastX_ = c.x;
      lastY_ = c.y;
      const int dx = c.x - startX_;
      const int dy = c.y - startY_;
      if (dx * dx + dy * dy > kSlopPx * kSlopPx) {
        tracking_ = false;  // genuine swipe
        return Verdict::CancelledTravel;
      }
      if (static_cast<int32_t>(now - startMs_) >= static_cast<int32_t>(kPressMs)) {
        tracking_ = false;
        fireX_ = c.x;
        fireY_ = c.y;
        return Verdict::Firing;
      }
      return Verdict::Nothing;
    }
    if (!tracking_) return Verdict::Nothing;
    // No contact this frame. A real lift is followed by many empty frames, while a
    // dropped sample is followed by contact again within a few ms -- only the
    // former may end the press, or the timer would restart forever.
    if (static_cast<int32_t>(now - lastHeldMs_) < static_cast<int32_t>(kHoldGapMs)) {
      return Verdict::Nothing;
    }
    // The panel can stop reporting a stationary finger entirely. If the press has
    // already lasted long enough at its last known point, fire from there instead
    // of dropping the gesture.
    if (static_cast<int32_t>(now - startMs_) >= static_cast<int32_t>(kPressMs)) {
      tracking_ = false;
      fireX_ = lastX_;
      fireY_ = lastY_;
      return Verdict::Firing;
    }
    tracking_ = false;
    return Verdict::Nothing;
  }

  // Where a Firing verdict asked for a selection.
  int fireX() const { return fireX_; }
  int fireY() const { return fireY_; }
  bool tracking() const { return tracking_; }
  void reset() { tracking_ = false; }

 private:
  bool tracking_ = false;
  uint32_t startMs_ = 0;
  uint32_t lastHeldMs_ = 0;
  int startX_ = 0;
  int startY_ = 0;
  int lastX_ = 0;
  int lastY_ = 0;
  int fireX_ = 0;
  int fireY_ = 0;
};

}  // namespace selection_hold
