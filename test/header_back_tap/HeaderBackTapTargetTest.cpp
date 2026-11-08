// Host regression for HeaderBackTapTarget: the padded Back hit area must cover
// near misses (the drawn disc is small, and a miss fell through to the status
// bar and opened the frontlight sheet) while containsUnpadded() keeps the exact
// rect for callers that need the drawn geometry.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "components/HeaderBackTapTarget.h"

namespace {

int g_failures = 0;

void expect(const bool got, const bool want, const char* what) {
  if (got == want) return;
  std::printf("FAIL: %s (got %d want %d)\n", what, static_cast<int>(got), static_cast<int>(want));
  ++g_failures;
}

}  // namespace

int main() {
  // 32x32 disc at (4,4), then cleared.
  expect(HeaderBackTapTarget::contains(10, 10), false, "cleared target must not match");

  HeaderBackTapTarget::set(4, 4, 32, 32);

  // Inside the drawn disc: both views agree.
  for (const int x : {4, 10, 35}) {
    for (const int y : {4, 10, 35}) {
      expect(HeaderBackTapTarget::containsUnpadded(x, y), true, "inside rect");
      expect(HeaderBackTapTarget::contains(x, y), true, "inside rect, padded");
    }
  }

  // Up to 12 px outside on any side: padded only.
  const struct {
    int x, y;
    bool padded;
  } nearMisses[] = {
      {-8, 10, true},   // 12 px left of x
      {10, -8, true},   // 12 px above y
      {47, 10, true},   // 12 px right of x+w-1
      {10, 47, true},   // 12 px below y+h-1
      {-9, 10, false},  // one past the padding
      {48, 10, false},  //
      {10, -9, false},  //
      {10, 48, false},  //
      {60, 0, false},   // far corner: status-bar band, must still reach it
  };
  for (const auto& miss : nearMisses) {
    expect(HeaderBackTapTarget::containsUnpadded(miss.x, miss.y), false, "near miss is outside the exact rect");
    expect(HeaderBackTapTarget::contains(miss.x, miss.y), miss.padded, "near miss padding");
  }

  // The padded area is symmetric: the same offset on each side behaves the same.
  for (const int d : {1, 6, 12}) {
    const bool left = HeaderBackTapTarget::contains(4 - d, 20);
    const bool right = HeaderBackTapTarget::contains(4 + 32 - 1 + d, 20);
    const bool top = HeaderBackTapTarget::contains(20, 4 - d);
    const bool bottom = HeaderBackTapTarget::contains(20, 4 + 32 - 1 + d);
    expect(left && right && top && bottom, true, "padding is symmetric");
  }

  HeaderBackTapTarget::clear();
  expect(HeaderBackTapTarget::contains(10, 10), false, "clear() disarms both views");
  expect(HeaderBackTapTarget::containsUnpadded(10, 10), false, "clear() disarms both views");

  if (g_failures == 0) std::puts("HeaderBackTapTarget: all checks passed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
