// Host coverage for the Find-books vertical plan (src/activities/home/FindBooksPlan.h).
//
// The defect this guards: at 125% the body shrinks to 759 design px while the
// authored card floor (type-scaled header block + fixed insets + capsule) needs
// 184 px per card, so the four-card stack was drawn 161 px past the body and
// covered the tab bar.
#include <cstdio>
#include <cstdlib>

#include "activities/home/FindBooksPlan.h"

namespace {

int checks = 0;
int failures = 0;

void check(const bool ok, const char* what, const int line) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL line %d: %s\n", line, what);
  }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

// The two real bodies on Read Pico (684x1216 panel): 100% and 125%.
constexpr find_books_plan::Design kDesign{16, 88, 16, 184, 64, 56, 18, 8};
constexpr int kBody100 = 1003;
constexpr int kBody125 = 759;

void testAuthoredGeometryAt100() {
  const auto p = find_books_plan::planFor(kDesign, kBody100, 4, 30, 27);
  // The authored geometry the design was signed off on, unchanged.
  CHECK(p.topPad == 16);
  CHECK(p.arrivalH == 88);
  CHECK(p.gap == 16);
  CHECK(p.cardH == 184);
  CHECK(p.padY == 18);
  CHECK(p.innerGap == 8);
  CHECK(p.pillH == 56);
  CHECK(p.seal == 64);
  CHECK(find_books_plan::stackHeight(p, 4) == 920);
  CHECK(find_books_plan::stackHeight(p, 4) <= kBody100);
}

void testCompressedGeometryAt125() {
  // typeBlock = 46 + 3 + 35 = 84 (the 125% line heights the device measured).
  const auto p = find_books_plan::planFor(kDesign, kBody125, 4, 46, 35);
  CHECK(p.cardH < kDesign.cardHeight);
  CHECK(p.cardH > 0);
  const int stack = find_books_plan::stackHeight(p, 4);
  CHECK(stack <= kBody125);
  // The plan uses the body: the leftover is smaller than one card row.
  CHECK(kBody125 - stack < p.cardH + p.gap);
  // The interior still fits inside the card: capsules never ride the title.
  CHECK(find_books_plan::interiorHeight(p, 84) <= p.cardH);
  // Type is never squeezed: the header block keeps the scaled lines.
  CHECK(find_books_plan::headerBlock(p, 84) >= 84);
  // The capsule still holds its text line.
  CHECK(p.pillH >= 35 + 8);
}

void testNeverOverflows() {
  for (int band = 320; band <= 1400; band += 1) {
    for (int typeBlock : {40, 63, 84, 110}) {
      const auto p = find_books_plan::planFor(kDesign, band, 4, typeBlock - 3 - 27, 27);
      const int stack = find_books_plan::stackHeight(p, 4);
      check(stack <= band, "stack fits the body", __LINE__);
      check(p.cardH >= 1, "card has height", __LINE__);
    }
  }
}

void testCountsAndSmallPanels() {
  // Two sources (no Chinese apps): the cards keep the design height.
  const auto two = find_books_plan::planFor(kDesign, kBody100, 2, 30, 27);
  CHECK(two.cardH == 184);
  CHECK(find_books_plan::stackHeight(two, 2) <= kBody100);
  // C3-sized body: still inside.
  const auto small = find_books_plan::planFor(kDesign, 360, 4, 40, 30);
  CHECK(find_books_plan::stackHeight(small, 4) <= 360);
  // Zero entries must not divide by zero or return a degenerate plan.
  const auto none = find_books_plan::planFor(kDesign, kBody125, 0, 46, 35);
  CHECK(none.cardH >= 1);
  CHECK(find_books_plan::stackHeight(none, 0) <= kBody125);
}

}  // namespace

int main() {
  testAuthoredGeometryAt100();
  testCompressedGeometryAt125();
  testNeverOverflows();
  testCountsAndSmallPanels();
  std::printf("FindBooksPlan: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
