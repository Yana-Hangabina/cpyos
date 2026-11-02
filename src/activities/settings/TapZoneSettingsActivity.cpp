#include "TapZoneSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Every action the editor offers, in menu order. PREV/NEXT stay in the list
// even when the matching gesture is Swipe Only: the reader gates them, and
// hiding them would make the stored index ambiguous while editing.
constexpr tapzones::Action kShortActions[] = {
    tapzones::NONE,          tapzones::PREV,        tapzones::NEXT,
    tapzones::MENU,          tapzones::BOOKMARK,    tapzones::DICTIONARY,
    tapzones::CHAPTER,       tapzones::FOOTNOTE,    tapzones::JUMP_PERCENT,
    tapzones::KOREADER,      tapzones::AUTO_TURN,   tapzones::GO_HOME,
    tapzones::REFRESH,       tapzones::ROTATE_CW,   tapzones::ROTATE_CCW,
};

constexpr tapzones::LongAction kLongActions[] = {
    tapzones::LONG_NONE,    tapzones::LONG_BOOKMARK,   tapzones::LONG_DICTIONARY,
    tapzones::LONG_CHAPTER, tapzones::LONG_MENU,       tapzones::LONG_FOOTNOTE,
    tapzones::LONG_JUMP_PERCENT, tapzones::LONG_KOREADER, tapzones::LONG_AUTO_TURN,
    tapzones::LONG_GO_HOME, tapzones::LONG_REFRESH,    tapzones::LONG_ROTATE_CW,
    tapzones::LONG_ROTATE_CCW,
};

constexpr int kPillHeight = 48;
constexpr int kResetHeight = 48;
constexpr int kBlockGap = 12;

bool contains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

} // namespace

void TapZoneSettingsActivity::onEnter() {
  Activity::onEnter();
  if (SETTINGS.tapZoneMode >= CrossPointSettings::TAP_ZONE_MODE_COUNT) {
    SETTINGS.tapZoneMode = CrossPointSettings::TAP_ZONE_MODE_LEGACY;
  }
  selected_ = 4;
  longLayer_ = false;
  popupZone_ = selected_;
  requestUpdate();
}

void TapZoneSettingsActivity::onExit() {
  // Persist no matter how the page closes (Back key, system gesture, sleep),
  // so a power cycle never loses the pick.
  SETTINGS.saveToFile();
  Activity::onExit();
}

TapZoneSettingsActivity::Layout TapZoneSettingsActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int pad = metrics.contentSidePadding;
  const int x = safe.x + pad;
  const int width = safe.width - pad * 2;

  Layout l;
  l.header = Rect{safe.x, static_cast<int16_t>(safe.y + metrics.topPadding),
                  static_cast<int16_t>(safe.width), static_cast<int16_t>(metrics.headerHeight)};

  int y = safe.y + metrics.topPadding + metrics.headerHeight + kBlockGap;
  const int half = (width - kBlockGap) / 2;
  l.legacyPill = Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(half),
                      static_cast<int16_t>(kPillHeight)};
  l.customPill = Rect{static_cast<int16_t>(x + half + kBlockGap), static_cast<int16_t>(y),
                      static_cast<int16_t>(half), static_cast<int16_t>(kPillHeight)};
  y += kPillHeight + kBlockGap;
  l.shortPill = Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(half),
                     static_cast<int16_t>(kPillHeight)};
  l.longPill = Rect{static_cast<int16_t>(x + half + kBlockGap), static_cast<int16_t>(y),
                    static_cast<int16_t>(half), static_cast<int16_t>(kPillHeight)};

  const int hintH = metrics.buttonHintsHeight;
  const int bottom = safe.y + safe.height - hintH - kBlockGap;
  const int resetWidth = std::min(width, 320);
  l.resetButton = Rect{static_cast<int16_t>(x), static_cast<int16_t>(bottom - kResetHeight),
                       static_cast<int16_t>(resetWidth), static_cast<int16_t>(kResetHeight)};

  const int gridTop = y + kPillHeight + kBlockGap + renderer.getLineHeight(UI_10_FONT_ID) + kBlockGap;
  const int gridBottom = bottom - kResetHeight - kBlockGap;
  l.gridArea = Rect{static_cast<int16_t>(x), static_cast<int16_t>(gridTop), static_cast<int16_t>(width),
                    static_cast<int16_t>(std::max(60, gridBottom - gridTop))};
  return l;
}

