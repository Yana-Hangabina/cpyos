#pragma once

#include <GfxRenderer.h>

#include "SdCardFontSystem.h"
#include "fontIds.h"

// Chinese UI glyph-size policy for the high-density panel.
//
// The embedded faces fall into two groups:
//   14pt / 16pt  only the 803 characters used by the i18n strings (the size the
//                UI was designed around; too narrow for a book title)
//   12pt         GB2312, 6,879 glyphs (anything a user or the service supplies)
//
// "Comfortable" (the default) keeps the size-matched 14/16pt faces first and
// lets the wide 12pt face catch what they lack: a row made only of UI
// vocabulary keeps the large face, and a row containing a rarer character
// falls back as a whole (resolveTextFontId picks one face for the run).
// "Compact" puts every UI id on the 12pt face, which is both tighter and the
// honest choice for a dense layout.
//
// Driven by SETTINGS.uiDensity so there is one setting, not two.
inline void applyUiCjkFallbacks(GfxRenderer& renderer, const bool compact) {
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  // The narrow faces always keep the wide face behind them, in both modes.
  renderer.setFallbackFont(CJK_UI_14_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(CJK_UI_16_FONT_ID, CJK_UI_12_FONT_ID);
  if (compact) {
    renderer.setFallbackFont(UI_10_FONT_ID, CJK_UI_12_FONT_ID);
    renderer.setFallbackFont(UI_12_FONT_ID, CJK_UI_12_FONT_ID);
    renderer.setFallbackFont(SMALL_FONT_ID, CJK_UI_12_FONT_ID);
    renderer.setFallbackFont(CONTROL_18_FONT_ID, CJK_UI_12_FONT_ID);
    renderer.setFallbackFont(NOTOSANS_18_FONT_ID, CJK_UI_12_FONT_ID);
    renderer.setFallbackFont(READER_STATUS_FONT_ID, CJK_UI_12_FONT_ID);
    renderer.setFallbackFont(READER_ESTIMATE_FONT_ID, CJK_UI_12_FONT_ID);
    return;
  }
  renderer.setFallbackFont(UI_10_FONT_ID, CJK_UI_14_FONT_ID);
  renderer.setFallbackFont(UI_12_FONT_ID, CJK_UI_16_FONT_ID);
  renderer.setFallbackFont(SMALL_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(CONTROL_18_FONT_ID, CJK_UI_16_FONT_ID);
  renderer.setFallbackFont(NOTOSANS_18_FONT_ID, CJK_UI_16_FONT_ID);
  renderer.setFallbackFont(READER_STATUS_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(READER_ESTIMATE_FONT_ID, CJK_UI_14_FONT_ID);
#else
  // C3 and other non-high-DPI targets keep the upstream 8/10/12 chain.
  (void)renderer;
  (void)compact;
#endif
}
