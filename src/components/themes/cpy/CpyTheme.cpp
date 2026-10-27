#include "CpyTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "InxItemLayout.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/inx_apps.h"
#include "CpyClockDraw.h"
#include "CpyDuckDraw.h"
#include "cpy_empty_illustration.h"
#include "cpy_sleep_scene.h"
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
#include "components/icons/search32.h"
#include "components/icons/uiChromeIcons.h"
#else
#include "components/icons/inx_tabs.h"
#endif
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
// The tab glyphs are 56x56 bitmaps (icon_tab_*_56) — the drawing stride must
// match them exactly.
constexpr int kIconSize = UiHighDpiProfile::enabled ? 40 : 38;
constexpr int kTabBarLabelGap = 2;
constexpr int kIndicatorWidth = 56;
constexpr int kIndicatorHeight = 4;
constexpr int kRowPadding = 32;
constexpr int kListIconSize = 24;
constexpr int kMenuIconSize = 32;
constexpr int kIconGap = 10;
constexpr int kMaxValueWidth = 240;
constexpr int kChipPadX = 24;
constexpr int kChipGap = 14;

#ifndef CROSSMUX_UI_PROFILE_HIGH_DPI
void drawIconBits(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y) {
  constexpr int rowBytes = (kIconSize + 7) / 8;
  for (int row = 0; row < kIconSize; ++row) {
    for (int column = 0; column < kIconSize; ++column) {
      const uint8_t byte = icon[row * rowBytes + column / 8];
      if (((byte >> (7 - column % 8)) & 1U) == 0) renderer.drawPixel(x + column, y + row, true);
    }
  }
}

UIIcon uiIconForTab(const MainTab tab) {
  switch (tab) {
    case MainTab::Recent:
      return UIIcon::Recent;
    case MainTab::Library:
      return UIIcon::Library;
    case MainTab::Settings:
      return UIIcon::Settings;
    case MainTab::Apps:
      return UIIcon::Apps;
    case MainTab::None:
      return UIIcon::None;
  }
  return UIIcon::None;
}

const uint8_t* iconForTab(const MainTab tab) {
  switch (tab) {
    case MainTab::Recent:
      return InxRecentTabIcon;
    case MainTab::Library:
      return InxLibraryTabIcon;
    case MainTab::Settings:
      return InxSettingsTabIcon;
    case MainTab::Apps:
      return InxAppsTabIcon;
    case MainTab::None:
      return nullptr;
  }
  return nullptr;
}
#endif

#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
void drawIconScaled(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y,
                    const int size) {
  const int rowBytes = (icon.w + 7) / 8;
  for (int row = 0; row < size; ++row) {
    const int sourceY = row * icon.h / size;
    const uint8_t* sourceRow = icon.bits + sourceY * rowBytes;
    for (int column = 0; column < size; ++column) {
      const int sourceX = column * icon.w / size;
      if (((sourceRow[sourceX / 8] >> (7 - (sourceX & 7))) & 1U) == 0) {
        renderer.drawPixel(x + column, y + row, true);
      }
    }
  }
}

const freeink::Icon* highDpiTabIcon(const MainTab tab) {
  switch (tab) {
    case MainTab::Recent:
      return &icon_tab_recent_56;
    case MainTab::Library:
      return &icon_tab_library_56;
    case MainTab::Apps:
      return &icon_search_32;
    case MainTab::Settings:
      return &icon_tab_settings_56;
    case MainTab::None:
      return nullptr;
  }
  return nullptr;
}
#endif

const char* tabLabel(const size_t index) {
  static constexpr StrId kLabels[] = {StrId::STR_HOME_SHORTCUT, StrId::STR_LIBRARY, StrId::STR_FIND_BOOKS,
                                      StrId::STR_MINE};
  return index < std::size(kLabels) ? I18N.get(kLabels[index]) : "";
}

bool batteryPercentageVisible() {
  return SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
}
}  // namespace

void CpyTheme::drawBatteryPill(const GfxRenderer& renderer, const int right, const int top, const bool showPercent) {
  const int width = CpyMetrics::values.batteryWidth;
  const int height = CpyMetrics::values.batteryHeight;
  const int x = right - width;
  const uint16_t percentage = powerManager.getBatteryPercentage();

  renderer.drawRoundedRect(x, top, width, height, 2, height / 2, true);

  constexpr int inset = 3;
  const int cavityX = x + inset;
  const int cavityY = top + inset;
  const int cavityWidth = width - inset * 2;
  const int cavityHeight = height - inset * 2;
  const int filledWidth =
      std::clamp(static_cast<int>(percentage) * cavityWidth / 100, 0, cavityWidth);
  if (filledWidth > 0) {
    renderer.fillRoundedRect(cavityX, cavityY, filledWidth, cavityHeight, cavityHeight / 2, Color::Black);
  }

  const bool charging = gpio.isUsbConnected();
  if (showPercent) {
    char text[8];
    std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(percentage));
    // The numeric face only: the UI text faces render ~27px ink at high DPI,
    // taller than the whole 26px pill (device feedback 2026-10-17: digits and
    // pill out of proportion). The legacy numeric bitmap sits at ~17px.
    constexpr int fontId = STATUS_NUMERIC_FONT_ID;
    const int textWidth = renderer.getTextWidth(fontId, text);
    const int textY = top + (height - renderer.getLineHeight(fontId)) / 2;
    if (textWidth + 10 <= cavityWidth) {
      // Centered in the pill; the part over the fill redraws white at the
      // fill boundary (two passes, second one clipped to the cavity).
      const int textX = x + (width - textWidth) / 2;
      renderer.drawText(fontId, textX, textY, text, true);
      if (filledWidth > 0) {
        const GfxRenderer::ClipScope clip(renderer, cavityX, cavityY, filledWidth, cavityHeight);
        renderer.drawText(fontId, textX, textY, text, false);
      }
    } else {
      renderer.drawText(fontId, x + width + 8, textY, text, true);
    }
  }
  if (charging) drawBatteryLightningBolt(renderer, x + inset + 1, top + (height - 14) / 2);
}

int CpyTheme::drawStatusBarBattery(const GfxRenderer& renderer, const int x, const int y,
                                   const bool showPercent) const {
  // Same capsule as the header pill, so the reader's bar and the home chrome agree
  // on where the percentage lives. The classic icon offsets itself a few pixels
  // down to sit with the text; keep that so the pill's centre matches the lane.
  const int top = y + (UiHighDpiProfile::enabled ? 8 : 6);
  drawBatteryPill(renderer, x + CpyMetrics::values.batteryWidth, top, showPercent);
  return CpyMetrics::values.batteryWidth;
}

