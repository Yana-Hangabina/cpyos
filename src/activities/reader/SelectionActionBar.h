#pragma once

#include <algorithm>
#include <cstdint>

#include "GfxRenderer.h"

// Shared selection action bar (Lookup/Highlight/Note or Highlight/Note/Cancel).
// The EPUB reader and the PDF reflow reader use the same chip geometry and
// drawing so a long-press selection does not turn into a second, alien UI.
namespace selection_bar {

struct Hits {
  int16_t x[3] = {0, 0, 0};
  int16_t y[3] = {0, 0, 0};
  int16_t w[3] = {0, 0, 0};
  int16_t h[3] = {0, 0, 0};
};

inline void draw(GfxRenderer& renderer, const int fontId, const char* const labels[3], const int count,
                 const int active, const bool vertical, const int screenW, const int screenH,
                 const int firstTopY, const int lastBottomY, const int firstX, const int lastX,
                 const int lastW, Hits& hits) {
  if (count <= 0 || count > 3) return;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int chipH = lineHeight + 10;
  const int padX = 26;
  const int gap = 14;
  int widths[3] = {0, 0, 0};
  int totalW = gap * (count - 1);
  for (int i = 0; i < count; ++i) {
    widths[i] = renderer.getTextAdvanceX(fontId, labels[i], EpdFontFamily::BOLD) + padX * 2 + 10;
    totalW += widths[i];
  }
  const uint8_t radius = static_cast<uint8_t>(chipH / 2);
  const auto drawChip = [&](const int x, const int y, const int w, const char* label, const int idx) {
    renderer.fillRoundedRect(x, y, w, chipH, radius, Color::White);
    renderer.drawRoundedRect(x, y, w, chipH, idx == active ? 3 : 2, radius, true);
    const int textW = renderer.getTextAdvanceX(fontId, label, EpdFontFamily::BOLD);
    renderer.drawText(fontId, x + (w - textW) / 2, y + (chipH - lineHeight) / 2, label, true,
                      EpdFontFamily::BOLD);
    hits.x[idx] = static_cast<int16_t>(x);
    hits.y[idx] = static_cast<int16_t>(y);
    hits.w[idx] = static_cast<int16_t>(w);
    hits.h[idx] = static_cast<int16_t>(chipH);
  };
  const auto rowX = [&]() {
    int x = std::max<int>(6, (screenW - totalW) / 2);
    if (x + totalW > screenW - 6) x = std::max<int>(6, screenW - 6 - totalW);
    return x;
  };
  if (vertical) {
    int chipW = 0;
    for (int i = 0; i < count; ++i) chipW = std::max(chipW, widths[i]);
    const int stackH = chipH * count + 10 * (count - 1);
    const int selCenterY = (firstTopY + lastBottomY) / 2;
    const int maxStackY = screenH - stackH - 56;
    int bx = lastX + lastW + 10;
    if (bx + chipW > screenW - 6) {
      bx = firstX - chipW - 10;
      if (bx < 6) {
        int x = rowX();
        int y = selCenterY - chipH / 2;
        if (y > screenH - chipH - 56) y = screenH - chipH - 56;
        if (y < 8) y = 8;
        for (int i = 0; i < count; ++i) {
          drawChip(x, y, widths[i], labels[i], i);
          x += widths[i] + gap;
        }
        return;
      }
    }
    int by = selCenterY - stackH / 2;
    if (by > maxStackY) by = maxStackY;
    if (by < 8) by = 8;
    for (int i = 0; i < count; ++i) drawChip(bx, by + i * (chipH + 10), widths[i], labels[i], i);
    return;
  }
  int barY = lastBottomY + 10;
  if (barY + chipH > screenH - 8) barY = firstTopY - chipH - 10;
  if (barY < 8) barY = 8;
  int x = rowX();
  for (int i = 0; i < count; ++i) {
    drawChip(x, barY, widths[i], labels[i], i);
    x += widths[i] + gap;
  }
}

inline int hitTest(const Hits& hits, const int count, const int x, const int y) {
  for (int i = 0; i < count; ++i) {
    if (x >= hits.x[i] && x < hits.x[i] + hits.w[i] && y >= hits.y[i] && y < hits.y[i] + hits.h[i]) return i;
  }
  return -1;
}

}  // namespace selection_bar