void TapZoneSettingsActivity::loop() {
  if (popup_.isActive()) {
    popup_.handleInput(mappedInput, [this] { requestUpdate(); });
    if (!popup_.isActive()) ignoreTapUntilMs_ = millis() + 300;
    return;
  }

  const Layout l = layout();
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (static_cast<int32_t>(millis() - ignoreTapUntilMs_) < 0) return;
    if (contains(l.legacyPill, x, y)) {
      setMode(CrossPointSettings::TAP_ZONE_MODE_LEGACY);
      return;
    }
    if (contains(l.customPill, x, y)) {
      setMode(CrossPointSettings::TAP_ZONE_MODE_CUSTOM);
      return;
    }
    if (contains(l.shortPill, x, y)) {
      longLayer_ = false;
      requestUpdate();
      return;
    }
    if (contains(l.longPill, x, y)) {
      longLayer_ = true;
      requestUpdate();
      return;
    }
    if (contains(l.resetButton, x, y)) {
      resetZones();
      return;
    }
    const tapzones::Grid grid(l.gridArea.x, l.gridArea.y, l.gridArea.width, l.gridArea.height);
    const int zone = grid.zoneAt(x, y);
    if (zone >= 0) {
      selected_ = static_cast<uint8_t>(zone);
      openPopup(selected_);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openPopup(selected_);
    return;
  }

  int dx = 0;
  int dy = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    dx = -1;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    dx = 1;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    dy = -1;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    dy = 1;
  }
  if (dx == 0 && dy == 0) return;
  // The cursor walks the 3x3 main grid; the six small zones are touch-only
  // (they are too small for a directional walk to be useful).
  int row = std::min<int>(selected_, 8) / 3;
  int col = std::min<int>(selected_, 8) % 3;
  row = (row + dy + 3) % 3;
  col = (col + dx + 3) % 3;
  selected_ = static_cast<uint8_t>(row * 3 + col);
  requestUpdate();
}

void TapZoneSettingsActivity::setMode(const uint8_t mode) {
  if (SETTINGS.tapZoneMode == mode) return;
  if (mode == CrossPointSettings::TAP_ZONE_MODE_CUSTOM && !SETTINGS.tapZonesCustomized) {
    // First switch: start from the stock layout the reader has been using, with
    // page-turn cells dropped for a direction that no longer accepts taps.
    tapzones::seedZones(SETTINGS.tapZones, ReaderUtils::allowsTap(SETTINGS.previousPageGesture),
                        ReaderUtils::allowsTap(SETTINGS.pageTurnGesture),
                        SETTINGS.showReaderMenu == CrossPointSettings::READER_MENU_TAP);
    SETTINGS.tapZonesCustomized = 1;
  }
  SETTINGS.tapZoneMode = mode;
  SETTINGS.saveToFile();
  requestUpdate();
}

void TapZoneSettingsActivity::resetZones() {
  tapzones::seedZones(SETTINGS.tapZones, ReaderUtils::allowsTap(SETTINGS.previousPageGesture),
                      ReaderUtils::allowsTap(SETTINGS.pageTurnGesture),
                      SETTINGS.showReaderMenu == CrossPointSettings::READER_MENU_TAP);
  for (int i = 0; i < 9; ++i) SETTINGS.tapZonesLong[i] = tapzones::LONG_NONE;
  for (int i = 0; i < 6; ++i) SETTINGS.miniZones[i] = tapzones::NONE;
  SETTINGS.tapZonesCustomized = 1;
  SETTINGS.saveToFile();
  requestUpdate();
}

StrId TapZoneSettingsActivity::shortActionNameId(const uint8_t action) const {
  switch (action) {
    case tapzones::PREV:
      return StrId::STR_PREV_PAGE;
    case tapzones::NEXT:
      return StrId::STR_NEXT_PAGE;
    case tapzones::MENU:
      return StrId::STR_READER_MENU;
    case tapzones::BOOKMARK:
      return StrId::STR_BOOKMARK_OPTION;
    case tapzones::DICTIONARY:
      return StrId::STR_DICTIONARY;
    case tapzones::CHAPTER:
      return StrId::STR_CHAPTER;
    case tapzones::FOOTNOTE:
      return StrId::STR_FOOTNOTES;
    case tapzones::JUMP_PERCENT:
      return StrId::STR_GO_TO_PERCENT;
    case tapzones::KOREADER:
      return StrId::STR_KOREADER_SYNC;
    case tapzones::AUTO_TURN:
      return StrId::STR_TAP_ZONE_AUTO_TURN;
    case tapzones::GO_HOME:
      return StrId::STR_GO_HOME_BUTTON;
    case tapzones::REFRESH:
      return StrId::STR_FORCE_REFRESH;
    case tapzones::ROTATE_CW:
      return StrId::STR_ROTATE_CW;
    case tapzones::ROTATE_CCW:
      return StrId::STR_ROTATE_CCW;
    case tapzones::NONE:
    default:
      return StrId::STR_NONE_OPT;
  }
}

