#include "PhotoDemoActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>

#include <algorithm>

#include "fontIds.h"

namespace {
// 380x380 of a 1216x684 panel is ~17%: above band_policy's 10% debt line, so each
// update charges the sweep counter and one of the twelve runs the scheduled clean.
constexpr int kBoxW = 380;
constexpr int kBoxH = 380;
constexpr int kUpdates = 12;
constexpr int kStepPx = 24;
}  // namespace

void PhotoDemoActivity::drawTestPage() const {
  renderer.clearScreen();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int line = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, 80, 80, "GHOST DEMO 0123456789", true, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, 80, 80 + line + 8, "abcdefghijklmnopqrstuvwxyz", true, EpdFontFamily::REGULAR);
  // 50% dither: the classic residue showroom.
  renderer.fillRectDither(80, 80 + 2 * (line + 8), 400, 160, Color::LightGray);
  // Solid black: shows the opposite polarity.
  renderer.fillRect(520, 80 + 2 * (line + 8), 200, 160, true);
  // Four-pixel ticks: a windowed push that misplaces its mask shows these shifted.
  for (int i = 0; i < 8; ++i) {
    renderer.fillRect(80 + i * 4, h - 200, 1, 60, true);
  }
  (void)w;
}

void PhotoDemoActivity::onEnter() {
  Activity::onEnter();
  const bool localClean = mode_ == 1;
  prevLocalClean_ = GfxRenderer::localCleanEnabled();
  GfxRenderer::setLocalCleanEnabled(localClean);
  // onEnter runs before ActivityManager's next loop tick picks this activity's
  // wantsBandUpdates(), so turn the mode on explicitly for the demo.
  renderer.setBandUpdates(true);
  LOG_ERR("PHOTO", "demo start mode=%d (local clean %s)", mode_, localClean ? "ON" : "off");

  {
    RenderLock lock;
    // 1) Test page from a clean panel.
    drawTestPage();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);

    // 2) Twelve updates of a board-sized box walking across the page. Each one is
    // a band (17%) and charges ghost debt; with the user's sweep interval (6 in
    // Standard) some pass runs the scheduled clean -- whole screen in mode 0,
    // dirty rect only in mode 1.
    for (int i = 0; i < kUpdates; ++i) {
      const int x = 60 + (i % 10) * kStepPx;
      const int y = 60 + (i % 6) * kStepPx;
      renderer.fillRect(x, y, kBoxW, kBoxH, (i & 1) == 0);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      delay(260);
    }

    // 3) Blank page, pushed WITHOUT a clean: whatever residue the run produced is
    // now on the glass for the photo.
    renderer.clearScreen();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Leave the demo policy as the real one: the switch is a debug lever, not a
  // setting, and the next screen must not inherit it.
  // Restore whatever the operator had set; the demo must not change policy.
  GfxRenderer::setLocalCleanEnabled(prevLocalClean_);
  renderer.setBandUpdates(false);
  LOG_ERR("PHOTO", "demo done mode=%d", mode_);
}
