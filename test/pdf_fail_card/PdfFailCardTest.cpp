// Pins the failure card's geometry: two whole pills, centred, never off-screen,
// and a tap between them is "nothing" rather than a mystery.
#include "PdfFailCardLayout.h"

#include <cstdio>

using namespace pdf_fail_card;

static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    ++checks;                                                                 \
    if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__);        \
                   std::printf(__VA_ARGS__); std::printf("\n"); }             \
  } while (0)

int main() {
  const Spec spec{684, 1216, 220, 64, 24, 664};
  const Layout l = layout(spec);

  // Centred, same size, separated by exactly the gap.
  CHECK(l.retry.y == l.skip.y && l.retry.y == spec.top, "pills are not on one line");
  CHECK(l.retry.w == l.skip.w && l.retry.w == spec.pillW, "pill width changed although it fits");
  CHECK(l.skip.x - (l.retry.x + l.retry.w) == spec.gap, "the gap between pills is not the spec");
  CHECK(l.retry.x >= 0 && l.skip.x + l.skip.w <= spec.screenW, "a pill leaves the panel");
  const int leftMargin = l.retry.x;
  const int rightMargin = spec.screenW - (l.skip.x + l.skip.w);
  CHECK(leftMargin - rightMargin <= 1 && rightMargin - leftMargin <= 1, "the pair is not centred");

  // Hit-testing: whole pill, half-open edges, and the gap is nothing.
  CHECK(hitTest(l, l.retry.x, l.retry.y) == Target::Retry, "the top-left corner of Retry is not Retry");
  CHECK(hitTest(l, l.retry.x + l.retry.w - 1, l.retry.y + l.retry.h - 1) == Target::Retry,
        "the bottom-right pixel of Retry is not Retry");
  CHECK(hitTest(l, l.retry.x + l.retry.w, l.retry.y) == Target::None, "one pixel past Retry still hits it");
  CHECK(hitTest(l, l.skip.x, l.skip.y + l.skip.h / 2) == Target::Skip, "Skip is not tappable");
  CHECK(hitTest(l, l.retry.x + l.retry.w + spec.gap / 2, l.retry.y + 1) == Target::None,
        "the gap between the pills reports a target");
  CHECK(hitTest(l, l.retry.x, l.retry.y - 1) == Target::None, "a tap above the pills hits one");

  // A narrow screen shrinks the pair instead of pushing a pill off the panel,
  // and the centre stays centred.
  {
    const Spec narrow{480, 800, 220, 64, 24, 520};
    const Layout n = layout(narrow);
    CHECK(n.skip.x + n.skip.w <= narrow.screenW, "on a narrow screen a pill leaves the panel");
    CHECK(n.retry.x >= 0, "on a narrow screen Retry starts off-panel");
    CHECK(n.retry.w == n.skip.w && n.retry.w < 220, "the pills did not shrink on a narrow screen");
    CHECK(n.retry.w >= 1, "the pills collapsed to nothing");
  }

  // Degenerate input must not produce a negative or zero rect that silently
  // eats every tap: width floors at 1 and hitting is then still consistent.
  {
    const Spec tiny{40, 40, 10, 10, 24, 10};
    const Layout t = layout(tiny);
    CHECK(t.retry.w >= 1 && t.skip.w >= 1, "a tiny screen produced empty pills");
    CHECK(hitTest(t, -5, -5) == Target::None, "a tap outside the screen reported a target");
  }

  std::printf("pdf fail card: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