void CpyTheme::fillBatteryIcon(const GfxRenderer& renderer, const Rect rect, const uint16_t percentage) const {
  // Rounded capsule fill for the bands that still compose their own battery
  // (reader status). The shell battery re-draws through drawBatteryPill.
  const int maxWidth = rect.width - 4;
  const int height = rect.height - 4;
  if (maxWidth <= 0 || height <= 0) return;
  const int filledWidth = std::clamp(static_cast<int>(percentage) * maxWidth / 100, 2, maxWidth);
  renderer.fillRoundedRect(rect.x + 2, rect.y + 2, filledWidth, height, height / 2, Color::Black);
  if (gpio.isUsbConnected()) drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
}

void CpyTheme::applyCompactDensity(ThemeMetrics& metrics) const {
  // Compact tightens only the row rhythm so lists and settings show more per
  // screen; the chrome (title band, tab bar, keyboard, cards) keeps its size
  // so the identity and hit targets stay put.
  if (UiHighDpiProfile::enabled) {
    metrics.listRowHeight = 80;
    metrics.listWithSubtitleRowHeight = 108;
    metrics.menuRowHeight = 80;
  } else {
    metrics.listRowHeight = 56;
    metrics.listWithSubtitleRowHeight = 68;
    metrics.menuRowHeight = 56;
  }
}

void CpyTheme::drawHeader(const GfxRenderer& renderer, const Rect rect, const char* title, const char* subtitle,
                          const bool backButton) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const int hairlineY = rect.y + rect.height - 1;
  renderer.drawLine(rect.x, hairlineY, rect.x + rect.width - 1, hairlineY, true);

  const bool showPercent = batteryPercentageVisible();
  const int pillTop = rect.y + (rect.height - CpyMetrics::values.batteryHeight) / 2;
  constexpr int edgeInset = 32;
  drawBatteryPill(renderer, rect.x + rect.width - edgeInset, pillTop, showPercent);

  int titleLeft = rect.x + kRowPadding;
  const bool showBack = backButton && title != nullptr && gpio.hasTouch();
  if (showBack) {
    constexpr int buttonSize = 56;
    const int buttonX = rect.x + 20;
    const int buttonY = rect.y + (rect.height - buttonSize) / 2;
    renderer.drawRoundedRect(buttonX, buttonY, buttonSize, buttonSize, 1, buttonSize / 2, true);
    const int centerX = buttonX + buttonSize / 2;
    const int centerY = buttonY + buttonSize / 2;
    renderer.drawLine(centerX + 4, centerY - 10, centerX - 5, centerY, true);
    renderer.drawLine(centerX - 5, centerY, centerX + 4, centerY + 10, true);
    HeaderBackTapTarget::set(buttonX, buttonY, buttonSize, buttonSize);
    titleLeft = buttonX + buttonSize + 12;
  } else {
    HeaderBackTapTarget::clear();
  }

  const int pillLeft = rect.x + rect.width - edgeInset - CpyMetrics::values.batteryWidth;
  int titleRight = pillLeft - 12;
  if (subtitle && *subtitle) {
    const int subtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, subtitle);
    const int subtitleX = pillLeft - 8 - subtitleWidth;
    if (subtitleWidth > 0 && subtitleX > titleLeft) {
      const int subtitleTop = rect.y + (rect.height - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
      const GfxRenderer::ClipScope clip(renderer, subtitleX, rect.y, subtitleWidth + 2, rect.height - 1);
      renderer.drawText(SMALL_FONT_ID, subtitleX, subtitleTop, subtitle);
      titleRight = std::min(titleRight, subtitleX - kIconGap);
    }
  }

  constexpr int titleFont = UI_12_FONT_ID;
  if (title && *title && titleRight > titleLeft) {
    const std::string shown = renderer.truncatedText(titleFont, title, titleRight - titleLeft);
    const int textWidth = renderer.getTextWidth(titleFont, shown.c_str(), EpdFontFamily::BOLD);
    const int textX = titleLeft + (titleRight - titleLeft - textWidth) / 2;
    const int textY = rect.y + (rect.height - renderer.getLineHeight(titleFont)) / 2;
    const GfxRenderer::ClipScope clip(renderer, titleLeft, rect.y, titleRight - titleLeft, rect.height - 1);
    renderer.drawText(titleFont, textX, textY, shown.c_str(), true, EpdFontFamily::BOLD);
  }
}

