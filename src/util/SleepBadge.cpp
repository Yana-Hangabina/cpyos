#include "util/SleepBadge.h"

#include <GfxRenderer.h>

#include "fontIds.h"

void drawSleepStateBadge(const GfxRenderer& renderer, const char* text, const bool topLeft, const bool bottomLeft) {
  if (text == nullptr || *text == '\0') return;

  constexpr int kPadX = 12;
  constexpr int kPadY = 6;
  constexpr int kMargin = 18;
  constexpr int kFont = UI_10_FONT_ID;
  const int textWidth = renderer.getTextWidth(kFont, text);
  const int textHeight = renderer.getLineHeight(kFont);
  const int width = textWidth + kPadX * 2;
  const int height = textHeight + kPadY * 2;
  const int x = (topLeft || bottomLeft) ? kMargin : renderer.getScreenWidth() - width - kMargin;
  const int y = topLeft ? kMargin : renderer.getScreenHeight() - height - kMargin;

  renderer.fillRect(x, y, width, height, false);
  renderer.drawRect(x, y, width, height, 1, true);
  renderer.drawText(kFont, x + kPadX, y + kPadY, text, true);
}
