#include "power_off_activity.h"

#include <Arduino.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <esp_system.h>

#include "components/UITheme.h"
#include "components/themes/cpy/CpyDuckDraw.h"
#include "components/themes/cpy/cpy_duck_assets.h"
#include "epd_board_metalio_eink4_plus.h"

namespace {
bool hit(const Rect& r, int x, int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}
}  // namespace

void PowerOffActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("PWR", "shutdown confirmation shown");
  requestUpdate();
}

void PowerOffActivity::cancel() {
  if (shuttingDown_) return;  // the power-off pulse/restart owns the screen now
  LOG_INF("PWR", "shutdown cancelled");
  finish();
}

void PowerOffActivity::drawShutdownScreen() {
  // A themed "powering off" frame instead of leaving the confirmation card on
  // the panel. The panel is bistable, so this is what the device shows while it
  // is off (the vendor firmware draws a power-off wallpaper for the same
  // reason); it is pushed as a full GC16 so no card residue survives.
  RenderLock lock;
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  renderer.clearScreen();
  constexpr int kDuck = 200;
  const int duckX = (w - kDuck) / 2;
  const int duckY = h / 2 - kDuck - 24;
  cpy_duck::draw(renderer, cpy_duck_sleep_200, duckX, duckY);
  constexpr const char* kLine = "已关机";
  const int textW = renderer.getTextWidth(UI_12_FONT_ID, kLine, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, (w - textW) / 2, duckY + kDuck + 32, kLine, true, EpdFontFamily::BOLD);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void PowerOffActivity::confirmOff() {
  if (shuttingDown_) return;
  shuttingDown_ = true;
  LOG_ERR("PWR", "shutdown confirmed; drawing the power-off screen");
  drawShutdownScreen();
  epd_board_metalio_eink4_plus_power_off_pulse();
  // The PMU normally cuts the host here. If USB keeps it alive, restart rather
  // than continue running a device the user asked to turn off.
  vTaskDelay(pdMS_TO_TICKS(900));
  esp_restart();
}

void PowerOffActivity::debugConfirm() { confirmOff(); }
void PowerOffActivity::debugCancel() { cancel(); }

void PowerOffActivity::loop() {
  if (shuttingDown_) return;
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (hit(confirmRect_, x, y)) {
      confirmOff();
      return;
    }
    if (hit(cancelRect_, x, y)) {
      cancel();
      return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    cancel();  // the safe default
  }
}

void PowerOffActivity::render(RenderLock&&) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  renderer.clearScreen();
  const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);

  const int btnH = 56;
  const int pad = 28;
  const int gap = 16;
  const int cardW = std::min(w - 2 * m.contentSidePadding, 560);
  const int cardH = pad + line12 + 18 + line10 + 8 + line10 + 28 + btnH + pad;
  const int cardX = (w - cardW) / 2;
  const int cardY = (h - cardH) / 2;
  GUI.drawModalCardFrame(renderer, Rect{cardX, cardY, cardW, cardH});

  int y = cardY + pad;
  const char* title = "关机";
  renderer.drawText(UI_12_FONT_ID, cardX + (cardW - renderer.getTextWidth(UI_12_FONT_ID, title)) / 2, y, title, true,
                    EpdFontFamily::BOLD);
  y += line12 + 18;
  const char* l1 = "电源键已按住 3 秒";
  renderer.drawText(UI_10_FONT_ID, cardX + (cardW - renderer.getTextWidth(UI_10_FONT_ID, l1)) / 2, y, l1);
  y += line10 + 8;
  const char* l2 = "确定要关机吗？";
  renderer.drawText(UI_10_FONT_ID, cardX + (cardW - renderer.getTextWidth(UI_10_FONT_ID, l2)) / 2, y, l2);
  y += line10 + 28;

  const int btnW = (cardW - 2 * pad - gap) / 2;
  cancelRect_ = Rect{cardX + pad, y, btnW, btnH};
  confirmRect_ = Rect{cardX + pad + btnW + gap, y, btnW, btnH};
  renderer.drawRoundedRect(cancelRect_.x, cancelRect_.y, cancelRect_.width, cancelRect_.height, 2, btnH / 2, true);
  const char* cancelLabel = "取消";
  renderer.drawText(UI_10_FONT_ID,
                    cancelRect_.x + (cancelRect_.width - renderer.getTextWidth(UI_10_FONT_ID, cancelLabel)) / 2,
                    cancelRect_.y + (btnH - line10) / 2, cancelLabel, true);
  renderer.fillRoundedRect(confirmRect_.x, confirmRect_.y, confirmRect_.width, confirmRect_.height, btnH / 2,
                           Color::Black);
  const char* offLabel = "关机";
  renderer.drawText(UI_10_FONT_ID,
                    confirmRect_.x + (confirmRect_.width - renderer.getTextWidth(UI_10_FONT_ID, offLabel)) / 2,
                    confirmRect_.y + (btnH - line10) / 2, offLabel, false);
  // Every activity owns its panel push. Use HALF with the transient context:
  // dropping the grayscale->1-bit promotion keeps the card visible ~0.4 s
  // after the hold threshold instead of ~0.9 s (the card is pushed while the
  // key is still held, see the shell's 2.5 s prompt lead).
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, DisplayRefreshContext::TransientUi);
}