void CpyTheme::drawSubHeader(const GfxRenderer& renderer, const Rect rect, const char* label,
                             const char* rightLabel) const {
  int labelRight = rect.x + rect.width - kRowPadding;
  if (rightLabel && *rightLabel) {
    const int rightWidth =
        std::min(renderer.getTextWidth(UI_10_FONT_ID, rightLabel), std::max(0, rect.width / 2 - kRowPadding));
    labelRight -= rightWidth + kIconGap;
    if (rightWidth > 0) {
      const GfxRenderer::ClipScope clip(renderer, labelRight + kIconGap, rect.y, rightWidth, rect.height - 1);
      renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - kRowPadding - rightWidth,
                        rect.y + std::max(0, (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2), rightLabel);
    }
  }
  if (label && *label && labelRight > rect.x + kRowPadding) {
    const GfxRenderer::ClipScope clip(renderer, rect.x + kRowPadding, rect.y, labelRight - rect.x - kRowPadding,
                                      rect.height - 1);
    renderer.drawText(UI_10_FONT_ID, rect.x + kRowPadding,
                      rect.y + std::max(0, (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2), label, true,
                      EpdFontFamily::BOLD);
  }
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

Rect CpyTheme::tabChipRect(const GfxRenderer& renderer, const Rect& band, const std::vector<TabInfo>& tabs,
                           const int index) {
  const int height = std::clamp(band.height - 16, 44, 64);
  const int y = band.y + (band.height - height) / 2;
  int x = band.x + kRowPadding;
  for (int i = 0; i <= index && i < static_cast<int>(tabs.size()); ++i) {
    const int width = renderer.getTextWidth(UI_10_FONT_ID, tabs[i].label,
                                            tabs[i].selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR) +
                      kChipPadX * 2;
    if (i == index) return Rect{x, y, width, height};
    x += width + kChipGap;
  }
  return Rect{};
}

void CpyTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                          const bool) const {
  for (int index = 0; index < static_cast<int>(tabs.size()); ++index) {
    const Rect chip = tabChipRect(renderer, rect, tabs, index);
    if (chip.width <= 0) return;
    const bool active = tabs[index].selected;
    if (active) {
      renderer.fillRoundedRect(chip.x, chip.y, chip.width, chip.height, chip.height / 2, Color::Black);
    } else {
      renderer.drawRoundedRect(chip.x, chip.y, chip.width, chip.height, 2, chip.height / 2, true);
    }
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tabs[index].label,
                                                active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const GfxRenderer::ClipScope clip(renderer, chip.x, chip.y, chip.width, chip.height);
    renderer.drawText(UI_10_FONT_ID, chip.x + (chip.width - textWidth) / 2,
                      chip.y + (chip.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, tabs[index].label, !active,
                      active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}

bool CpyTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                 const int x, const int y, int& index) const {
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height) return false;
  for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
    const Rect chip = tabChipRect(renderer, rect, tabs, i);
    if (chip.width > 0 && x >= chip.x && x < chip.x + chip.width && y >= chip.y && y < chip.y + chip.height) {
      index = i;
      return true;
    }
  }
  return false;
}

int CpyTheme::getListRowStep(const bool hasSubtitle) const {
  return CpyMetrics::values.listRowHeight + (hasSubtitle ? 32 : 0);
}

int CpyTheme::getListPageItems(const int contentHeight, const bool hasSubtitle) const {
  return std::max(1, contentHeight / getListRowStep(hasSubtitle));
}

void CpyTheme::drawList(const GfxRenderer& renderer, const Rect rect, const int itemCount, const int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle,
                        const std::function<UIIcon(int index)>& rowIcon,
                        const std::function<std::string(int index)>& rowValue, const bool,
                        const std::function<bool(int index)>& rowDimmed, const bool showSelection,
                        const std::function<bool(int index)>& rowHeading) const {
  if (itemCount <= 0 || rect.height <= 0) return;

  const int rowHeight = getListRowStep(rowSubtitle != nullptr);
  const int pageItems = getListPageItems(rect.height, rowSubtitle != nullptr);
  const int pageStart = selectedIndex >= 0 ? selectedIndex / pageItems * pageItems : 0;
  const int pageEnd = std::min(itemCount, pageStart + pageItems);
  const bool hasScrollBar = itemCount > pageItems;
  const int contentRight = rect.x + rect.width - (hasScrollBar ? 10 : 0);
  const int iconSize = rowSubtitle != nullptr ? kMenuIconSize : kListIconSize;
  const bool selectionVisible = showSelection && UITheme::getInstance().showSelectionCursor();

  for (int index = pageStart; index < pageEnd; ++index) {
    const int slot = index - pageStart;
    const int rowY = rect.y + slot * rowHeight;
    const bool selected = selectionVisible && index == selectedIndex;
    if (selected) renderer.fillRect(rect.x, rowY, rect.width, rowHeight, true);

    int textX = rect.x + kRowPadding;
    if (rowIcon) {
      if (UiHighDpiProfile::enabled) {
        const auto bitmap = listIconFor(rowIcon(index), UiHighDpiProfile::controlIconSize);
        if (bitmap) {
          freeink::ui::GfxRendererTarget target(renderer, true);
          target.bitmap(
              freeink::ui::Rect{static_cast<int16_t>(textX), static_cast<int16_t>(rowY + (rowHeight - 48) / 2), 48, 48},
              bitmap, freeink::ui::BitmapMode::Center,
              freeink::ui::Paint::solid(selected ? freeink::ui::Color::White : freeink::ui::Color::Black));
          textX += 48 + kIconGap;
        }
      } else if (const uint8_t* bitmap = iconForName(rowIcon(index), iconSize)) {
        const int iconY = rowY + (rowHeight - iconSize) / 2;
        if (selected)
          renderer.drawIconInverted(bitmap, textX, iconY, iconSize);
        else
          renderer.drawIcon(bitmap, textX, iconY, iconSize);
        textX += iconSize + kIconGap;
      }
    }

    std::string value;
    int valueWidth = 0;
    if (rowValue) {
      value = renderer.truncatedText(UI_10_FONT_ID, rowValue(index).c_str(), kMaxValueWidth);
      valueWidth = value.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, value.c_str()) + kIconGap;
    }
    const bool heading = rowHeading && rowHeading(index);
    const int titleFont = heading ? UI_12_FONT_ID : UI_10_FONT_ID;
    const auto titleStyle = heading ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = std::max(1, contentRight - kRowPadding - textX - valueWidth);
    const std::string title = renderer.truncatedText(titleFont, rowTitle(index).c_str(), textWidth, titleStyle);
    const int titleY = rowY + (rowSubtitle ? (UiHighDpiProfile::enabled ? 20 : 12)
                                           : (rowHeight - renderer.getLineHeight(titleFont)) / 2);
    renderer.drawText(titleFont, textX, titleY, title.c_str(), !selected, titleStyle);

    if (rowSubtitle) {
      const std::string subtitle = renderer.truncatedText(SMALL_FONT_ID, rowSubtitle(index).c_str(), textWidth);
      renderer.drawText(SMALL_FONT_ID, textX,
                        rowY + (UiHighDpiProfile::enabled ? 20 + renderer.getLineHeight(titleFont) + 6 : 36),
                        subtitle.c_str(), !selected);
    }
    if (!value.empty()) {
      const int valueX = contentRight - kRowPadding - renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
      const int valueY = rowY + (rowHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, valueX, valueY, value.c_str(), !selected);
    }
    if (rowDimmed && rowDimmed(index) && !selected) {
      drawDitherMask(renderer, textX, rowY, std::max(0, contentRight - textX), rowHeight - 1);
    }
    if (slot + 1 < pageEnd - pageStart) {
      renderer.drawLine(rect.x, rowY + rowHeight - 1, rect.x + rect.width - 1, rowY + rowHeight - 1, true);
    }
  }

  drawSideScrollBar(renderer, rect, itemCount, pageStart, pageItems);
}

