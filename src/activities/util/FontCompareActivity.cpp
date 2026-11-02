#include "FontCompareActivity.h"

#if defined(CROSSPOINT_SYSTEM_FONT_PACK)

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "SystemFontPack.h"
#include "fontIds.h"

namespace {
// Same sample at every row, so the only variables are coverage depth and the
// B/W threshold. The Latin line is a stem-spacing canary.
constexpr char kChineseSample[] = "横竖撇捺 永字八法";
constexpr char kLatinSample[] = "Hamburgefonstiv 0123";

// Old 4-level reader path: 0 = white, 3 = black. The exact panel levels depend
// on the waveform, but these are the four equally spaced levels the LSB/MSB
// plane pair represents.
constexpr uint8_t kLegacyLevels[4] = {255, 170, 85, 0};
}  // namespace

void FontCompareActivity::onEnter() {
  Activity::onEnter();
  pointSize_ = static_cast<uint8_t>(requestedPointSize_ > 0 ? requestedPointSize_ : SETTINGS.fontPointSize);
  if (pointSize_ < 10) pointSize_ = 10;
  if (pointSize_ > 34) pointSize_ = 34;

  {
    RenderLock lock;
    preview2BitId_ = system_font_pack::registerPreviewFace(renderer, pointSize_, /*fourBit=*/false);
    preview4BitId_ = system_font_pack::registerPreviewFace(renderer, pointSize_, /*fourBit=*/true);
  }
  ready_ = preview2BitId_ != 0 && preview4BitId_ != 0;
  LOG_INF("FONTDEMO", "compare @%upt 2-bit id=%d 4-bit id=%d ready=%d", static_cast<unsigned>(pointSize_),
          preview2BitId_, preview4BitId_, ready_ ? 1 : 0);
  requestUpdate();
}

void FontCompareActivity::loop() {
  // This is a serial-pushed diagnostic page, not a Home activity, so the
  // ActivityManager will not pop it for us. Without this hook Back appeared
  // dead and the only escape was CMD:HOME.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
}

void FontCompareActivity::onExit() {
  // exitActivity() calls this with the render lock already held, so do not take
  // another one here.
  system_font_pack::releasePreviewFaces(renderer);
  preview2BitId_ = 0;
  preview4BitId_ = 0;
  ready_ = false;
  Activity::onExit();
}

void FontCompareActivity::drawLegacyFourLevel(const int fontId, const int x, const int yTop,
                                               const char* text) const {
  const auto it = renderer.getFontMap().find(fontId);
  if (it == renderer.getFontMap().end() || text == nullptr) return;
  const EpdFontFamily& family = it->second;
  const EpdFontData* data = family.getData(EpdFontFamily::REGULAR);
  if (data == nullptr) return;
  const int baseline = yTop + data->ascender;
  int pen = x;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor != '\0') {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    const EpdGlyph* glyph = family.getGlyph(cp, EpdFontFamily::REGULAR);
    if (glyph == nullptr) continue;
    const uint8_t* bitmap = renderer.getGlyphBitmap(data, glyph);
    if (bitmap != nullptr) {
      const int gx0 = pen + glyph->left;
      const int gy0 = baseline - glyph->top;
      for (int gy = 0; gy < glyph->height; ++gy) {
        for (int gx = 0; gx < glyph->width; ++gx) {
          const int pos = gy * glyph->width + gx;
          const uint8_t byte = bitmap[pos >> 2];
          const uint8_t coverage = (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
          if (coverage != 0) renderer.drawGrayscale16Pixel(gx0 + gx, gy0 + gy, kLegacyLevels[coverage]);
        }
      }
    }
    pen += (glyph->advanceX + 8) >> 4;
  }
}

