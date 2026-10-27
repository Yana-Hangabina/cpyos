#pragma once

#include "components/themes/upstream/UpstreamStyleTheme.h"

// CPY ("脆皮鸭") theme: the Crispy Duck look frozen in
// docs/plans/2026-10-15-cpyos-ui-theme-design.md and
// docs/plans/2026-10-16-ui-interaction-spec.md.
//
// Design language recap:
//   - white pages, 1px hairline separators, solid hairlines only
//   - rounded cards/popups (r=20), capsule controls (r=255), sheets r=24
//   - inverted fill marks the current/selected item; the caption band keeps
//     a 4px black bar over the active main tab (never a full slot inversion)
//   - battery is a pill with the percentage inside, split-colour at the fill
//     boundary
namespace CpyMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics metrics = BaseMetrics::values;
  UiHighDpiProfile::apply(metrics);

  // CPY overrides run AFTER the high-DPI profile so the duck look is the same
  // shape on every board (the profile sizes the fonts, CPY sizes the shell).
  metrics.topPadding = 0;
  metrics.verticalSpacing = 8;

  // Sized so the three-digit worst case ("100") lives inside the cavity at
  // this profile's digit font: measured 46px for UI_10, cavity = width - 6.
  metrics.batteryWidth = UiHighDpiProfile::enabled ? 64 : 46;
  metrics.batteryHeight = 26;

  metrics.headerHeight = UiHighDpiProfile::enabled ? 112 : 84;
  metrics.batteryBarHeight = UiHighDpiProfile::enabled ? 48 : 36;
  metrics.headerSidePadding = 32;
  metrics.headerUnderlineSize = 0;  // the 1px hairline is drawn explicitly
  metrics.headerTitleAlign = 1;     // centered
  metrics.headerBatterySide = 0;
  metrics.headerShowsClock = false;  // the pill sits at the band's right edge
  metrics.headerBatteryDetached = false;

  metrics.contentSidePadding = 32;

  metrics.listRowHeight = UiHighDpiProfile::enabled ? 96 : 64;
  metrics.listWithSubtitleRowHeight = UiHighDpiProfile::enabled ? 128 : 80;
  metrics.listRowGap = 0;
  metrics.listRowRadius = 0;      // full-width hairline rows
  metrics.listInset = 0;
  metrics.listSidePadding = 32;
  metrics.listSelectionStyle = 0;  // InvertFill
  metrics.listScrollWidth = 6;
  metrics.listScrollSide = 0;
  metrics.listTitleBold = false;
  metrics.listSeparatorStyle = 1;  // Solid hairline
  metrics.listValueMaxWidth = 240;
  metrics.listSelectionCoversScrollReservation = true;

  metrics.menuRowHeight = UiHighDpiProfile::enabled ? 96 : 64;
  metrics.menuSpacing = 0;

  // Sub-tab strips (settings sort rows etc.), not the main bottom bar — the
  // main bar's height comes from mainTabBarHeight() above.
  metrics.tabBarHeight = UiHighDpiProfile::enabled ? 96 : 60;
  metrics.tabSpacing = 8;
  // refreshed UI mast: page label left, date/clock/battery right. 80 px replaces the
  // old 88 title + 48 status pair, giving the content the design's top edge.
  metrics.mainTabTitleHeight = UiHighDpiProfile::enabled ? 72 : 64;

  metrics.scrollBarWidth = 6;
  metrics.scrollBarRightOffset = 4;

  metrics.homeTopPadding = 56;
  metrics.homeCoverHeight = 226;
  metrics.homeCoverTileHeight = 242;
  metrics.homeRecentBooksCount = 1;
  metrics.homeShowRecentBookTitle = false;
  metrics.homeContinueReadingInMenu = false;
  metrics.homeMenuTopOffset = 16;

  metrics.buttonHintsHeight = 40;
  metrics.sideButtonHintsWidth = 30;

  metrics.keyboardKeyHeight = UiHighDpiProfile::enabled ? 88 : 56;
  metrics.keyboardKeySpacing = UiHighDpiProfile::enabled ? 7 : 0;
  metrics.keyboardCenteredText = true;
  metrics.keyboardVerticalOffset = UiHighDpiProfile::enabled ? 0 : -7;
  metrics.keyboardTextFieldWidthPercent = 92;
  metrics.keyboardWidthPercent = 96;

  metrics.popupTopOffsetRatio = 0.2f;
  metrics.popupMarginX = 48;
  metrics.popupMarginY = 24;
  metrics.popupFrameThickness = 2;
  metrics.popupCornerRadius = 20;
  metrics.popupTextBold = false;
  metrics.popupTextInverted = false;
  metrics.popupTextBaselineOffsetY = -2;
  metrics.popupProgressBarHeight = 8;
  metrics.popupProgressDrawOutline = true;
  metrics.popupProgressClampPercent = false;
  metrics.popupProgressFillInverted = false;
  metrics.popupProgressOutlineInverted = false;

  metrics.optionPopupItemSpacing = 0;
  metrics.optionPopupInnerPadding = 28;
  metrics.optionPopupSelectionHPadding = 20;
  metrics.optionPopupSelectionVPadding = 16;
  metrics.optionPopupTitleGap = 16;
  metrics.optionPopupUseSmallFont = false;
  metrics.optionPopupOptionFontBold = false;
  metrics.optionPopupSelectionRadius = 16;
  metrics.optionPopupSelectionLight = false;  // selected row = black fill
  metrics.optionPopupDrawAllRows = false;
  metrics.optionPopupDialogSideMargin = 40;
  metrics.optionPopupTitleSeparator = false;  // no dividers inside the card

  metrics.textFieldHorizontalPadding = 20;
  metrics.textFieldNormalThickness = 2;
  metrics.textFieldCursorThickness = 3;
  metrics.textFieldLineEndOffset = 0;

  metrics.controlRadius = 16;
  metrics.sheetRadius = 24;
  metrics.capsuleRadius = 255;

  metrics.progressBarHeight = 16;
  metrics.progressBarMarginTop = 1;
  metrics.statusBarHorizontalMargin = 32;
  metrics.statusBarVerticalMargin = UiHighDpiProfile::enabled ? 48 : 36;
  return metrics;
}
inline constexpr ThemeMetrics values = makeValues();
}  // namespace CpyMetrics