void CpyTheme::drawButtonMenu(GfxRenderer& renderer, const Rect rect, const int buttonCount, const int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon, const int) const {
  const int rowHeight = CpyMetrics::values.menuRowHeight;
  const int pageItems = std::max(1, rect.height / rowHeight);
  const int clamped = selectedIndex >= 0 && selectedIndex < buttonCount ? selectedIndex : 0;
  const int pageStart = clamped / pageItems * pageItems;
  const int pageEnd = std::min(buttonCount, pageStart + pageItems);
  const bool selectionVisible = UITheme::getInstance().showSelectionCursor();
  for (int index = pageStart; index < pageEnd; ++index) {
    const int rowY = rect.y + (index - pageStart) * rowHeight;
    const bool selected = selectionVisible && index == selectedIndex;
    if (selected) renderer.fillRect(rect.x, rowY, rect.width, rowHeight, true);

    int textX = rect.x + kRowPadding;
    if (rowIcon) {
      const UIIcon icon = rowIcon(index);
      if (InxAppIcons::get(icon)) {
        const int iconY = rowY + (rowHeight - kMenuIconSize) / 2;
        InxAppIcons::draw(renderer, icon, textX, iconY, 1, selected);
        textX += kMenuIconSize + kIconGap;
      } else if (const uint8_t* bitmap = iconForName(icon, kMenuIconSize)) {
        const int iconY = rowY + (rowHeight - kMenuIconSize) / 2;
        if (selected)
          renderer.drawIconInverted(bitmap, textX, iconY, kMenuIconSize);
        else
          renderer.drawIcon(bitmap, textX, iconY, kMenuIconSize);
        textX += kMenuIconSize + kIconGap;
      }
    }

    const std::string label = renderer.truncatedText(UI_12_FONT_ID, buttonLabel(index).c_str(),
                                                     std::max(1, rect.x + rect.width - kRowPadding - textX));
    const int textY = rowY + (rowHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, textY, label.c_str(), !selected);
    if (index + 1 < pageEnd) {
      renderer.drawLine(rect.x, rowY + rowHeight - 1, rect.x + rect.width - 1, rowY + rowHeight - 1, true);
    }
  }
  drawSideScrollBar(renderer, rect, buttonCount, pageStart, pageItems);
}

CpyTheme::MenuRowGeometry CpyTheme::getMenuRowGeometry(const GfxRenderer&, const Rect& rect, const int selectedIndex,
                                                       const int rowCount) const {
  // Mirror of CpyTheme::drawButtonMenu: fixed row height, paged from rect.y.
  const int rowHeight = CpyMetrics::values.menuRowHeight;
  const int pageItems = std::max(1, rect.height / rowHeight);
  const int clamped = selectedIndex >= 0 && selectedIndex < rowCount ? selectedIndex : 0;
  const int pageStart = clamped / pageItems * pageItems;
  return {rect.y,
          rowHeight,
          rowHeight,
          pageStart,
          std::min(rowCount - pageStart, pageItems),
          rect.x + kRowPadding,
          rect.x + rect.width};
}

void CpyTheme::drawOptionPopup(const GfxRenderer& renderer, const char* title, const char* headline,
                               const int headlineHeight, const std::vector<std::string>& options,
                               const int selectedIndex) const {
  if (options.empty()) return;

  const int optionCount = static_cast<int>(options.size());
  const int selected = std::clamp(selectedIndex, 0, optionCount - 1);
  drawModalScrim(renderer);
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto layout =
      InxOptionGeometry::layout(screen, optionCount, selected, headlineHeight);
  const int visibleRows = layout.rows;
  const int start = layout.first;
  const Rect panel = layout.panel;
  const bool selectionVisible = UITheme::getInstance().showSelectionCursor();

  renderer.fillRoundedRect(panel.x, panel.y, panel.width, panel.height, CpyMetrics::values.popupCornerRadius,
                           Color::White);
  renderer.drawRoundedRect(panel.x, panel.y, panel.width, panel.height, CpyMetrics::values.popupFrameThickness,
                           CpyMetrics::values.popupCornerRadius, true);

  constexpr int innerPad = 28;
  const std::string shownTitle =
      renderer.truncatedText(UI_10_FONT_ID, title, panel.width - innerPad * 2, EpdFontFamily::BOLD);
  const int titleY = panel.y + (InxOptionGeometry::headerHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, panel.x + innerPad, titleY, shownTitle.c_str(), true, EpdFontFamily::BOLD);

  // Optional wrapped body (a confirmation's subject) under the title. Its band
  // height was measured by the caller with this exact font / width / line cap, so
  // the lines land where the option hit rects (layout.optionRect) expect them and
  // the body can never overlap the first option.
  if (headline != nullptr && headline[0] != '\0' && headlineHeight > 0) {
    const int bodyWidth = panel.width - InxOptionGeometry::headlineInnerPad * 2;
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const auto lines =
        renderer.wrappedText(UI_10_FONT_ID, headline, bodyWidth, InxOptionGeometry::headlineMaxLines,
                             EpdFontFamily::REGULAR);
    int bodyY = panel.y + InxOptionGeometry::headerHeight +
                std::max(0, (headlineHeight - static_cast<int>(lines.size()) * lineH) / 2);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, panel.x + InxOptionGeometry::headlineInnerPad, bodyY, line.c_str(), true,
                        EpdFontFamily::REGULAR);
      bodyY += lineH;
    }
  }

  for (int slot = 0; slot < visibleRows; ++slot) {
    const int optionIndex = start + slot;
    const Rect row{panel.x + 12,
                   panel.y + InxOptionGeometry::headerHeight + headlineHeight + slot * InxOptionGeometry::rowHeight + 4,
                   panel.width - 24, InxOptionGeometry::rowHeight - 8};
    const bool isSelected = selectionVisible && optionIndex == selected;
    if (isSelected) {
      renderer.fillRoundedRect(row.x, row.y, row.width, row.height, CpyMetrics::values.optionPopupSelectionRadius,
                               Color::Black);
    }
    const std::string option = renderer.truncatedText(UI_10_FONT_ID, options[optionIndex].c_str(), row.width - 40);
    const int textX = row.x + 20;
    const int textY = row.y + (row.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, option.c_str(), !isSelected,
                      isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  if (optionCount > visibleRows) {
    const int trackX = panel.x + panel.width - 10;
    const int trackY = panel.y + InxOptionGeometry::headerHeight + headlineHeight;
    const int trackHeight = visibleRows * InxOptionGeometry::rowHeight;
    const int thumbHeight = std::max(8, trackHeight * visibleRows / optionCount);
    const int maxStart = optionCount - visibleRows;
    const int thumbY = trackY + (maxStart > 0 ? start * (trackHeight - thumbHeight) / maxStart : 0);
    renderer.fillRect(trackX, trackY, 2, trackHeight, true);
    renderer.fillRect(trackX - 2, thumbY, 6, thumbHeight, true);
  }
}

void CpyTheme::drawMainTabBar(const GfxRenderer& renderer, const Rect rect, const MainTab selected) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  // UI baseline: a shallow wave instead of a straight hairline. The active
  // item gets three ripple arcs behind its icon and a small duck on the top
  // edge; the bar itself stays 88 px so it stops eating the page.
  constexpr float kTwoPi = 6.2831853f;
  const auto waveY = [rect](const int x) {
    return rect.y + static_cast<int>(std::lround(2.0f * std::sin(static_cast<float>(x) * kTwoPi / 28.0f)));
  };
  for (int x = 0; x + 1 < rect.width; ++x) {
    renderer.drawLine(rect.x + x, waveY(x), rect.x + x + 1, waveY(x + 1), true);
  }
#else
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width - 1, rect.y, true);
#endif

  const int labelFont = SMALL_FONT_ID;
  const int labelHeight = renderer.getLineHeight(labelFont);
  const int contentHeight = kIconSize + kTabBarLabelGap + labelHeight;
  const int top = rect.y + std::max(8, (rect.height - contentHeight) / 2);

  for (size_t i = 0; i < MainTabs::values.size(); ++i) {
    const MainTab tab = MainTabs::values[i];
    const auto bounds = MainTabs::tabBounds(static_cast<int>(i), rect.width);
    const int left = rect.x + bounds.left;
    const int right = rect.x + bounds.right;
    const int centerX = (left + right) / 2;
    const bool active = tab == selected;

#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
    if (active) {
      const int centerY = top + kIconSize / 2;
      for (const int radius : {kIconSize / 2 + 8, kIconSize / 2 + 14, kIconSize / 2 + 20}) {
        renderer.drawArc(radius, centerX, centerY, -1, -1, 1, true);
        renderer.drawArc(radius, centerX, centerY, 1, -1, 1, true);
      }
    }
    if (const freeink::Icon* icon = highDpiTabIcon(tab)) {
      drawIconScaled(renderer, *icon, centerX - kIconSize / 2, top, kIconSize);
    }
#else
    const UIIcon ui = uiIconForTab(tab);
    const bool modern = ui != UIIcon::None &&
                        InxAppIcons::drawModern(renderer, ui, centerX - kIconSize / 2, top, kIconSize, false);
    if (!modern) {
      if (const uint8_t* icon = iconForTab(tab)) {
        drawIconBits(renderer, icon, centerX - kIconSize / 2, top);
      }
    }
#endif

    const char* label = tabLabel(i);
    const auto labelStyle = active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int labelWidth = renderer.getTextWidth(labelFont, label, labelStyle);
    const GfxRenderer::ClipScope clip(renderer, left, rect.y, right - left, rect.height);
    renderer.drawText(labelFont, centerX - labelWidth / 2, top + kIconSize + kTabBarLabelGap, label, true, labelStyle);
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
    (void)active;
#else
    if (active) {
      renderer.fillRect(centerX - kIndicatorWidth / 2, rect.y + 2, kIndicatorWidth, kIndicatorHeight);
    }
#endif
  }

