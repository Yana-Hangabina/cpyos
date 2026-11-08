#include "Typography.h"

#include <GfxRenderer.h>
#include <SdCardFontSystem.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "SystemFontPack.h"
#include "fontIds.h"

namespace typography {

Points pointsFor(const Scale scale) {
  switch (scale) {
    case Scale::Compact:
      return {8, 10, 12, 14};
    case Scale::Standard:
      return {10, 12, 14, 16};
    case Scale::Large:
      return {12, 14, 16, 16};
    case Scale::ExtraLarge:
    case Scale::Count:
    default:
      return {14, 16, 16, 16};
  }
}

std::size_t uiRolePointsFor(const Points points, UiRolePoint* out, const std::size_t capacity) {
#if !defined(CROSSPOINT_FONT_PACK_EMBEDDED)
  // C3 streams a single reader face and keeps the built-in 8pt UI subset, so
  // there are no pack role bindings for a supplement to follow.
  (void)points;
  (void)out;
  (void)capacity;
  return 0;
#else
  if (out == nullptr) return 0;
  const UiRolePoint roles[] = {
      {SMALL_FONT_ID, points.caption},
      {UI_10_FONT_ID, points.body},
      {UI_12_FONT_ID, points.title},
      {CONTROL_18_FONT_ID, points.title},
      // Large UI values (stats, games, web cards) follow the display role. The
      // reader's 18pt selection uses CJK_UI_18 instead, so it stays 18pt.
      {NOTOSANS_18_FONT_ID, points.display},
      // Status/estimate are single-line chrome; the caption size is what their
      // fixed bands were designed around.
      {READER_STATUS_FONT_ID, points.caption},
      {READER_ESTIMATE_FONT_ID, points.caption},
  };
  const std::size_t count = std::min(capacity, std::size(roles));
  for (std::size_t i = 0; i < count; ++i) out[i] = roles[i];
  return count;
#endif
}

namespace {

int cjkIdFor(const uint8_t pointSize) {
  switch (pointSize) {
    case 8:
      return CJK_UI_8_FONT_ID;
    case 10:
      return CJK_UI_10_FONT_ID;
    case 12:
      return CJK_UI_12_FONT_ID;
    case 14:
      return CJK_UI_14_FONT_ID;
    default:
      return CJK_UI_16_FONT_ID;
  }
}

bool hasFace(const GfxRenderer& renderer, const int fontId) {
  return renderer.getFontMap().find(fontId) != renderer.getFontMap().end();
}

}  // namespace

void apply(GfxRenderer& renderer, const uint8_t scaleIndex) {
#if !defined(CROSSPOINT_SYSTEM_FONT_PACK)
  (void)renderer;
  (void)scaleIndex;
  return;
#else
  if (!system_font_pack::ready()) return;
  const Scale scale = scaleIndex < static_cast<uint8_t>(Scale::Count) ? static_cast<Scale>(scaleIndex) : Scale::Standard;
  const Points points = pointsFor(scale);

  // Load only the faces this target needs. A FreeType face keeps its own copy
  // of the font's cmap/loca/hmtx tables, so registering all six sizes at boot
  // is expensive on a PSRAM build and impossible on C3.
  uint8_t sizes[8];
  size_t sizeCount = 0;
  const auto wantSize = [&](const uint8_t pt) {
    // 8pt stays the built-in subset: it is the emergency fallback and must
    // never be replaced (a later scale change would otherwise leave no 8pt
    // family at all). Reader sizes above 18pt are loaded on demand too.
    if (pt < 10 || pt > 34) return;
    if (std::find(sizes, sizes + sizeCount, pt) != sizes + sizeCount) return;
    if (sizeCount < std::size(sizes)) sizes[sizeCount++] = pt;
  };
#if defined(CROSSPOINT_FONT_PACK_EMBEDDED)
  // Full vector UI (Read Pico): the four role sizes plus 18 for the screens
  // that ask NOTOSANS_18 directly, and the current reader size.
  wantSize(points.caption);
  wantSize(points.body);
  wantSize(points.title);
  wantSize(points.display);
  wantSize(SETTINGS.fontPointSize);
#else
  // C3 has no PSRAM: one reader face at a time. The system UI keeps the
  // built-in 8pt CJK subset (its strings never contain unexpected glyphs);
  // the pack covers book text and titles at the reader size.
  wantSize(SETTINGS.fontPointSize);
#endif
  system_font_pack::configure(renderer, sizes, sizeCount);

#if defined(CROSSPOINT_FONT_PACK_EMBEDDED)
  const auto bind = [&](const int uiId, const uint8_t pointSize) {
    const int cjkId = cjkIdFor(pointSize);
    if (hasFace(renderer, cjkId)) renderer.setPreferredFont(uiId, cjkId);
  };

  bind(SMALL_FONT_ID, points.caption);
  bind(UI_10_FONT_ID, points.body);
  bind(UI_12_FONT_ID, points.title);
  bind(CONTROL_18_FONT_ID, points.title);
  // Large UI values (stats, games, web cards) follow the display role. The
  // reader's 18pt selection uses CJK_UI_18 instead, so it stays 18pt.
  bind(NOTOSANS_18_FONT_ID, points.display);
  // Status/estimate are single-line chrome; the caption size is what their
  // fixed 48px bands were designed around. Using body/title here clipped the
  // status row on Read Pico's high-DPI layout.
  bind(READER_STATUS_FONT_ID, points.caption);
  bind(READER_ESTIMATE_FONT_ID, points.caption);
#endif
  // removeFont() cleared the fallbacks that pointed at the placeholder ids.
  // Re-point every role at the 8pt built-in emergency face so an unusual
  // codepoint still draws something instead of an empty family.
  const int emergency = CJK_UI_8_FONT_ID;
  for (const int id : {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID, CONTROL_18_FONT_ID, NOTOSANS_18_FONT_ID,
                       CJK_UI_18_FONT_ID, READER_STATUS_FONT_ID, READER_ESTIMATE_FONT_ID, NOTOSANS_12_FONT_ID,
                       NOTOSANS_14_FONT_ID, NOTOSANS_16_FONT_ID, NOTOSERIF_12_FONT_ID, NOTOSERIF_14_FONT_ID,
                       NOTOSERIF_16_FONT_ID}) {
    if (hasFace(renderer, id)) renderer.setFallbackFont(id, emergency);
  }
  for (uint8_t pt = 10; pt <= 34; ++pt) {
    const int id = CJK_READER_SIZE_FONT_ID(pt);
    if (hasFace(renderer, id)) renderer.setFallbackFont(id, emergency);
  }
#endif
#if CROSSPOINT_VECTOR_FONTS
  // Font Extras supplements follow the same scale as the roles above. Deriving
  // them here (not at each install site) is what keeps a rare-script glyph at
  // its row's size after the user changes uiFontScale.
  sdFontSystem.loadSupplementFonts(renderer, SETTINGS.fontPointSize);
#endif
}

}  // namespace typography
