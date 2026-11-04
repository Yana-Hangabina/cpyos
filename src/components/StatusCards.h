#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "I18n.h"
#include "components/UITheme.h"

// The system's two persistent surfaces above a page:
//
//   W (wait)  -- 12.5% scrim + a 560x328 card with the theme's duck/wave
//                progress bar, a title and up to two detail lines, plus an
//                optional Cancel pill. Used for every blocking "loading /
//                indexing / syncing / rebuilding" state, so the same object
//                means "busy" everywhere.
//   D (message) -- the same scrim + card shell without the progress bar, for a
//                message that stays until the user acts (errors that carry
//                Back/Retry hints rather than a timeout).
//
// Neither is a page: the caller keeps the level it was on. paint() draws into
// the current framebuffer and never presents -- hosts inside a render pass
// present once, after the overlay (and a 16-level host gets the card inside the
// same grayscale frame). show() paints and pushes the 1-bit frame, for callers
// that run outside a render pass or in the middle of one and are about to
// block: it must not commit the grayscale canvas, which a mid-pass host still
// needs for the page it is building.
namespace status_card {

struct Layout {
  Rect card{};
  Rect cancel{};
};

// `permille < 0` animates an indeterminate swim; `phaseMs` drives it (pass
// millis() - startedAt so successive frames move). `withCancel` draws the
// Cancel pill; the returned rect is empty otherwise.
inline Layout paintWait(const GfxRenderer& renderer, const char* title, const char* detail, const char* detail2,
                        const int permille, const bool withCancel, const uint32_t phaseMs) {
  auto& theme = UITheme::getInstance();
  const Rect safe = theme.getScreenSafeArea(renderer, true, true);
  GUI.drawModalScrim(renderer);

  constexpr int kCardW = 560;
  constexpr int kCardH = 328;
  constexpr int kPad = 32;
  const int cardW = std::min(kCardW, safe.width - 48);
  const int cardX = safe.x + (safe.width - cardW) / 2;
  const int cardY = safe.y + (safe.height - kCardH) / 2;
  GUI.drawCardFrame(renderer, Rect{cardX, cardY, cardW, kCardH}, false);

  // The theme's own wave + duck, the same primitive the reading and download
  // bars use -- not a second progress drawing.
  constexpr size_t kTotal = 1000;
  size_t current = 0;
  if (permille >= 0) {
    current = static_cast<size_t>(std::clamp(permille, 0, 1000));
  } else {
    constexpr uint32_t kSwimPeriodMs = 12000;
    const uint32_t swimHalf = kSwimPeriodMs / 2;
    const uint32_t phase = phaseMs % kSwimPeriodMs;
    const uint32_t tri = phase < swimHalf ? phase : (kSwimPeriodMs - phase);
    current = static_cast<size_t>(static_cast<uint64_t>(tri) * kTotal / swimHalf);
  }
  const Rect waveRect{cardX + kPad, cardY + 54, cardW - 2 * kPad, 12};
  GUI.drawProgressBar(renderer, waveRect, current, kTotal, false);

  const int lineWidth = cardW - 2 * kPad;
  int y = cardY + 104;
  const std::string shownTitle = renderer.truncatedText(UI_10_FONT_ID, title != nullptr ? title : "", lineWidth,
                                                       EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, cardX + kPad, y, shownTitle.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 6;
  const auto detailLine = [&](const char* text) {
    if (text == nullptr || text[0] == '\0') return;
    const std::string shown = renderer.truncatedText(SMALL_FONT_ID, text, lineWidth);
    renderer.drawText(SMALL_FONT_ID, cardX + kPad, y, shown.c_str(), true);
    y += renderer.getLineHeight(SMALL_FONT_ID) + 4;
  };
  detailLine(detail);
  detailLine(detail2);

  Layout out{};
  if (!withCancel) return out;
  constexpr int kPillW = 200;
  constexpr int kPillH = 56;
  const int pillX = cardX + (cardW - kPillW) / 2;
  const int pillY = cardY + kCardH - 28 - kPillH;
  renderer.fillRoundedRect(pillX, pillY, kPillW, kPillH, kPillH / 2, Color::Black);
  const char* cancel = tr(STR_CANCEL);
  const int cancelW = renderer.getTextWidth(UI_10_FONT_ID, cancel);
  renderer.drawText(UI_10_FONT_ID, pillX + (kPillW - cancelW) / 2,
                    pillY + (kPillH - renderer.getLineHeight(UI_10_FONT_ID)) / 2, cancel, false);
  out.cancel = Rect{pillX, pillY, kPillW, kPillH};
  out.card = Rect{cardX, cardY, cardW, kCardH};
  return out;
}

inline Layout showWait(const GfxRenderer& renderer, const char* title, const char* detail, const char* detail2,
                       const int permille, const bool withCancel, const uint32_t phaseMs) {
  const Layout layout = paintWait(renderer, title, detail, detail2, permille, withCancel, phaseMs);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
  return layout;
}

// A message that stays until the user acts (the button hints below the card
// are the actions). Wrapping goes through the same font the card body uses.
inline void paintMessage(const GfxRenderer& renderer, const char* title, const char* body) {
  auto& theme = UITheme::getInstance();
  const Rect safe = theme.getScreenSafeArea(renderer, true, true);
  GUI.drawModalScrim(renderer);

  constexpr int kPad = 32;
  constexpr int kMaxCardW = 560;
  const int cardW = std::min(kMaxCardW, safe.width - 48);
  const int innerW = cardW - 2 * kPad;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const bool hasBody = body != nullptr && body[0] != '\0';
  const auto lines = hasBody ? renderer.wrappedText(SMALL_FONT_ID, body, innerW, 6, EpdFontFamily::REGULAR)
                             : std::vector<std::string>{};
  int contentH = lineH + (hasBody ? static_cast<int>(lines.size()) * smallH + 10 : 0);
  const int cardH = contentH + kPad * 2;
  const int cardX = safe.x + (safe.width - cardW) / 2;
  const int cardY = safe.y + (safe.height - cardH) / 2;
  GUI.drawCardFrame(renderer, Rect{cardX, cardY, cardW, cardH}, false);

  int y = cardY + kPad;
  const std::string shownTitle =
      renderer.truncatedText(UI_10_FONT_ID, title != nullptr ? title : "", innerW, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, cardX + kPad, y, shownTitle.c_str(), true, EpdFontFamily::BOLD);
  y += lineH + 10;
  for (const auto& line : lines) {
    renderer.drawText(SMALL_FONT_ID, cardX + kPad, y, line.c_str(), true, EpdFontFamily::REGULAR);
    y += smallH;
  }
}

inline void showMessage(const GfxRenderer& renderer, const char* title, const char* body) {
  paintMessage(renderer, title, body);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
}

}  // namespace status_card