#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  if (selected != MainTab::None) {
    const int index = MainTabs::indexOf(selected);
    if (index >= 0) {
      const auto bounds = MainTabs::tabBounds(index, rect.width);
      const int centerX = rect.x + (bounds.left + bounds.right) / 2;
      cpy_duck::draw(renderer, cpy_duck_stand_36, centerX - 18, rect.y - 30);
    }
  }
#endif
}

int CpyTheme::drawProgressBar(const GfxRenderer& renderer, const Rect rect, const size_t current, const size_t total,
                              const bool showPercentage) const {
  if (rect.width <= 0 || rect.height <= 0) return rect.y;
  const int percent = total == 0 ? 0 : static_cast<int>(static_cast<uint64_t>(current) * 100 / total);
  const int waveY = rect.y + rect.height / 2;
  const int bottom = rect.y + rect.height;
  constexpr float kTwoPi = 6.2831853f;
  const auto waveAt = [waveY](const int x) {
    return waveY + static_cast<int>(std::lround(3.0f * std::sin(static_cast<float>(x) * kTwoPi / 28.0f)));
  };

  const int fillEnd = rect.width * percent / 100;
  for (int x = 0; x < rect.width; ++x) {
    const int y0 = waveAt(x);
    if (x < fillEnd) {
      renderer.fillRectDither(rect.x + x, y0 + 1, 1, std::max(1, bottom - y0 - 1), Color::DarkGray);
    }
    if (x + 1 < rect.width) {
      renderer.drawLine(rect.x + x, y0, rect.x + x + 1, waveAt(x + 1), true);
    }
  }

  // The duck rides the boundary; its own waterline sits ~31px into the 40px
  // art (the simplified geometry's wave row), so align the two lines.
  constexpr int duckSize = 40;
  constexpr int duckWaterline = 31;
  const int duckX = std::clamp(fillEnd - duckSize / 2, 0, std::max(0, rect.width - duckSize));
  cpy_duck::drawMirroredX(renderer, cpy_duck_swim_40, rect.x + duckX, waveY - duckWaterline);

  if (showPercentage) {
    char percentText[16];
    std::snprintf(percentText, sizeof(percentText), "%d%%", percent);
    renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText);
  }
  return rect.y + measureProgressBarHeight(renderer, rect.height, showPercentage);
}

void CpyTheme::drawCardFrame(const GfxRenderer& renderer, const Rect rect, const bool focused) const {
  constexpr int radius = 20;
  // Design §3.1: the card frame is g2 = 25 %, not 50 %. DarkGray is the 50 %
  // checkerboard (x+y even); at 297 ppi it reads as a hard grey line and made
  // every card look twice as heavy as the mockup. LightGray is the 25 %
  // lattice. The focused (selected) ring stays solid black.
  // Same primitive for the home tiles, app tiles and settings group ring, so
  // the four hand-rolled frames that used to surround cards (solid black r20,
  // solid black r16, dotted r0, dithered r20) converge on one weight.
  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, radius,
                           focused ? Color::Black : Color::LightGray);
  renderer.fillRoundedRect(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4, radius - 2, Color::White);
}

void CpyTheme::drawDuckStand(const GfxRenderer& renderer, const int x, const int y) {
  cpy_duck::draw(renderer, cpy_duck_stand_36, x, y);
}

void CpyTheme::drawDuckSwim(const GfxRenderer& renderer, const int x, const int y) {
  cpy_duck::draw(renderer, cpy_duck_swim_48, x, y);
}

