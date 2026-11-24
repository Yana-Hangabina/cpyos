#pragma once

// Geometry and hit-testing for the "cannot display this page" card's two actions.
//
// The card is the only surface while a page has failed, and this device has no
// key hints -- if a tap lands between the pills, or the pills run off a narrow
// screen, the page is a dead end again.  Those are geometry mistakes, so the
// geometry lives here, pure, with a host test.
//
// Widths are injected because the caller knows the screen; the pill width is
// fixed by the caller too (it depends on the label metrics), and that is the
// one number this file refuses to guess.

namespace pdf_fail_card {

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct Layout {
  Rect retry;
  Rect skip;
};

enum class Target { None, Retry, Skip };

struct Spec {
  int screenW = 0;
  int screenH = 0;
  int pillW = 0;
  int pillH = 0;
  int gap = 0;
  int top = 0;  // first y below the message card
};

// Two pills side by side, centred, at `top`.  If the pair cannot fit the screen
// the width shrinks rather than letting a pill leave the panel: a half-visible
// button is worse than a smaller one, and the tap target stays whole.
inline Layout layout(const Spec spec) {
  Layout out{};
  out.retry.h = out.skip.h = spec.pillH;
  out.retry.y = out.skip.y = spec.top;
  const int margin = 16;
  const int maxPair = spec.screenW - 2 * margin;
  int pillW = spec.pillW;
  if (pillW <= 0) pillW = 1;
  if (2 * pillW + spec.gap > maxPair) {
    pillW = (maxPair - spec.gap) / 2;
    if (pillW < 1) pillW = 1;
  }
  out.retry.w = out.skip.w = pillW;
  const int total = 2 * pillW + spec.gap;
  const int left = (spec.screenW - total) / 2;
  out.retry.x = left;
  out.skip.x = left + pillW + spec.gap;
  return out;
}

inline bool inside(const Rect r, const int x, const int y) {
  return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

inline Target hitTest(const Layout l, const int x, const int y) {
  if (inside(l.retry, x, y)) return Target::Retry;
  if (inside(l.skip, x, y)) return Target::Skip;
  return Target::None;
}

}  // namespace pdf_fail_card