void FontCompareActivity::drawPage(const bool grayscale) const {
  const int width = renderer.getScreenWidth();
  const int margin = 28;
  const int right = width - margin;
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
  const int sampleH = renderer.getLineHeight(preview4BitId_);
  int y = 22;

  renderer.drawText(UI_12_FONT_ID, margin, y, "FONT RENDER COMPARE", true, EpdFontFamily::REGULAR);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 2;
  char subtitle[96];
  snprintf(subtitle, sizeof(subtitle), "%upt / Regular / same face; old rows are simulated", static_cast<unsigned>(pointSize_));
  renderer.drawText(SMALL_FONT_ID, margin, y, subtitle, true, EpdFontFamily::REGULAR);
  y += labelH + 8;
  renderer.drawLine(margin, y, right, y, true);
  y += 12;

  const auto drawRowLabel = [&](const char* label) {
    renderer.drawText(SMALL_FONT_ID, margin, y, label, true, EpdFontFamily::REGULAR);
    y += labelH + 2;
  };

  // 1) Old B/W: 2-bit thresholds, 25% is ink.
  drawRowLabel("OLD: 2-bit -> 1-bit (25% threshold)");
  GfxRenderer::setBwGlyphThreshold(1);
  renderer.drawText(preview2BitId_, margin, y, kChineseSample, true, EpdFontFamily::REGULAR);
  y += sampleH;
  renderer.drawText(preview2BitId_, margin, y, kLatinSample, true, EpdFontFamily::REGULAR);
  GfxRenderer::setBwGlyphThreshold(2);
  y += sampleH + 6;
  renderer.drawLine(margin, y, right, y, true);
  y += 10;

  // 2) New B/W: same 2-bit face, 50% threshold. This is the fast-page-turn and
  // settings/list path after the change.
  drawRowLabel("NEW: 1-bit UI (50% threshold)");
  renderer.drawText(preview2BitId_, margin, y, kChineseSample, true, EpdFontFamily::REGULAR);
  y += sampleH;
  renderer.drawText(preview2BitId_, margin, y, kLatinSample, true, EpdFontFamily::REGULAR);
  y += sampleH + 6;
  renderer.drawLine(margin, y, right, y, true);
  y += 10;

  // 3) Old reader: 2-bit -> LSB/MSB pair = four levels. Drawn by hand because
  // the current canvas is 16-level and the old pair cannot be selected per row.
  drawRowLabel("OLD: 2-bit -> 4 levels (reader)");
  if (grayscale) {
    drawLegacyFourLevel(preview2BitId_, margin, y, kChineseSample);
  } else {
    renderer.drawText(preview2BitId_, margin, y, kChineseSample, true, EpdFontFamily::REGULAR);
  }
  y += sampleH;
  if (grayscale) {
    drawLegacyFourLevel(preview2BitId_, margin, y, kLatinSample);
  } else {
    renderer.drawText(preview2BitId_, margin, y, kLatinSample, true, EpdFontFamily::REGULAR);
  }
  y += sampleH + 10;
  renderer.drawLine(margin, y, right, y, true);
  y += 12;

  // 4) New: 4-bit -> 16 levels. This is the code path the change ships.
  drawRowLabel("NEW: 4-bit -> 16 levels (current)");
  if (grayscale) {
    renderer.drawText(preview4BitId_, margin, y, kChineseSample, true, EpdFontFamily::REGULAR);
  } else {
    renderer.drawText(preview4BitId_, margin, y, kChineseSample, true, EpdFontFamily::REGULAR);
  }
  y += sampleH;
  renderer.drawText(preview4BitId_, margin, y, kLatinSample, true, EpdFontFamily::REGULAR);
  y += sampleH + 14;

  renderer.drawLine(margin, y, right, y, true);
  y += 12;
  renderer.drawText(SMALL_FONT_ID, margin, y, "UI 1-bit pages: 50%. Reader quick turn: 25% weight.", true,
                    EpdFontFamily::REGULAR);
  y += labelH + 2;
  renderer.drawText(SMALL_FONT_ID, margin, y, "16-level text needs fast page turn off.", true, EpdFontFamily::REGULAR);
  y += labelH + 2;
  renderer.drawText(SMALL_FONT_ID, margin, y, "Back to exit / CMD:FONTDEMO [pt]", true, EpdFontFamily::REGULAR);
}

void FontCompareActivity::render(RenderLock&&) {
  if (!ready_) {
    renderer.clearScreen();
    renderer.drawText(UI_12_FONT_ID, 28, 28, "FONT COMPARE unavailable", true, EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, 28, 70, "system font pack is not ready", true, EpdFontFamily::REGULAR);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  if (renderer.beginGrayscale16()) {
    drawPage(/*grayscale=*/true);
    if (renderer.commitGrayscale16(/*clean=*/false)) return;
    renderer.cancelGrayscale16();
    LOG_ERR("FONTDEMO", "16-level commit failed; falling back to 1-bit");
  }

  renderer.clearScreen();
  drawPage(/*grayscale=*/false);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

#endif  // CROSSPOINT_SYSTEM_FONT_PACK
