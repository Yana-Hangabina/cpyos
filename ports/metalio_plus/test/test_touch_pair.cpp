#include "touch_pair.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

void test_expand() {
  plus_touch_pair::State state;
  plus_touch_pair::Pair p{true, 100, 100, 200, 100};
  auto out = plus_touch_pair::evaluate(p, state, 684, 1216);
  check(!out.fire, "first two-finger sample only sets the baseline");
  p.x2 = 250;
  out = plus_touch_pair::evaluate(p, state, 684, 1216);
  check(out.fire, "expansion fires");
  check(near(out.scale, 1.5f), "cumulative scale 1.5");
  check(near(out.cx, 175.0f / 684.0f), "centre x");
  check(near(out.cy, 100.0f / 1216.0f), "centre y");
  p.x2 = 400;
  out = plus_touch_pair::evaluate(p, state, 684, 1216);
  check(!out.fire, "one event per gesture");
}

void test_contract_and_reset() {
  plus_touch_pair::State state;
  plus_touch_pair::Pair p{true, 300, 100, 100, 100};
  plus_touch_pair::evaluate(p, state, 684, 1216);
  p.x2 = 200;  // distance 100 from 200 -> half
  auto out = plus_touch_pair::evaluate(p, state, 684, 1216);
  check(out.fire && near(out.scale, 0.5f), "contraction fires with 0.5");
  p.down = false;
  out = plus_touch_pair::evaluate(p, state, 684, 1216);
  check(!out.fire && state.startDistance == 0, "release resets the gesture");
}

void test_threshold_and_noise() {
  plus_touch_pair::State state;
  plus_touch_pair::Pair p{true, 100, 100, 200, 100};
  plus_touch_pair::evaluate(p, state, 684, 1216);
  p.x2 = 210;  // 1.10
  check(!plus_touch_pair::evaluate(p, state, 684, 1216).fire, "1.10 is below threshold");
  p.x2 = 216;  // 1.16
  check(plus_touch_pair::evaluate(p, state, 684, 1216).fire, "1.16 crosses threshold");

  plus_touch_pair::State close;
  plus_touch_pair::Pair q{true, 100, 100, 120, 100};
  plus_touch_pair::evaluate(q, close, 684, 1216);
  q.x2 = 200;
  check(!plus_touch_pair::evaluate(q, close, 684, 1216).fire, "20 px contacts are ignored");
}
}  // namespace

int main() {
  test_expand();
  test_contract_and_reset();
  test_threshold_and_noise();
  if (failures == 0) std::printf("touch pair: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