StrId TapZoneSettingsActivity::longActionNameId(const uint8_t action) const {
  switch (action) {
    case tapzones::LONG_BOOKMARK:
      return StrId::STR_BOOKMARK_OPTION;
    case tapzones::LONG_DICTIONARY:
      return StrId::STR_DICTIONARY;
    case tapzones::LONG_CHAPTER:
      return StrId::STR_CHAPTER;
    case tapzones::LONG_MENU:
      return StrId::STR_READER_MENU;
    case tapzones::LONG_FOOTNOTE:
      return StrId::STR_FOOTNOTES;
    case tapzones::LONG_JUMP_PERCENT:
      return StrId::STR_GO_TO_PERCENT;
    case tapzones::LONG_KOREADER:
      return StrId::STR_KOREADER_SYNC;
    case tapzones::LONG_AUTO_TURN:
      return StrId::STR_TAP_ZONE_AUTO_TURN;
    case tapzones::LONG_GO_HOME:
      return StrId::STR_GO_HOME_BUTTON;
    case tapzones::LONG_REFRESH:
      return StrId::STR_FORCE_REFRESH;
    case tapzones::LONG_ROTATE_CW:
      return StrId::STR_ROTATE_CW;
    case tapzones::LONG_ROTATE_CCW:
      return StrId::STR_ROTATE_CCW;
    case tapzones::LONG_NONE:
    default:
      return StrId::STR_NONE_OPT;
  }
}

const char* TapZoneSettingsActivity::shortActionLabel(const uint8_t action) const {
  return I18N.get(shortActionNameId(action));
}

const char* TapZoneSettingsActivity::longActionLabel(const uint8_t action) const {
  return I18N.get(longActionNameId(action));
}

void TapZoneSettingsActivity::openPopup(const uint8_t zone) {
  // Editing a cell only makes sense on the custom grid; a tap/Confirm while the
  // legacy layout is selected switches over first (seeding it) so the pick the
  // user makes is actually live.
  if (SETTINGS.tapZoneMode != CrossPointSettings::TAP_ZONE_MODE_CUSTOM) {
    setMode(CrossPointSettings::TAP_ZONE_MODE_CUSTOM);
  }
  popupZone_ = zone;
  rebuildPopupOptions();
  const bool mini = zone >= 9;
  snprintf(popupTitle_, sizeof(popupTitle_), "%s %u %s", tr(STR_TAP_ZONE), static_cast<unsigned>(zone + 1),
           mini ? tr(STR_TAP_ZONE_SHORT) : (longLayer_ ? tr(STR_TAP_ZONE_LONG) : tr(STR_TAP_ZONE_SHORT)));
  const int current = [&] {
    if (mini) {
      const uint8_t raw = tapzones::rawShortAction(zone, SETTINGS.tapZones, SETTINGS.miniZones);
      for (size_t i = 0; i < std::size(kShortActions); ++i) {
        if (kShortActions[i] == raw) return static_cast<int>(i);
      }
      return 0;
    }
    if (longLayer_) {
      const uint8_t raw = tapzones::clampLongAction(SETTINGS.tapZonesLong[zone]);
      for (size_t i = 0; i < std::size(kLongActions); ++i) {
        if (kLongActions[i] == raw) return static_cast<int>(i);
      }
      return 0;
    }
    const uint8_t raw = tapzones::clampAction(SETTINGS.tapZones[zone]);
    for (size_t i = 0; i < std::size(kShortActions); ++i) {
      if (kShortActions[i] == raw) return static_cast<int>(i);
    }
    return 0;
  }();
  popup_.show(popupTitle_, popupOptions_.data(), static_cast<int>(popupOptions_.size()), current,
              [this](const int index) { applyAction(index); });
  requestUpdate();
}

void TapZoneSettingsActivity::rebuildPopupOptions() {
  popupOptions_.clear();
  const bool longLayer = longLayer_ && popupZone_ < 9;
  if (longLayer) {
    popupOptions_.reserve(std::size(kLongActions));
    for (const auto action : kLongActions) popupOptions_.push_back(longActionLabel(action));
  } else {
    popupOptions_.reserve(std::size(kShortActions));
    for (const auto action : kShortActions) popupOptions_.push_back(shortActionLabel(action));
  }
}

void TapZoneSettingsActivity::applyAction(const int index) {
  if (popupZone_ >= 9) {
    // Small zones are tap-only.
    if (index < 0 || static_cast<size_t>(index) >= std::size(kShortActions)) return;
    SETTINGS.miniZones[popupZone_ - 9] = kShortActions[index];
  } else if (longLayer_) {
    if (index < 0 || static_cast<size_t>(index) >= std::size(kLongActions)) return;
    SETTINGS.tapZonesLong[popupZone_] = kLongActions[index];
  } else {
    if (index < 0 || static_cast<size_t>(index) >= std::size(kShortActions)) return;
    SETTINGS.tapZones[popupZone_] = kShortActions[index];
  }
  SETTINGS.tapZonesCustomized = 1;
  SETTINGS.saveToFile();
  requestUpdate();
}

