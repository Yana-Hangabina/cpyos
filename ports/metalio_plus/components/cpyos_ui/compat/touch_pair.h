#pragma once

#include <cmath>

// Pinch classification for the FT6336U two-point report.
//
// Semantics match what the reader expects: one event per gesture carrying the
// *cumulative* scale since the second finger landed (1.0 = no change), fired
// once when the distance has changed by at least `threshold`. The reader applies
// one font step for scales >= 1.15 / <= 0.85, so the default is 0.15.
//
// Pure math (no I2C) so the thresholds and centre normalisation can be
// host-tested; the sampling lives in the vendor touch driver and HalGPIO.
namespace plus_touch_pair {

struct Pair {
  bool down;
  int x1, y1;
  int x2, y2;
};

struct State {
  int startDistance = 0;  // 0 = no gesture in progress
  bool fired = false;
};

struct Pinch {
  bool fire;
  float scale;  // cumulative, >1 expand, <1 contract
  float cx;     // normalised midpoint, 0..1
  float cy;
};

inline float distance(const Pair& p) {
  const float dx = static_cast<float>(p.x2 - p.x1);
  const float dy = static_cast<float>(p.y2 - p.y1);
  return std::sqrt(dx * dx + dy * dy);
}

inline Pinch evaluate(const Pair& p, State& state, int width, int height,
                      int minDistance = 40, float threshold = 0.15f) {
  Pinch out{false, 1.0f, 0.0f, 0.0f};
  if (!p.down) {
    state.startDistance = 0;
    state.fired = false;
    return out;
  }
  const float d = distance(p);
  if (d < static_cast<float>(minDistance)) {
    // Two contacts this close are noise, not a deliberate pinch.
    state.startDistance = 0;
    state.fired = false;
    return out;
  }
  if (state.startDistance == 0) {
    state.startDistance = static_cast<int>(d);
    state.fired = false;
    return out;
  }
  const float scale = d / static_cast<float>(state.startDistance);
  if (!state.fired && (scale >= 1.0f + threshold || scale <= 1.0f - threshold)) {
    out.fire = true;
    out.scale = scale;
    out.cx = (static_cast<float>(p.x1 + p.x2) * 0.5f) / static_cast<float>(width);
    out.cy = (static_cast<float>(p.y1 + p.y2) * 0.5f) / static_cast<float>(height);
    state.fired = true;
  }
  return out;
}

}  // namespace plus_touch_pair