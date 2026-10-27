#include "DuckSleepFace.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <time.h>

#include <cmath>
#include <cstdio>

#include "StandbyTime.h"
#include "components/themes/cpy/CpyClockDraw.h"
#include "components/themes/cpy/CpyDuckDraw.h"

namespace {
constexpr float kTwoPi = 6.2831853f;
}  // namespace

void DuckSleepFace::onEnter() {
  startMs_ = millis();
  lastMin_ = -1;
}

void DuckSleepFace::onExit() {}

StandbyFace::TickResult DuckSleepFace::tick() {
  const uint32_t nowMin = standby_time::getMinuteTick(startMs_);
  if (static_cast<int32_t>(nowMin) == lastMin_) return TickResult::None;
  lastMin_ = static_cast<int32_t>(nowMin);

  const uint8_t currentBucket = standby_time::getTenMinuteBucket(startMs_);
  const bool cleanGhosting = standby_time::didTenMinuteBucketChange(lastTenMinuteBucket_, currentBucket);
  lastTenMinuteBucket_ = currentBucket;
  return cleanGhosting ? TickResult::RedrawWithGhostCleanup : TickResult::Redraw;
}

void DuckSleepFace::render(GfxRenderer& renderer, const Rect& viewport) {
  if (viewport.width <= 0 || viewport.height <= 0) return;

  // Songti extra-bold clock with the date line under it (frame 34's layout:
  // the block sits high on the page, ratio-based so C3 scales with it).
  unsigned hh = 0;
  unsigned mm = 0;
  standby_time::getNowHHMM(startMs_, hh, mm);
  const int centerX = viewport.x + viewport.width / 2;
  const int clockTop = viewport.y + viewport.height * 21 / 100;
  const int timeHeight = cpy_clock::drawTime(renderer, centerX, clockTop, hh, mm);
  cpy_clock::drawDateLine(renderer, centerX, clockTop + timeHeight + 12);

  // Water band low on the page (frame 34: ~220px off the bottom), wave line
  // on top of a light dither.
  const int waterTop = viewport.y + viewport.height * 79 / 100;
  constexpr int bandHeight = 40;
  const auto waveAt = [waterTop](const int x) {
    return waterTop + static_cast<int>(std::lround(3.0f * std::sin(static_cast<float>(x) * kTwoPi / 28.0f)));
  };
  for (int x = 0; x < viewport.width; ++x) {
    const int y0 = waveAt(x);
    renderer.fillRectDither(viewport.x + x, y0 + 1, 1, std::max(1, waterTop + bandHeight - y0 - 1), Color::LightGray);
    if (x + 1 < viewport.width) renderer.drawLine(viewport.x + x, y0, viewport.x + x + 1, waveAt(x + 1), true);
  }

  // Sleeping duck on the band; its waterline row is ~46/64 into the art.
  constexpr int duckSize = 200;
  const int duckX = viewport.x + (viewport.width - duckSize) / 2;
  const int duckY = waterTop - duckSize * 46 / 64;
  cpy_duck::draw(renderer, cpy_duck_sleep_200, duckX, duckY);
}

StrId DuckSleepFace::titleId() const { return StrId::STR_FACE_DUCK_SLEEP; }

uint32_t DuckSleepFace::secondsUntilNextWake() const {
  uint32_t sec;
  if (standby_time::isSynced()) {
    const time_t now = time(nullptr);
    sec = static_cast<uint32_t>(now % 60);
  } else {
    sec = static_cast<uint32_t>(((millis() - startMs_) / 1000u) % 60u);
  }
  const uint32_t remaining = (sec == 0) ? 60u : (60u - sec);
  return remaining < 1u ? 1u : remaining;
}