void CpyTheme::drawModalScrim(const GfxRenderer& renderer) const {
  // A light dither reads as a soft grey; modal open/close already pays a full
  // refresh, where a black mask flickers and ghosts far worse on e-ink.
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  renderer.fillRectDither(screen.x, screen.y, screen.width, screen.height, Color::LightGray);
}

Rect CpyTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  // CPY card language for every transient message: white card, 25 % ring,
  // black text, centred on the panel. The legacy implementation drew a
  // solid-black strip whose width followed the text, which read as a system
  // alert rather than part of this UI.
  constexpr int kMarginX = 40;
  constexpr int kMarginY = 28;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int maxTextWidth = std::max(1, screenW - 64 - kMarginX * 2);
  const std::string shown = renderer.truncatedText(UI_12_FONT_ID, message, maxTextWidth);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, shown.c_str(), EpdFontFamily::REGULAR);
  const int w = textWidth + kMarginX * 2;
  const int h = textHeight + kMarginY * 2;
  const int x = (screenW - w) / 2;
  const int y = (screenH - h) / 2;
  drawCardFrame(renderer, Rect{x, y, w, h}, false);
  renderer.drawText(UI_12_FONT_ID, x + kMarginX, y + kMarginY, shown.c_str(), true, EpdFontFamily::REGULAR);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void CpyTheme::drawEmptyState(const GfxRenderer& renderer, const Rect rect, const char* message) const {
  // Large empty bands (shelf, library) get the v5 illustration: a duck on a
  // stack of books, rendered as a 604x420 1-bit bitmap. Small bands (home
  // continue card, file browser) keep the old light duck lattice so the
  // message stays legible without a 31 KB bitmap squeezed into 200 px.
  constexpr int kMinIllustrationWidth = 500;
  constexpr int kMinIllustrationHeight = 470;
  if (rect.width >= kMinIllustrationWidth && rect.height >= kMinIllustrationHeight) {
    const int drawWidth = std::min(kEmptyIllustrationWidth, rect.width - 32);
    const int drawHeight = drawWidth * kEmptyIllustrationHeight / kEmptyIllustrationWidth;
    const int x = rect.x + (rect.width - drawWidth) / 2;
    const int y = rect.y + 8;
    for (int row = 0; row < drawHeight; ++row) {
      const int sourceY = row * kEmptyIllustrationHeight / drawHeight;
      const uint8_t* sourceRow = kEmptyIllustrationBits + sourceY * kEmptyIllustrationStride;
      for (int column = 0; column < drawWidth; ++column) {
        const int sourceX = column * kEmptyIllustrationWidth / drawWidth;
        if (((sourceRow[sourceX / 8] >> (7 - (sourceX & 7))) & 1U) == 0) {
          renderer.drawPixel(x + column, y + row, true);
        }
      }
    }
    if (message != nullptr && *message != '\0' && rect.height >= drawHeight + 64) {
      constexpr int fontId = UI_12_FONT_ID;
      const auto lines = renderer.wrappedText(fontId, message, std::max(1, rect.width - 96), 2);
      int textY = y + drawHeight + 12;
      for (const auto& line : lines) {
        renderer.drawText(fontId, rect.x + (rect.width - renderer.getTextWidth(fontId, line.c_str(),
                                                                               EpdFontFamily::BOLD)) / 2,
                          textY, line.c_str(), true, EpdFontFamily::BOLD);
        textY += renderer.getLineHeight(fontId);
      }
    }
    return;
  }

  // Fallback: original sparse duck lattice + white message strip.
  constexpr int cell = 96;
  for (int y = rect.y + 12; y + 48 <= rect.y + rect.height; y += cell) {
    for (int x = rect.x + 12; x + 48 <= rect.x + rect.width; x += cell) {
      cpy_duck::drawSparse(renderer, cpy_duck_swim_48, x, y);
    }
  }

  if (message == nullptr || *message == '\0') return;
  constexpr int fontId = UI_12_FONT_ID;
  const auto lines = renderer.wrappedText(fontId, message, std::max(1, rect.width - 96), 2);
  if (lines.empty()) return;
  const int lineHeight = renderer.getLineHeight(fontId);
  constexpr int padX = 24;
  constexpr int padY = 14;
  int blockWidth = 0;
  for (const auto& line : lines) {
    blockWidth = std::max(blockWidth, renderer.getTextWidth(fontId, line.c_str(), EpdFontFamily::BOLD));
  }
  const int blockHeight = static_cast<int>(lines.size()) * lineHeight + padY * 2;
  const int blockX = rect.x + (rect.width - (blockWidth + padX * 2)) / 2;
  int textY = rect.y + (rect.height - blockHeight) / 2 + padY;
  renderer.fillRoundedRect(blockX, textY - padY + 2, blockWidth + padX * 2, blockHeight - 4,
                           CpyMetrics::values.popupCornerRadius, Color::White);
  for (const auto& line : lines) {
    renderer.drawText(fontId, rect.x + (rect.width - renderer.getTextWidth(fontId, line.c_str(), EpdFontFamily::BOLD)) / 2,
                      textY, line.c_str(), true, EpdFontFamily::BOLD);
    textY += lineHeight;
  }
}

void CpyTheme::drawSleepScreen(const GfxRenderer& renderer, const char* status, const char* bookTitle,
                               const int readPercent) const {
  // refreshed UI frame 12: a night scene only. No current book, no reading stats
  // and no Wi-Fi line -- standby must not claim anything the device cannot
  // know while asleep (ALIGNMENT-AUDIT.md).
  (void)bookTitle;
  (void)readPercent;
  renderer.clearScreen();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer);
  const int centerX = safe.x + safe.width / 2;

