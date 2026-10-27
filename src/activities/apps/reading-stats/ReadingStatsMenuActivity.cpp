#include "ReadingStatsMenuActivity.h"

#include <I18n.h>

#include <algorithm>
#include <memory>
#include <string>

#include "AchievementsActivity.h"
#include "AchievementsStore.h"
#include "AppMetricCard.h"
#include "CrossPointSettings.h"
#include "ReadingHeatmapActivity.h"
#include "ReadingProfileActivity.h"
#include "ReadingStatsActivity.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "components/icons/listIcons48.h"
#include "util/ReadingStatsAnalytics.h"

namespace {

// Screens listed in the Reading Stats sub-menu. Achievements is kept LAST so it
// can be hidden by simply trimming the active count when the setting is off.
enum class StatsScreen { Stats, Heatmap, Profile, Achievements };

struct MenuEntry {
  StrId titleId;
  StatsScreen screen;
  UIIcon icon;
};

constexpr MenuEntry kEntries[] = {
    {StrId::STR_READING_STATS, StatsScreen::Stats, UIIcon::ReadingStats},
    {StrId::STR_READING_HEATMAP, StatsScreen::Heatmap, UIIcon::ReadingHeatmap},
    {StrId::STR_READING_PROFILE, StatsScreen::Profile, UIIcon::ReadingProfile},
    {StrId::STR_ACHIEVEMENTS, StatsScreen::Achievements, UIIcon::Achievements},
};

constexpr int kEntryCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

void drawStatsIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const int size) {
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

// Achievements (the last row) is only listed when enabled in settings.
int activeEntryCount() { return SETTINGS.achievementsEnabled ? kEntryCount : kEntryCount - 1; }

}  // namespace

void ReadingStatsMenuActivity::onEnter() {
  Activity::onEnter();
  selected = 0;
  requestUpdate();
}

void ReadingStatsMenuActivity::onExit() { Activity::onExit(); }

void ReadingStatsMenuActivity::openSelected() {
  if (selected < 0 || selected >= activeEntryCount()) {
    return;
  }
  // Leaves call finish() on Back, so pushing them returns control here.
  switch (kEntries[selected].screen) {
    case StatsScreen::Stats:
      startActivityForResultWith<ReadingStatsActivity>([](const ActivityResult&) {});
      break;
    case StatsScreen::Heatmap:
      startActivityForResultWith<ReadingHeatmapActivity>([](const ActivityResult&) {});
      break;
    case StatsScreen::Profile:
      startActivityForResultWith<ReadingProfileActivity>([](const ActivityResult&) {});
      break;
    case StatsScreen::Achievements:
      startActivityForResultWith<AchievementsActivity>([](const ActivityResult&) {});
      break;
  }
}

void ReadingStatsMenuActivity::loop() {
  const int count = activeEntryCount();
  Rect cards[kEntryCount];
  layoutCards(cards, count);

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTouchDown(x, y)) {
    const int touched = cardAt(x, y, cards, count);
    if (touched >= 0 && touched != selected) {
      selected = touched;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(x, y)) {
    const int touched = cardAt(x, y, cards, count);
    if (touched >= 0) {
      selected = touched;
      openSelected();
    }
    return;
  }

  buttonNavigator.onNext([this, count] {
    selected = ButtonNavigator::nextIndex(selected, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selected = ButtonNavigator::previousIndex(selected, count);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Per the apps convention, a sub-app's Back returns to the Apps menu.
    activityManager.goToApps();
  }
}

void ReadingStatsMenuActivity::layoutCards(Rect out[kEntryCount], const int count) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int top = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = safe.y + safe.height - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int margin = UiHighDpiProfile::enabled ? UiHighDpiProfile::contentPadding : 20;
  const int w = std::max(1, safe.width - margin * 2);
  const int gap = 24;
  const int cw = (w - gap) / 2;
  const int avail = std::max(1, bottom - top - 8);
  const int ch = std::min(400, (avail - gap) / 2);
  const int left = safe.x + margin;
  for (int i = 0; i < kEntryCount; ++i) {
    out[i] = Rect{left + (i % 2) * (cw + gap), top + 8 + (i / 2) * (ch + gap), cw, ch};
  }
  (void)count;
}

int ReadingStatsMenuActivity::cardAt(const int x, const int y, const Rect* cards, const int count) const {
  for (int i = 0; i < count; ++i) {
    const Rect& r = cards[i];
    if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) return i;
  }
  return -1;
}

