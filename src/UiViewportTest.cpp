#include <cstdio>
#include <cstdlib>

#include "UiViewport.h"

namespace {
int failures = 0;

void check(bool condition, const char* what, int line) {
  if (condition) return;
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, what);
  ++failures;
}

#define CHECK(expr) check((expr), #expr, __LINE__)

using ui_viewport::DesignSpec;
using ui_viewport::Viewport;

void testIdentity() {
  const Viewport v = ui_viewport::resolve(DesignSpec{684, 1216}, 684, 1216, 100);
  CHECK(v.scalePermille == 1000);
  CHECK(v.width == 684 && v.height == 1216);
  CHECK(v.identity());
  CHECK(v.toNative(0) == 0);
  CHECK(v.toNative(96) == 96);
  CHECK(v.toDesign(96) == 96);
  CHECK(v.lengthToNative(0) == 0);
  CHECK(v.lengthToNative(1) == 1);
}

void testUserScale() {
  const Viewport v125 = ui_viewport::resolve(DesignSpec{684, 1216}, 684, 1216, 125);
  CHECK(v125.scalePermille == 1250);
  CHECK(v125.width == 547);
  CHECK(v125.height == 972);
  CHECK(!v125.identity());
  CHECK(v125.toNative(96) == 120);
  CHECK(v125.toDesign(120) == 96);
  CHECK(v125.lengthToNative(1) == 1);

  const Viewport v75 = ui_viewport::resolve(DesignSpec{684, 1216}, 684, 1216, 75);
  CHECK(v75.scalePermille == 750);
  CHECK(v75.toNative(40) == 30);
  CHECK(v75.lengthToNative(1) == 1);
}

void testClamp() {
  CHECK(ui_viewport::resolve(DesignSpec{684, 1216}, 684, 1216, 10).scalePermille == 500);
  CHECK(ui_viewport::resolve(DesignSpec{684, 1216}, 684, 1216, 250).scalePermille == 2000);
}

void testOrientationSwap() {
  const Viewport v = ui_viewport::resolve(DesignSpec{684, 1216}, 1216, 684, 100);
  CHECK(v.scalePermille == 1000);
  CHECK(v.width == 1216 && v.height == 684);
  CHECK(v.identity());
}

void testDifferentPanel() {
  const Viewport v = ui_viewport::resolve(DesignSpec{684, 1216}, 480, 800, 100);
  CHECK(v.scalePermille == 657);
  CHECK(v.width == 730);
  CHECK(v.height == 1217);
  CHECK(v.toNative(96) == 63);
  CHECK(v.toDesign(63) == 96);
}

void testEdgeMapping() {
  const Viewport v = ui_viewport::resolve(DesignSpec{684, 1216}, 684, 1216, 125);
  const int x1 = v.toNative(10);
  const int x2 = v.toNative(10 + 30);
  // Rectangles map both edges; the width is the difference of the two mapped
  // edges (not a separately rounded length, which would leave a 1px seam).
  CHECK(x2 - x1 == v.toNative(40) - v.toNative(10));
  CHECK(x2 - x1 >= 1);
  const int y1 = v.toNative(100);
  const int y2 = v.toNative(100 + 20);
  CHECK(y2 - y1 == v.toNative(120) - v.toNative(100));
}

void testRoundTrip() {
  const Viewport v = ui_viewport::resolve(DesignSpec{684, 1216}, 1216, 684, 115);
  for (int value = 0; value <= 600; value += 7) {
    const int native = v.toNative(value);
    const int back = v.toDesign(native);
    CHECK(std::abs(back - value) <= 1);
  }
}

void testZeroDesignFallsBackToNative() {
  const Viewport v = ui_viewport::resolve(DesignSpec{}, 1000, 700, 100);
  CHECK(v.scalePermille == 1000);
  CHECK(v.width == 1000 && v.height == 700);
  CHECK(v.identity());
  // Panels without an authored design (e.g. the 800x480 Metalio Plus) must
  // still get a plain user zoom instead of a letterboxed readpico spec.
  const Viewport plus = ui_viewport::resolve(DesignSpec{}, 800, 480, 125);
  CHECK(plus.scalePermille == 1250);
  CHECK(plus.width == 640 && plus.height == 384);
  CHECK(plus.toNative(96) == 120);
  const Viewport plusSmall = ui_viewport::resolve(DesignSpec{}, 800, 480, 75);
  CHECK(plusSmall.width == 1066 && plusSmall.height == 640);
}

}  // namespace

int main() {
  testIdentity();
  testUserScale();
  testClamp();
  testOrientationSwap();
  testDifferentPanel();
  testEdgeMapping();
  testRoundTrip();
  testZeroDesignFallsBackToNative();
  if (failures == 0) {
    std::printf("UiViewport: ALL PASS\n");
    return 0;
  }
  std::printf("UiViewport: %d failure(s)\n", failures);
  return 1;
}