#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  // v5 night scene: moon, stars, a closed book, the sleeping duck, ripples and
  // the waterline. Date and time are stacked above the art, leaving the
  // designed whitespace in the middle.
  for (int row = 0; row < kSleepSceneHeight && row < renderer.getScreenHeight(); ++row) {
    const uint8_t* sourceRow = kSleepSceneBits + row * kSleepSceneStride;
    for (int column = 0; column < kSleepSceneWidth && column < renderer.getScreenWidth(); ++column) {
      if (((sourceRow[column / 8] >> (7 - (column & 7))) & 1U) == 0) {
        renderer.drawPixel(column, row, true);
      }
    }
  }
  // The raster's two stray mid-page stars are removed here; the design keeps
  // all three stars in the upper-right cluster around the moon.
  renderer.fillRect(178, 386, 26, 26, false);
  renderer.fillRect(458, 494, 26, 26, false);
  const auto star = [&renderer](const int cx, const int cy, const int arm) {
    renderer.fillRect(cx - 1, cy - arm, 3, arm * 2 + 1, true);
    renderer.fillRect(cx - arm, cy - 1, arm * 2 + 1, 3, true);
  };
  star(548, 64, 7);
  star(616, 92, 9);
  star(636, 166, 6);

  // The raster scene stops at the book; the design's three ripples and the
  // waterline below it are stroked here (1.5-2 px arcs read as hairlines and
  // were lost when the SVG was thresholded).
  const auto ripple = [&renderer](const int baseY, const int x0, const int x1, const int amplitude) {
    constexpr float kTwoPi = 6.2831853f;
    int prevY = baseY;
    for (int x = x0; x <= x1; ++x) {
      const float phase = static_cast<float>(x - x0) * kTwoPi / 64.0f;
      const int y = baseY + static_cast<int>(std::lround(static_cast<float>(amplitude) * std::sin(phase)));
      if (x > x0) renderer.drawLine(x - 1, prevY, x, y, 1, true);
      prevY = y;
    }
  };
  ripple(958, 170, 554, 4);
  ripple(982, 140, 556, 4);
  ripple(1016, 96, renderer.getScreenWidth() - 1, 3);
#else
  const int waterTop = safe.y + safe.height * 79 / 100;
  constexpr int bandHeight = 40;
  constexpr float kTwoPi = 6.2831853f;
  const auto waveAt = [waterTop](const int x) {
    return waterTop + static_cast<int>(std::lround(3.0f * std::sin(static_cast<float>(x) * kTwoPi / 28.0f)));
  };
  for (int x = 0; x < safe.width; ++x) {
    const int y0 = waveAt(x);
    renderer.fillRectDither(safe.x + x, y0 + 1, 1, std::max(1, waterTop + bandHeight - y0 - 1), Color::LightGray);
    if (x + 1 < safe.width) renderer.drawLine(safe.x + x, y0, safe.x + x + 1, waveAt(x + 1), true);
  }

  constexpr int duckSize = 200;
  const int duckX = safe.x + (safe.width - duckSize) / 2;
  const int duckY = waterTop - duckSize * 46 / 64;
  cpy_duck::draw(renderer, cpy_duck_sleep_200, duckX, duckY);
#endif

  // Date line on top, clock under it (design frame 12: 20 px date at y 64,
  // 96 px clock at y 100, both flush left at 40).
  if (cpy_clock::clockPlausible()) {
    struct tm local {};
    const time_t now = time(nullptr);
    localtime_r(&now, &local);
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
    static constexpr StrId kWeekdays[7] = {
        StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE,
        StrId::STR_CAL_WEEKDAY_WED, StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI,
        StrId::STR_CAL_WEEKDAY_SAT,
    };
    char date[64];
    std::snprintf(date, sizeof(date), "%d/%d \xc2\xb7 %s", local.tm_mon + 1, local.tm_mday,
                  I18N.get(kWeekdays[local.tm_wday % 7]));
    renderer.drawText(cpy_clock::kDateFont, safe.x + 40, safe.y + 56, date);
    // drawTime() centres a 320 px ink block; left-align it at 40.
    constexpr int kClockInkWidth = 320;
    cpy_clock::drawTime(renderer, safe.x + 40 + kClockInkWidth / 2, safe.y + 96, local.tm_hour, local.tm_min);
    // Battery capsule bottom-right, real percentage (no Wi-Fi line: standby is
    // offline by design).
    drawBatteryPill(renderer, safe.x + safe.width - 40, safe.y + safe.height - 24 - 20,
                    batteryPercentageVisible());
#else
    cpy_clock::drawDateLine(renderer, centerX, safe.y + 24);
    cpy_clock::drawTime(renderer, centerX, safe.y + 56, local.tm_hour, local.tm_min);
#endif
  } else if (status != nullptr && *status != '\0') {
    // Clock not synced yet: say so instead of drawing an implausible time.
    const int width = renderer.getTextWidth(cpy_clock::kDateFont, status);
    renderer.drawText(cpy_clock::kDateFont, centerX - width / 2, safe.y + safe.height * 21 / 100, status);
  }
}

void CpyTheme::drawSplash(const GfxRenderer& renderer, const char* status, const char* version) const {
  renderer.clearScreen();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer);

  constexpr int duckSize = 260;
  const int duckX = safe.x + (safe.width - duckSize) / 2;
  const int waterTop = safe.y + safe.height / 2 + 40;
  constexpr int bandHeight = 44;
  // The asset's wave row is y=46 of the 64-unit art grid (same geometry the
  // sleep duck uses); align it with the drawn waterline. A 30/64 guess here
  // lifted the duck ~65px and left its built-in wave detached below the band.
  constexpr int duckWaterline = 46;
  const int duckY = waterTop - duckSize * duckWaterline / 64;
  cpy_duck::draw(renderer, cpy_duck_swim_260, duckX, duckY);

  // Water band under the duck: dithered fill with a sine top edge.
  constexpr float kTwoPi = 6.2831853f;
  const auto waveAt = [waterTop](const int x) {
    return waterTop + static_cast<int>(std::lround(3.0f * std::sin(static_cast<float>(x) * kTwoPi / 28.0f)));
  };
  for (int x = 0; x < safe.width; ++x) {
    const int y0 = waveAt(x);
    renderer.fillRectDither(safe.x + x, y0 + 1, 1, std::max(1, waterTop + bandHeight - y0 - 1), Color::LightGray);
    if (x + 1 < safe.width) renderer.drawLine(safe.x + x, y0, safe.x + x + 1, waveAt(x + 1), true);
  }

  const int wordmarkY = waterTop + bandHeight + 48;
  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, wordmarkY, tr(STR_CPY_WORDMARK), true,
                            EpdFontFamily::BOLD);
  if (status && *status) {
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, wordmarkY + renderer.getLineHeight(UI_12_FONT_ID) + 12,
                              status);
  }
  if (version) {
    UITheme::drawCenteredText(renderer, safe, SMALL_FONT_ID, safe.y + safe.height - 36, version);
  }
}

void CpyTheme::drawMainTabTitle(const GfxRenderer& renderer, const Rect rect, const char* title,
                                const bool masthead) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const int hairlineY = rect.y + rect.height - 1;
  renderer.drawLine(rect.x, hairlineY, rect.x + rect.width - 1, hairlineY, true);

