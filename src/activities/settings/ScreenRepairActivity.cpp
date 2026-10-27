#include "ScreenRepairActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>

void ScreenRepairActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("SCREEN", "Screen repair: white -> black -> white");
  step = Step::WhiteFirst;
  // The whole sequence runs inside one render pass, so the render lock is held
  // for all three refreshes and nothing can repaint between them.
  requestUpdate();
}

void ScreenRepairActivity::render(RenderLock&&) {
  if (step == Step::Done) return;

  // FULL_REFRESH is the point of the exercise: on Read Pico FAST is promoted to
  // HALF (GfxRenderer's readpico policy), and HALF rewrites the frame with GL16
  // without clearing the residual charge a smeared screen is made of. Sleep's
  // white wash (SleepActivity) uses the same FULL pass for the same reason.
  constexpr uint8_t kWhite = 0xFF;
  constexpr uint8_t kBlack = 0x00;
  const uint8_t frames[] = {kWhite, kBlack, kWhite};

  for (const uint8_t color : frames) {
    renderer.clearScreen(color);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }

  step = Step::Done;
}

void ScreenRepairActivity::loop() {
  if (step != Step::Done) return;
  // The settings page comes back through ActivityManager's Pop. Ask for a full
  // refresh so it lands on a clean panel instead of a HALF rewrite of the white
  // frame we just left.
  renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
  finish();
}