class CpyTheme final : public UpstreamStyleTheme {
 public:
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                  bool backButton = true) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  bool tabIndexFromPoint(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, int x, int y,
                         int& index) const override;
  int getListRowStep(bool hasSubtitle) const override;
  int getListPageItems(int contentHeight, bool hasSubtitle) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr,
                bool showSelection = true, const std::function<bool(int index)>& rowHeading = nullptr) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon, int rowSpacing = -1) const override;
  MenuRowGeometry getMenuRowGeometry(const GfxRenderer& renderer, const Rect& rect, int selectedIndex,
                                     int rowCount) const override;
  void drawOptionPopup(const GfxRenderer& renderer, const char* title, const char* headline, int headlineHeight,
                       const std::vector<std::string>& options, int selectedIndex) const override;
  void drawMainTabBar(const GfxRenderer& renderer, Rect rect, MainTab selected) const override;
  void drawMainTabStatusBar(const GfxRenderer& renderer, Rect rect) const override;
  void drawMainTabTitle(const GfxRenderer& renderer, Rect rect, const char* title, bool masthead = false) const override;
  // The home masthead strip merged into the status bar row (wordmark + date
  // ride beside the clock and battery), so no extra band height is reserved
  // (device feedback: the separate strip wasted vertical space).
  int mainTabMastheadHeight() const override { return 0; }
  // 126: the 56px tab glyphs plus the label line fill a 96px bar to the last
  // pixel and the labels ran into the panel's bottom edge (three device
  // reports). The icons keep their 56px bitmaps; the extra height is clearance
  // above and below the icon+label stack.
  int mainTabBarHeight() const override { return UiHighDpiProfile::enabled ? 88 : 60; }
  // Empty state: the duck pattern tiles the backdrop and the message sits on
  // a white strip so it stays readable over the pattern.
  void drawEmptyState(const GfxRenderer& renderer, Rect rect, const char* message) const override;
  void drawModalScrim(const GfxRenderer& renderer) const override;
  void drawCardFrame(const GfxRenderer& renderer, Rect rect, bool focused) const override;
  // Transient messages (loading / saved / failed) used to be a solid-black
  // strip with white text -- the only remaining piece of the pre-CPY visual
  // language, repeated at 30+ call sites. One card with the 25 % ring and
  // black text gives every one of them the dialog/tile language. No mascot:
  // the frozen duck list does not include transient messages.
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
  // The mascot's upright pose, for pages that show it inline (the JJWXC account
  // card). Same 36px bits the title band uses; lives here so consumers do not
  // pull a second copy of the asset table into their own translation unit.
  static void drawDuckStand(const GfxRenderer& renderer, int x, int y);
  // The 48px swimming pose, for waiting/job cards that should carry the same
  // mascot language as the title band and the progress bar.
  static void drawDuckSwim(const GfxRenderer& renderer, int x, int y);
  // Boot / sleep splash: the big swimming duck on a dithered water band with
  // the CPY wordmark and version.
  void drawSplash(const GfxRenderer& renderer, const char* status, const char* version = nullptr) const override;
  // Sleep: hand-drawn wall clock (sloppy digits, same family as the standby
  // faces) over the sleeping duck on its water band. SleepActivity inverts the
  // frame for the DARK mode, giving the night version for free.
  void drawSleepScreen(const GfxRenderer& renderer, const char* status, const char* bookTitle = nullptr,
                       int readPercent = 0) const override;
  // Water-wave progress bar: the line is a sine wave, the filled part sits in
  // dithered water, and the duck swims at the fill edge.
  int drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total,
                      bool showPercentage = true) const override;
  // CpyMetrics bakes UiHighDpiProfile and then layers post-profile overrides
  // (tab bar height, battery pill, keyboard key height, menu rows): the
  // profile must not be re-applied on top of them.
  bool metricsIncludeProfile() const override { return true; }
  bool supportsCompactDensity() const override { return true; }
  void applyCompactDensity(ThemeMetrics& metrics) const override;

  // The battery pill, shared by the subpage header and the main-tab status
  // band. Percentage sits INSIDE the pill, split-colour at the fill edge; when
  // the fill is too narrow to hold the text it moves to the pill's right.
  static void drawBatteryPill(const GfxRenderer& renderer, int right, int top, bool showPercent);

  // Reader status bar: use the same capsule as the header chrome, so the number
  // sits inside it there too. The rect's width/height are the pill metrics.
  int drawStatusBarBattery(const GfxRenderer& renderer, int x, int y, bool showPercent) const override;

 private:
  static Rect tabChipRect(const GfxRenderer& renderer, const Rect& band, const std::vector<TabInfo>& tabs, int index);
};