#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  {
  // refreshed UI mast (design frame 01): one quiet row. Page label on the left,
  // date + clock + battery capsule on the right. No large serif page title:
  // the designed pages put their own display type in the content area.
  (void)masthead;
  constexpr int labelFont = UI_10_FONT_ID;
  constexpr int duckSize = 26;
  const int centerY = rect.y + (rect.height - 1) / 2;
  int textX = rect.x + kRowPadding;
  if (title != nullptr) {
    cpy_duck::draw(renderer, cpy_duck_stand_36, textX, centerY - duckSize / 2);
    textX += duckSize + 14;
  }
  const int right = rect.x + rect.width - kRowPadding;
  const int pillLeft = right - CpyMetrics::values.batteryWidth;
  const int pillTop = centerY - CpyMetrics::values.batteryHeight / 2;
  drawBatteryPill(renderer, right, pillTop, batteryPercentageVisible());

  // Right metadata, laid out from the pill leftwards: clock closest to the
  // battery, then the date. Keeping both in one row is the whole point of the
  // merged mast; the content below starts where the design's does.
  const int metaY = centerY - renderer.getLineHeight(labelFont) / 2;
  char clock[9] = "--:--";
  TimeUtils::formatCurrentTime(clock, sizeof(clock), SETTINGS.clockFormat == 1);
  const int clockW = renderer.getTextWidth(labelFont, clock);
  const int clockX = pillLeft - 16 - clockW;
  renderer.drawText(labelFont, clockX, metaY, clock);

  const time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm local {};
    localtime_r(&now, &local);
    char date[32];
    std::snprintf(date, sizeof(date), "%d/%d", local.tm_mon + 1, local.tm_mday);
    const int dateW = renderer.getTextWidth(labelFont, date);
    const int dateX = clockX - 18 - dateW;
    if (dateX > textX + 8) renderer.drawText(labelFont, dateX, metaY, date);
  }

  if (title == nullptr || *title == '\0') return;
  const int titleRight = clockX - 12;
  const std::string shown =
      renderer.truncatedText(labelFont, title, std::max(1, titleRight - textX), EpdFontFamily::BOLD);
  renderer.drawText(labelFont, textX, metaY, shown.c_str(), true, EpdFontFamily::BOLD);
  return;
  }
#endif

  // The masthead strip is retired: mainTabMastheadHeight() is 0 and the
  // wordmark + date moved into the status bar row. The branch stays for the
  // base-class contract only; with no reserved height it can never draw.
  int titleTop = rect.y;
  if (masthead && mainTabMastheadHeight() > 0) {
    const int stripH = mainTabMastheadHeight();
    titleTop = rect.y + stripH;
  }

  int textX = rect.x + kRowPadding;
  // 页标题 = 宋体特黑 (design doc line 42). ReadPico embeds a real Song serif
  // at the mockups' ~48px (Noto Serif SC subset, main.cpp registration);
  // C3 keeps the legacy reader face because the subset does not fit its
  // flash budget (user feedback: the 18px alias read as a label, and the
  // built-in "serif" ids all resolve to one sans fallback).
  // Same gate as the registration in main.cpp: the embedded serif exists in
  // every high-density build (the simulator included — the stray BOARD_HAS_
  // PSRAM condition kept it out of sim verification and hid a missing-glyph
  // fallback). C3 has no high-density profile and keeps the legacy face.
#if defined(CROSSMUX_UI_PROFILE_HIGH_DPI)
  constexpr int titleFont = NOTOSERIF_TITLE_FONT_ID;
#else
  constexpr int titleFont = NOTOSERIF_18_FONT_ID;
#endif
  const int titleHeight = renderer.getLineHeight(titleFont);
  const int rowHeight = rect.height - (titleTop - rect.y);
  const int centerY = titleTop + (rowHeight - 1) / 2;
  if (title != nullptr) {
    constexpr int duckSize = 36;
    const int duckY = centerY - duckSize / 2;
    cpy_duck::draw(renderer, cpy_duck_stand_36, textX, duckY);
    textX += duckSize + 16;
  }
  const int titleRight = rect.x + rect.width - kRowPadding;
  if (title == nullptr || *title == '\0') return;

  const std::string shown =
      renderer.truncatedText(titleFont, title, std::max(1, titleRight - textX), EpdFontFamily::BOLD);
  renderer.drawText(titleFont, textX, centerY - titleHeight / 2, shown.c_str(), true, EpdFontFamily::BOLD);
}

void CpyTheme::drawMainTabStatusBar(const GfxRenderer& renderer, const Rect rect) const {
  if (rect.width <= 0 || rect.height <= 0) return;
  // One type size for the whole meta row (device feedback): the clock used to
  // be smaller than the wordmark and date, which read as three stray elements.
  // The pill's digits keep their own mono face; the row's text does not.
  constexpr int metaFontId = UI_10_FONT_ID;
  constexpr int edgeInset = 32;
  const int right = std::min(rect.x + rect.width - 1, renderer.getScreenWidth() - edgeInset);
  const int textY = rect.y + (rect.height - renderer.getLineHeight(metaFontId)) / 2;
  const GfxRenderer::ClipScope clip(renderer, rect.x, rect.y, rect.width, rect.height);

  char timeText[9] = "--:--";
  TimeUtils::formatCurrentTime(timeText, sizeof(timeText), SETTINGS.clockFormat == 1);
  renderer.drawText(metaFontId, std::max(rect.x, edgeInset), textY, timeText);

  // Masthead merged into this row (user feedback, 2026-10-16): the wordmark
  // sits centred and the date tucks against the battery pill, replacing the
  // separate 44px wordmark/date strip that used to sit above the title band.
  const char* wordmark = tr(STR_CPY_WORDMARK);
  const int wordmarkW = renderer.getTextWidth(metaFontId, wordmark);
  renderer.drawText(metaFontId, rect.x + (rect.width - wordmarkW) / 2, textY, wordmark);
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm local {};
    localtime_r(&now, &local);
    char date[16];
    std::snprintf(date, sizeof(date), "%d/%d", local.tm_mon + 1, local.tm_mday);
    const int dateW = renderer.getTextWidth(metaFontId, date);
    renderer.drawText(metaFontId, right - CpyMetrics::values.batteryWidth - 16 - dateW, textY, date);
  }
  // A single hairline under the row keeps the 报头 rule the strip carried.
  renderer.drawLine(rect.x + kRowPadding, rect.y + rect.height - 1, rect.x + rect.width - kRowPadding,
                    rect.y + rect.height - 1, true);

  const int pillTop = rect.y + (rect.height - CpyMetrics::values.batteryHeight) / 2;
  drawBatteryPill(renderer, right, pillTop, batteryPercentageVisible());
}
