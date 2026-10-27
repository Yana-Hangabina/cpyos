#include "FullScreenMessageActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include "components/UITheme.h"
#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();
  paint();
}

void FullScreenMessageActivity::paint() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const bool cpy = UITheme::getInstance().usesCpyLayout();

  renderer.clearScreen();

  if (!cpy || !retryable) {
    // Legacy / plain form: one centred line, unchanged from before.
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = (screenH - height) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, top, text.c_str(), true, style);
    renderer.displayBuffer(refreshMode);
    return;
  }

  // Design frame 19: a "!" mark over a title, an explanation line and one
  // action capsule. I18N is not initialised yet on this path (it is loaded
  // after the card mounts), so both lines are literal English -- the only
  // honest choice when no translation table can be reached.
  const int cx = screenW / 2;
  const int markY = screenH * 34 / 100;
  constexpr int kMarkRadius = 46;
  renderer.drawRoundedRect(cx - kMarkRadius, markY, kMarkRadius * 2, kMarkRadius * 2, 3, kMarkRadius, true);
  const int bangHeight = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawCenteredText(UI_12_FONT_ID, markY + (kMarkRadius * 2 - bangHeight) / 2, "!", true,
                            EpdFontFamily::BOLD);

  const int titleY = markY + kMarkRadius * 2 + 40;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, text.c_str(), true, EpdFontFamily::BOLD);

  if (!detail.empty()) {
    const int detailY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 14;
    renderer.drawCenteredText(SMALL_FONT_ID, detailY, detail.c_str(), true, EpdFontFamily::REGULAR);
  }

  // Action capsule, bottom-anchored so it sits in thumb reach.
  constexpr int kButtonW = 240;
  constexpr int kButtonH = 64;
  const int bx = (screenW - kButtonW) / 2;
  const int by = screenH - 220;
  renderer.drawRoundedRect(bx, by, kButtonW, kButtonH, 2, kButtonH / 2, true);
  const int labelHeight = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawCenteredText(UI_10_FONT_ID, by + (kButtonH - labelHeight) / 2, "Retry", true, EpdFontFamily::BOLD);
  retryRect_ = Rect{bx, by, kButtonW, kButtonH};

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void FullScreenMessageActivity::loop() {
  if (!retryable || retryRect_.width <= 0) return;
  if (!mappedInput.wasTapInRect(retryRect_.x, retryRect_.y, retryRect_.width, retryRect_.height)) return;

  LOG_INF("MAIN", "SD card retry requested");
  if (Storage.begin()) {
    // The whole boot sequence after the card mount (settings, i18n, library
    // index, UI) was skipped on the failure path; re-running it is exactly a
    // cold boot with the card present.
    LOG_INF("MAIN", "SD card remounted; restarting");
    ESP.restart();
  }
  LOG_ERR("MAIN", "SD card retry failed; staying on the error screen");
  requestUpdate();
}

void FullScreenMessageActivity::render(RenderLock&& lock) {
  (void)lock;
  paint();
}
