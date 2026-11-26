#include "PowerOffActivity.h"

#include <Arduino.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <esp_system.h>

#if FREEINK_DEVICE_READPICO && !defined(SIMULATOR)
#include <BoardReadPico.h>
#endif

#include "components/UITheme.h"
#include "components/themes/cpy/CpyDuckDraw.h"
#include "components/themes/cpy/cpy_duck_assets.h"

namespace {
bool hit(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}
}  // namespace

void PowerOffActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("PWR", "shutdown confirmation shown");
  requestUpdate();
}

void PowerOffActivity::cancel() {
  if (shuttingDown_) return;  // the power-off owns the screen now
  LOG_INF("PWR", "shutdown cancelled");
  finish();
}

void PowerOffActivity::drawShutdownScreen() {
  // The panel is bistable, so this frame is what the device shows while it is
  // off (the vendor firmware draws a power-off wallpaper for the same reason).
  // Full GC16 so no card residue survives the transition.
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
#if FREEINK_DEVICE_READPICO && !defined(SIMULATOR)
  // Cooperative power-off: HOST_REQUEST_OFF -> SHUTDOWN_REQUESTED ->
  // SHUTDOWN_READY, and the PMU drops the host EN rail.  The PMU owns the only
  // wake source (the key), so there is no ESP deep-sleep step here.
  if (BoardReadPico::pmuPowerOff()) {
    vTaskDelay(pdMS_TO_TICKS(900));  // EN should have dropped; survive USB keeping us up
  } else {
    LOG_ERR("PWR", "PMU power-off handoff failed; restarting instead of staying on");
  }
#endif
  // The PMU normally cuts power here.  If USB keeps the host alive (or the PMU
  // refused), restart rather than keep running a device the user asked to turn
  // off -- the same fallback the Metalio Plus card uses.
  esp_restart();
}

void PowerOffActivity::debugConfirm() { confirmOff(); }
void PowerOffActivity::debugCancel() { cancel(); }

void PowerOffActivity::loop() {
  if (shuttingDown_) return;
  int x = 0;
  int y = 0;
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
    return;
  }
  // A short power tap must not turn "cancelled" into a standby overlay on top of
  // this card: swallow it (the card is the only surface until Back/confirm).
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
  renderer.drawText(UI_12_FONT_ID, cardX + (cardW - renderer.getTextWidth(UI_12_FONT_ID, title)) / 2, y, title,
                    true, EpdFontFamily::BOLD);
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
  // A transient half refresh: the card is pushed while the key is still held, so
  // ~0.4 s from the prompt to the card instead of two panel scans.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, DisplayRefreshContext::TransientUi);
}
