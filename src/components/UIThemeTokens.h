#pragma once
#include <BoardConfig.h>
#include <FreeInkUIGfxRenderer.h>

#include "SelectionCursorPolicy.h"
#include "UITheme.h"
#include "themes/upstream/UpstreamStyleTheme.h"

inline const ThemeMetrics& uiThemeMetrics(bool upstreamStyle = false) {
  // The upstream-style metrics table only ever served the INX theme; with it
  // retired the live theme's metrics are the one answer.
  (void)upstreamStyle;
  return UITheme::getInstance().getMetrics();
}

namespace ui_theme_detail {
template <typename Profile>
int16_t scrollInset(const Profile& profile, const uint8_t side) {
  if constexpr (requires { profile.viewableInsets; }) {
    return static_cast<int16_t>(side == 1 ? profile.viewableInsets.left : profile.viewableInsets.right);
  }
  return 0;
}
}  // namespace ui_theme_detail

// Keep INX's original advance-based alignment, including one-digit controls.
inline void applyUiTextAlignment(freeink::ui::GfxRendererTarget& target, bool upstreamStyle = false) {
#ifdef FREEINK_UI_THEME_LAYOUT_POLICY
  (void)upstreamStyle;
  target.setTextCentering(freeink::ui::TextCentering::InkBounds);
#else
  (void)target;
#endif
}

// Merges the active UITheme's shape (row gaps, radii, insets, selection
// style) with the uiScale-derived sizes into FreeInkUI theme tokens: the
// theme says what lists look like, the scale says how big they are.
// Everything read here is plain data from ThemeMetrics — the same values an
// SD-card theme file will eventually supply.
inline freeink::ui::ThemeTokens uiThemeTokens(const freeink::ui::GfxRendererTarget& target,
                                              bool upstreamStyle = false) {
  namespace fui = freeink::ui;
  const ThemeMetrics& metrics = uiThemeMetrics(upstreamStyle);
  const bool modernTheme = !upstreamStyle && UITheme::getInstance().usesModernLayout();

  fui::ThemeTokens tokens = fui::themeTokensForLineHeight(target.lineHeight(fui::GfxRendererTarget::FONT_BODY));
  if (!modernTheme && !BoardConfig::hasTouch()) tokens.listMinRowHeight = static_cast<int16_t>(metrics.listRowHeight);
#ifdef FREEINK_UI_THEME_LAYOUT_POLICY
  if (modernTheme) {
    tokens.listLayoutPolicy = fui::ListLayoutPolicy::ThemeRow;
  }
#endif
  tokens.listRowGap = static_cast<int16_t>(metrics.listRowGap);
  tokens.listRowRadius = static_cast<uint8_t>(metrics.listRowRadius);
  tokens.listInset = static_cast<int16_t>(metrics.listInset);
  tokens.listSidePadding = static_cast<int16_t>(metrics.listSidePadding);
  tokens.listSelectionStyle = static_cast<fui::SelectionStyle>(metrics.listSelectionStyle);
  tokens.listScrollWidth = static_cast<int16_t>(metrics.listScrollWidth);
  tokens.listScrollSide = static_cast<uint8_t>(metrics.listScrollSide);
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO
  // These upstream board profiles already apply their bezel inset through the
  // FreeInkUI safe area, so do not compensate the scroll track a second time.
  tokens.listScrollInset = 0;
#else
  // The scroll track hugs the band edge; on boards whose panel sits recessed
  // behind the bezel the edge columns are covered, so push the indicator
  // inward past the covered side. Bezel truth is per-board data
  // (BoardConfig::ViewableInsets); lists render in the portrait UI frame, so
  // the panel-native portrait insets apply directly.
  tokens.listScrollInset = modernTheme ? ui_theme_detail::scrollInset(BoardConfig::ACTIVE, metrics.listScrollSide) : 0;
#endif
  // Screen::header()/status() band height. Without this the SDK's
  // line-height-derived default applies and fui-drawn headers (OPDS) come out
  // a different height than every GUI.drawHeader band.
  tokens.headerHeight = static_cast<int16_t>(metrics.headerHeight);
  tokens.headerSidePadding = static_cast<int16_t>(metrics.headerSidePadding);
  tokens.headerUnderline = static_cast<uint8_t>(metrics.headerUnderlineSize);
  tokens.headerTitleAlign = static_cast<fui::TextAlign>(metrics.headerTitleAlign);
  // Control-panel shape (sheet corners, tiles, step buttons, capsule slider),
  // so the control center follows the theme like every list and header does.
  tokens.controlRadius = static_cast<uint8_t>(metrics.controlRadius);
  tokens.sheetRadius = static_cast<uint8_t>(metrics.sheetRadius);
  tokens.capsuleRadius = static_cast<uint8_t>(metrics.capsuleRadius);
  tokens.bodyText.bold = metrics.listTitleBold;
  if (modernTheme) {
    tokens.listSeparator = static_cast<fui::SeparatorStyle>(metrics.listSeparatorStyle);
    tokens.listValueMaxWidth = static_cast<int16_t>(metrics.listValueMaxWidth);
    tokens.listSelectionCoversScrollReservation = metrics.listSelectionCoversScrollReservation;
    tokens.listEmphasizedText = tokens.titleText;
    tokens.listEmphasizedText.bold = true;
    tokens.listValueText = tokens.bodyText;
  }
  if (UiHighDpiProfile::enabled) {
    // Compact density (CPY): the SDK row tokens follow the theme's density
    // metric so the setting reaches fui-drawn lists (settings etc.) too;
    // comfort keeps the profile's shipped 104px rhythm bit-for-bit.
    const bool compactDensity =
        UITheme::getInstance().getTheme().supportsCompactDensity() && SETTINGS.uiDensity != 0;
    // CPY (density-capable themes) take the row height from their metric in
    // both densities — the theme's own 96px list rhythm was being masked by
    // the profile's 104 here, which is also why rows read chunkier than the
    // frames. Other themes keep the profile value.
    const int16_t rowHeight =
        compactDensity
            ? static_cast<int16_t>(metrics.listRowHeight)
            : (UITheme::getInstance().getTheme().supportsCompactDensity()
                   ? static_cast<int16_t>(metrics.listRowHeight)
                   : UiHighDpiProfile::rowHeight);
    tokens.minTouchSize = UiHighDpiProfile::buttonHeight;
    tokens.rowHeight = rowHeight;
    tokens.listMinRowHeight = rowHeight;
    tokens.listTouchMinRowHeight = rowHeight;
    tokens.listTouchRowPaddingY = 16;
    tokens.listTouchRowGap = UiHighDpiProfile::controlGap;
    tokens.spaceSm = 8;
    tokens.spaceMd = UiHighDpiProfile::controlGap;
    tokens.spaceLg = 24;
  }
  if (!UITheme::getInstance().showSelectionCursor()) SelectionCursorPolicy::hideFreeInkListFocus(tokens);
  return tokens;
}
