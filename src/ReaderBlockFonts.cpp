#include "ReaderBlockFonts.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "CrossPointSettings.h"
#include "GfxRenderer.h"
#include "ReaderFontSizes.h"
#include "SystemFontPack.h"
#include "fontIds.h"

// Extra faces the user can end up paying for in one reading session. A book using
// h1 + h2 + `.title` needs three distinct sizes in the worst case; anything past
// that keeps the section font instead of growing the face set without bound.
static constexpr uint8_t kMaxBlockPointSizes = 3;

ReaderBlockFonts::ReaderBlockFonts(GfxRenderer& renderer) : renderer_(renderer) {
#if defined(CROSSPOINT_SYSTEM_FONT_PACK) && defined(BOARD_HAS_PSRAM)
  if (!system_font_pack::ready()) {
    LOG_INF("RBF", "block font sizes off: font pack not ready");
    return;
  }
  // Only the pack registers one face per point size. An SD family loads a single
  // reader face and an EPUB-embedded font is one transient face, so neither can
  // host a second size without a variant registry (deliberately not built: see the
  // layout M2 design, section 3.4).
  if (!system_font_pack::ownsFontId(SETTINGS.getReaderFontId())) {
    LOG_INF("RBF", "block font sizes off: %s",
            SETTINGS.readerFontOverrideId != 0 ? "embedded book font" : "SD font family");
    return;
  }
  basePointSize_ = snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                          SETTINGS.fontPointSize);
  enabled_ = true;
  LOG_INF("RBF", "block font sizes on: base %upt, up to %u extra faces", static_cast<unsigned>(basePointSize_),
          static_cast<unsigned>(kMaxBlockPointSizes));
#else
  (void)renderer;
#endif
}

int ReaderBlockFonts::resolveBlockFont(const float scale) {
  if (!enabled_) return 0;
  return resolveSystemPack(scale);
}

int ReaderBlockFonts::resolveSystemPack(const float scale) {
  const float clamped = std::clamp(scale, 0.5f, 3.0f);
  const int scaled = static_cast<int>(std::lround(clamped * static_cast<float>(basePointSize_)));
  const uint8_t pointSize = static_cast<uint8_t>(std::clamp(scaled, 10, 34));
  // Snapping back to the reader size means the block keeps the section font; no
  // extra face and no per-line font id (0 is the on-disk spelling of that).
  if (pointSize == basePointSize_) return 0;

  if (std::find(extraPointSizes_, extraPointSizes_ + extraCount_, pointSize) != extraPointSizes_ + extraCount_) {
    return CJK_READER_SIZE_FONT_ID(pointSize);
  }
  if (extraCount_ >= kMaxBlockPointSizes) {
    logOnce(loggedUnsupported_, "block font sizes capped; remaining blocks use the reader face");
    return 0;
  }
  if (!system_font_pack::ensurePointSize(renderer_, pointSize)) {
    // Heap pressure or a pack that cannot build another face: keep the section
    // font rather than lay the block out at a size nothing can draw.
    logOnce(loggedUnsupported_, "block font size unavailable; using the reader face");
    return 0;
  }
  extraPointSizes_[extraCount_++] = pointSize;
  LOG_INF("RBF", "block font face %upt (%u/%u)", static_cast<unsigned>(pointSize), static_cast<unsigned>(extraCount_),
          static_cast<unsigned>(kMaxBlockPointSizes));
  return CJK_READER_SIZE_FONT_ID(pointSize);
}

void ReaderBlockFonts::logOnce(const char*& flag, const char* message) {
  if (flag != nullptr) return;
  flag = message;
  LOG_INF("RBF", "%s", message);
}
