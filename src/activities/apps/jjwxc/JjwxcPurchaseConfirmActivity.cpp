#include "JjwxcPurchaseConfirmActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
constexpr int kSide = 40;
constexpr int kCardW = 604;
constexpr int kCardH = 780;
constexpr int kInner = 30;
constexpr int kRowH = 54;

void drawCoin(const GfxRenderer& renderer, const int cx, const int cy, const int radius) {
  renderer.fillRoundedRect(cx - radius, cy - radius, radius * 2, radius * 2, radius, Color::Black);
  const char* yen = "\xC2\xA5";  // ¥
  const int w = renderer.getTextWidth(SMALL_FONT_ID, yen);
  renderer.drawText(SMALL_FONT_ID, cx - w / 2, cy - renderer.getLineHeight(SMALL_FONT_ID) / 2, yen, false);
}
}  // namespace

void JjwxcPurchaseConfirmActivity::onEnter() {
  Activity::onEnter();
  requestUpdate(true);
}

void JjwxcPurchaseConfirmActivity::confirm() {
  ActivityResult result;
  result.isCancelled = false;
  setResult(std::move(result));
  finish();
}

void JjwxcPurchaseConfirmActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void JjwxcPurchaseConfirmActivity::drawLock(const int x, const int y, const int size) {
  const int bodyH = size * 3 / 5;
  renderer.fillRoundedRect(x, y + size - bodyH, size, bodyH, 3, Color::Black);
  const int shackle = size / 2;
  for (int i = 0; i < 3; ++i) {
    renderer.drawArc(shackle - i, x + size / 2, y + size - bodyH, -1, -1, 1, true);
    renderer.drawArc(shackle - i, x + size / 2, y + size - bodyH, 1, -1, 1, true);
  }
}

void JjwxcPurchaseConfirmActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (x >= confirmRect_.x && x < confirmRect_.x + confirmRect_.width && y >= confirmRect_.y &&
        y < confirmRect_.y + confirmRect_.height) {
      confirm();
      return;
    }
    if (x >= cancelRect_.x && x < cancelRect_.x + cancelRect_.width && y >= cancelRect_.y &&
        y < cancelRect_.y + cancelRect_.height) {
      cancel();
      return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
  }
}

void JjwxcPurchaseConfirmActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int cardX = (screenW - kCardW) / 2;
  const int cardY = std::max(20, (screenH - kCardH) / 2 - 20);
  const int innerX = cardX + kInner;
  const int innerW = kCardW - kInner * 2;

  // Context line above the bill, like the page the dialog was opened over.
  const std::string context = work_ + " · " + chapter_;
  renderer.drawText(UI_10_FONT_ID, kSide, cardY - 34, renderer.truncatedText(UI_10_FONT_ID, context.c_str(), screenW - 200).c_str());
  char clock[9] = "--:--";
  TimeUtils::formatCurrentTime(clock, sizeof(clock), SETTINGS.clockFormat == 1);
  const int clockW = renderer.getTextWidth(UI_10_FONT_ID, clock);
  renderer.drawText(UI_10_FONT_ID, screenW - kSide - clockW, cardY - 34, clock);
  renderer.drawLine(kSide, cardY - 12, screenW - kSide, cardY - 12, true);

  renderer.fillRoundedRect(cardX, cardY, kCardW, kCardH, 20, Color::White);
  renderer.drawRoundedRect(cardX, cardY, kCardW, kCardH, 2, 20, true);

  int y = cardY + 24;
  char sourceLine[64];
  std::snprintf(sourceLine, sizeof(sourceLine), "%s · %s", tr(STR_SOURCE_JJWXC), chapter_.c_str());
  renderer.drawText(UI_10_FONT_ID, innerX, y, renderer.truncatedText(UI_10_FONT_ID, sourceLine, innerW).c_str());
  y += 32;
  drawLock(innerX, y + 2, 28);
  renderer.drawText(NOTOSERIF_18_FONT_ID, innerX + 40, y, tr(STR_JJWXC_PURCHASE_TITLE), true, EpdFontFamily::BOLD);
  y += 60;
  renderer.drawLine(innerX, y, innerX + innerW, y, true);
  y += 10;

  const auto row = [&](const char* label, const std::string& value, const bool coin) {
    const int textY = y + (kRowH - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    if (!coin) renderer.drawText(UI_10_FONT_ID, innerX, textY, label);
    if (coin) drawCoin(renderer, innerX + 9, y + kRowH / 2, 9);
    const int valueX = innerX + (coin ? 30 : 0);
    if (coin) renderer.drawText(UI_10_FONT_ID, valueX, textY, label);
    const std::string shown =
        renderer.truncatedText(UI_10_FONT_ID, value.c_str(), innerW - (valueX - innerX));
    const int valueW = renderer.getTextWidth(UI_10_FONT_ID, shown.c_str());
    renderer.drawText(UI_10_FONT_ID, innerX + innerW - valueW, textY, shown.c_str());
    renderer.drawLine(innerX, y + kRowH, innerX + innerW, y + kRowH, true);
    y += kRowH;
  };

  char chapterValue[128];
  std::snprintf(chapterValue, sizeof(chapterValue), "%s", chapter_.c_str());
  char priceValue[32];
  std::snprintf(priceValue, sizeof(priceValue), I18N.get(StrId::STR_JJWXC_PURCHASE_POINTS_FMT),
                static_cast<unsigned>(points_));

  row(tr(STR_JJWXC_PURCHASE_WORK), work_, false);
  row(tr(STR_JJWXC_PURCHASE_CHAPTER_ROW), chapterValue, false);
  row(tr(STR_JJWXC_PURCHASE_UNIT_PRICE), priceValue, true);
  row(tr(STR_JJWXC_PURCHASE_METHOD), tr(STR_JJWXC_PURCHASE_METHOD_JJWXC), false);
  row(tr(STR_JJWXC_PURCHASE_TOTAL), priceValue, false);

  y += 14;
  {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_JJWXC_PURCHASE_NOTE), innerW - 28, 3);
    for (const auto& line : lines) {
      if (line.empty()) continue;
      renderer.drawText(UI_10_FONT_ID, innerX + 28, y, line.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID) + 2;
    }
    // Check mark beside the note.
    renderer.drawLine(innerX + 4, y - 58, innerX + 10, y - 52, 2, true);
    renderer.drawLine(innerX + 10, y - 52, innerX + 20, y - 66, 2, true);
  }

  y += 8;
  confirmRect_ = Rect{innerX, y, innerW, 88};
  renderer.fillRoundedRect(confirmRect_.x, confirmRect_.y, confirmRect_.width, confirmRect_.height, 12, Color::Black);
  {
    const char* label = tr(STR_JJWXC_PURCHASE_CONFIRM);
    const int w = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, confirmRect_.x + (confirmRect_.width - w) / 2,
                      confirmRect_.y + (confirmRect_.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2, label,
                      false, EpdFontFamily::BOLD);
  }
  y += confirmRect_.height + 10;
  cancelRect_ = Rect{innerX, y, innerW, 80};
  renderer.drawRoundedRect(cancelRect_.x, cancelRect_.y, cancelRect_.width, cancelRect_.height, 2, 12, true);
  {
    const char* label = tr(STR_CANCEL);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, label);
    renderer.drawText(UI_10_FONT_ID, cancelRect_.x + (cancelRect_.width - w) / 2,
                      cancelRect_.y + (cancelRect_.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, label);
  }

  // Dashed footer rule and the "this chapter only" reassurance.
  const int footerY = cardY + kCardH - 62;
  for (int x = innerX; x < innerX + innerW; x += 10) {
    renderer.drawLine(x, footerY, std::min(x + 5, innerX + innerW), footerY, true);
  }
  renderer.drawText(SMALL_FONT_ID, innerX, footerY + 10, tr(STR_JJWXC_PURCHASE_ONLY_CHAPTER));

  renderer.displayBuffer();
}
