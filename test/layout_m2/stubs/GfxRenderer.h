#pragma once

// Font-aware GfxRenderer test double for the layout M2 host test.
//
// The section font (any id below kStubBlockFontBase) has an 8 px advance, 16 px
// line height and 12 px ascender; ids at or above kStubBlockFontBase are the
// "scaled block faces" and are 50% larger, matching the 1.5x/2x CSS sizes the test
// feeds in. That lets the test tell from page geometry alone whether a block was
// measured with the face it selected.

#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <Utf8.h>

#include <deque>
#include <map>
#include <string>

namespace layout_m2_stub {
inline constexpr int kBlockFontBase = 3000;  // ids 3000+ are scaled faces
}

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  FontCacheManager fontCache;
  FontCacheManager* getFontCacheManager() { return &fontCache; }
  // The fixture has no framebuffer to lend; image probes run as without a loan.
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer&) {}
    void end() {}
  };
  enum class TextMeasureMode { Layout, Rendered };
  static int trackingBetween(uint32_t left, uint32_t right, int8_t tracking) {
    return left == 0 || left == ' ' || right == ' ' ? 0 : tracking;
  }
  // PageLine::render() asks whether a block's serialised font id is live. The test
  // never renders lines, but the symbol has to exist for Page.cpp.
  const std::map<int, EpdFontFamily>& getFontMap() const {
    static const std::map<int, EpdFontFamily> empty;
    return empty;
  }
  bool isFontCacheScanning() const { return false; }
  void drawLine(int, int, int, int, int, bool) const {}
  void fillRect(int, int, int, int, bool) const {}
  void fillRectShade(int, int, int, int, uint8_t) const {}
  bool drawScaledCodepoint(const EpdFontFamily&, uint32_t, int, int, int, EpdFontFamily::Style = EpdFontFamily::REGULAR,
                           bool = true) const {
    return false;
  }
  bool drawScaledCodepointTop(const EpdFontFamily&, uint32_t, int, int, int,
                              EpdFontFamily::Style = EpdFontFamily::REGULAR, bool = true) const {
    return false;
  }
  // A drop cap's ink box is one glyph: the stub's advance wide and its line height tall,
  // scaled like the real primitive so layout can derive the number of lines it spans.
  bool getScaledCodepointMetrics(int fontId, uint32_t codepoint, int scale256, EpdFontFamily::Style,
                                 int& width, int& height) const {
    if (codepoint == 0) return false;
    const int glyph = fontId >= layout_m2_stub::kBlockFontBase ? 12 : 8;
    const int line = fontId >= layout_m2_stub::kBlockFontBase ? 24 : 16;
    width = glyph * scale256 / 256;
    height = (line * 3 / 4) * scale256 / 256;
    return width > 0 && height > 0;
  }
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {}
  int getTextWidth(int font, const char* text, EpdFontFamily::Style style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    return getTextAdvanceX(font, text, style);
  }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int fontId, float = 1.0f) const {
    return fontId >= layout_m2_stub::kBlockFontBase ? 24 : 16;
  }
  int getFontAscenderSize(int fontId) const { return fontId >= layout_m2_stub::kBlockFontBase ? 18 : 12; }
  int getSpaceWidth(int fontId, EpdFontFamily::Style) const {
    return fontId >= layout_m2_stub::kBlockFontBase ? 6 : 4;
  }
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode = TextMeasureMode::Layout) const {
    const int glyph = fontId >= layout_m2_stub::kBlockFontBase ? 12 : 8;
    int width = 0;
    uint32_t previous = 0;
    while (const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      if (utf8IsCombiningMark(cp)) continue;
      width += glyph + trackingBetween(previous, cp, tracking);
      previous = cp;
    }
    return width;
  }
  int getKerning(int, uint32_t left, uint32_t right, EpdFontFamily::Style, int8_t tracking = 0) const {
    return trackingBetween(left, right, tracking);
  }
  int getSpaceAdvance(int fontId, uint32_t, uint32_t, EpdFontFamily::Style) const {
    return fontId >= layout_m2_stub::kBlockFontBase ? 6 : 4;
  }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const char* const*, const size_t*, size_t, bool, bool, uint8_t) const {}
};