void TapZoneSettingsActivity::drawPill(const Rect& rect, const char* label, const bool selected) const {
  const int radius = std::min(16, std::min(rect.width, rect.height) / 2);
  if (selected) {
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, radius, Color::Black);
  } else {
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, radius, true);
  }
  const int font = UI_12_FONT_ID;
  const std::string text = renderer.truncatedText(font, label, rect.width - 16);
  const int textWidth = renderer.getTextWidth(font, text.c_str());
  const int textHeight = renderer.getTextHeight(font);
  renderer.drawText(font, rect.x + (rect.width - textWidth) / 2, rect.y + (rect.height - textHeight) / 2,
                    text.c_str(), !selected);
}

void TapZoneSettingsActivity::drawZone(const Rect& rect, const uint8_t action, const bool selected,
                                       const bool mini, const bool longLabel) const {
  const int radius = std::min(12, std::min(rect.width, rect.height) / 2);
  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, selected ? 3 : 1, radius, true);
  const int font = mini ? UI_10_FONT_ID : UI_12_FONT_ID;
  const char* label = action == 0 ? "-" : (longLabel ? longActionLabel(action) : shortActionLabel(action));
  const std::string text = renderer.truncatedText(font, label, std::max(8, rect.width - 8));
  const int textWidth = renderer.getTextWidth(font, text.c_str());
  const int textHeight = renderer.getTextHeight(font);
  renderer.drawText(font, rect.x + (rect.width - textWidth) / 2, rect.y + (rect.height - textHeight) / 2,
                    text.c_str(), true);
}

void TapZoneSettingsActivity::render(RenderLock&&) {
  const Layout l = layout();
  const bool custom = SETTINGS.tapZoneMode == CrossPointSettings::TAP_ZONE_MODE_CUSTOM;

  renderer.clearScreen();
  GUI.drawHeader(renderer, l.header, tr(STR_TAP_ZONES));

  drawPill(l.legacyPill, tr(STR_TAP_ZONE_LEGACY), !custom);
  drawPill(l.customPill, tr(STR_TAP_ZONE_CUSTOM), custom);
  drawPill(l.shortPill, tr(STR_TAP_ZONE_SHORT), !longLayer_);
  drawPill(l.longPill, tr(STR_TAP_ZONE_LONG), longLayer_);

  const int hintY = l.shortPill.y + kPillHeight + kBlockGap;
  const char* hint = custom ? tr(STR_TAP_ZONE_HINT) : tr(STR_TAP_ZONE_LEGACY_NOTE);
  const std::string hintText = renderer.truncatedText(UI_10_FONT_ID, hint, l.gridArea.width);
  renderer.drawText(UI_10_FONT_ID, l.gridArea.x, hintY, hintText.c_str());

  const tapzones::Grid grid(l.gridArea.x, l.gridArea.y, l.gridArea.width, l.gridArea.height);
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      const uint8_t zone = static_cast<uint8_t>(row * 3 + col);
      const uint8_t action = custom ? (longLayer_ ? SETTINGS.tapZonesLong[zone] : SETTINGS.tapZones[zone])
                                    : tapzones::kDefaultZones[zone];
      const tapzones::Rect c = grid.cell(row, col);
      drawZone(Rect{c.x, c.y, c.width, c.height}, action, zone == selected_, false, custom && longLayer_);
    }
  }
  for (int i = 0; i < tapzones::Grid::kMiniCount; ++i) {
    const uint8_t action = custom ? SETTINGS.miniZones[i] : tapzones::NONE;
    // Mini zones are edited by touch only; never mark them as the key cursor.
    const tapzones::Rect c = grid.miniCell(i);
    drawZone(Rect{c.x, c.y, c.width, c.height}, action, false, true, false);
  }

  // Reset button (outline pill, label centred).
  {
    const int radius = std::min(16, std::min(l.resetButton.width, l.resetButton.height) / 2);
    renderer.drawRoundedRect(l.resetButton.x, l.resetButton.y, l.resetButton.width, l.resetButton.height, 2, radius,
                             true);
    const std::string text = renderer.truncatedText(UI_12_FONT_ID, tr(STR_TAP_ZONE_RESET), l.resetButton.width - 16);
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, text.c_str());
    const int textHeight = renderer.getTextHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, l.resetButton.x + (l.resetButton.width - textWidth) / 2,
                      l.resetButton.y + (l.resetButton.height - textHeight) / 2, text.c_str());
  }

  if (popup_.isActive()) {
    // Draw the picker over the preview and hand it the Back/Select/Up/Down
    // hints (OptionPopup::render only paints the panel).
    popup_.render(renderer);
    const auto popupLabels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