void ReadingStatsMenuActivity::drawCard(const Rect& rect, const int index, const bool selected) const {
  GUI.drawCardFrame(renderer, rect, selected);
  const freeink::Icon* icons[kEntryCount] = {&icon_chart_line_48, &icon_layout_grid_48, &icon_book_48,
                                             &icon_trophy_48};
  const int iconSize = 40;
  drawStatsIcon(renderer, *icons[index], rect.x + 20, rect.y + 18, iconSize);
  const int titleY = rect.y + 26;
  renderer.drawText(UI_12_FONT_ID, rect.x + 20 + iconSize + 12, titleY, I18N.get(kEntries[index].titleId), true,
                    EpdFontFamily::BOLD);
  const int chevW = renderer.getTextWidth(UI_10_FONT_ID, ">");
  renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - 20 - chevW, titleY, ">");

  std::string value;
  std::string label;
  std::string foot;
  switch (kEntries[index].screen) {
    case StatsScreen::Stats:
      value = ReadingStatsAnalytics::formatDurationHm(READING_STATS.getRecentReadingMs(7));
      label = tr(STR_DASH_WEEK);
      {
        char today[32];
        snprintf(today, sizeof(today), "%s %llum", tr(STR_DASH_TODAY),
                 static_cast<unsigned long long>(READING_STATS.getTodayReadingMs() / 60000));
        foot = today;
      }
      break;
    case StatsScreen::Heatmap: {
      uint32_t ref = 0;
      for (const auto& day : READING_STATS.getReadingDays()) ref = std::max(ref, day.dayOrdinal);
      uint32_t days = 0;
      for (const auto& day : READING_STATS.getReadingDays()) {
        if (day.readingMs > 0 && day.dayOrdinal <= ref && ref - day.dayOrdinal < 30) ++days;
      }
      char buf[16];
      snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(days));
      value = buf;
      label = tr(STR_DAYS_READ);
      foot = tr(STR_READING_HEATMAP);
      break;
    }
    case StatsScreen::Profile: {
      char buf[32];
      snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(READING_STATS.getBooksFinishedCount()));
      value = buf;
      label = tr(STR_BOOKS_FINISHED);
      const uint32_t started = READING_STATS.getBooksStartedCount();
      const uint32_t finished = READING_STATS.getBooksFinishedCount();
      char footBuf[48];
      snprintf(footBuf, sizeof(footBuf), "%s %lu", tr(STR_BOOKS_STARTED),
               static_cast<unsigned long>(started > finished ? started - finished : 0));
      foot = footBuf;
      break;
    }
    case StatsScreen::Achievements: {
      uint32_t unlocked = 0;
      for (const auto& view : ACHIEVEMENTS.buildViews()) {
        if (view.state.unlocked) ++unlocked;
      }
      char buf[16];
      snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(unlocked));
      value = buf;
      label = tr(STR_ACHIEVEMENTS);
      foot.clear();
      break;
    }
  }
  const int valueY = rect.y + rect.height / 2 - 20;
  if (!value.empty()) renderer.drawText(UI_12_FONT_ID, rect.x + 20, valueY, value.c_str(), true, EpdFontFamily::BOLD);
  if (!label.empty()) renderer.drawText(UI_10_FONT_ID, rect.x + 20, valueY + 42, label.c_str());
  if (!foot.empty()) renderer.drawText(UI_10_FONT_ID, rect.x + 20, rect.y + rect.height - 44, foot.c_str());
}

void ReadingStatsMenuActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_READING_STATS));

  Rect cards[kEntryCount];
  layoutCards(cards, activeEntryCount());
  for (int i = 0; i < activeEntryCount(); ++i) drawCard(cards[i], i, selected == i);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
