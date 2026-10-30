#include "MyActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "activities/ActivityManager.h"
#include "activities/apps/reading-stats/ReadingDayDetailActivity.h"
#include "activities/apps/reading-stats/ReadingHeatmapActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/AboutActivity.h"
#include "activities/settings/BluetoothSettingsActivity.h"
#include "activities/settings/ScreenRepairActivity.h"
#include "activities/settings/SettingsSearchActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "components/UITheme.h"
#include "components/UiHighDpiProfile.h"
#include "components/icons/inx_apps.h"
#include "components/icons/listIcons48.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"

namespace {
constexpr int kMargin = 40;
constexpr int kTabH = 80;
constexpr int kQuickH = 64;
constexpr int kCardW = 292;
constexpr int kCardH = 296;
constexpr int kCardGapX = 20;
constexpr int kCardGapY = 20;
constexpr int kCardHeader = 56;
constexpr int kCardRow = 80;
constexpr int kToolW = 139;
constexpr int kToolH = 120;
constexpr int kToolGap = 16;
constexpr int kGroupHeader = 48;

void drawMaskIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const int size) {
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

void drawChevron(const GfxRenderer& renderer, const int x, const int y) {
  renderer.drawLine(x, y, x + 6, y + 6, true);
  renderer.drawLine(x + 6, y + 6, x, y + 12, true);
}

std::string gigabytes(const uint64_t bytes) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(bytes) / 1e9);
  return buf;
}

std::string compactDuration(const uint64_t ms) {
  const uint64_t minutes = ms / 60000;
  char buf[24];
  if (minutes >= 60) {
    std::snprintf(buf, sizeof(buf), "%lluh%02llu", static_cast<unsigned long long>(minutes / 60),
                  static_cast<unsigned long long>(minutes % 60));
  } else {
    std::snprintf(buf, sizeof(buf), "%llum", static_cast<unsigned long long>(minutes));
  }
  return buf;
}

const char* gestureLabel() {
  const uint8_t g = SETTINGS.pageTurnGesture;
  if (g == CrossPointSettings::SWIPE_ONLY) return tr(STR_SWIPE_ONLY);
  if (g == CrossPointSettings::TAP_ONLY) return tr(STR_TAP_ONLY);
  return tr(STR_TAP_AND_SWIPE);
}

const char* densityLabel() {
  return SETTINGS.uiDensity == 0 ? tr(STR_DENSITY_COMFORTABLE) : tr(STR_DENSITY_COMPACT);
}

const char* motionLabel() {
  switch (SETTINGS.motionPageTurn) {
    case CrossPointSettings::MOTION_TILT:
      return tr(STR_MOTION_TILT);
    case CrossPointSettings::MOTION_TILT_REVERSED:
      return tr(STR_MOTION_TILT_REVERSED);
    case CrossPointSettings::MOTION_SHAKE:
      return tr(STR_MOTION_SHAKE);
    case CrossPointSettings::MOTION_SHAKE_REVERSED:
      return tr(STR_MOTION_SHAKE_REVERSED);
    default:
      return tr(STR_STATE_OFF);
  }
}

std::string wifiValue() {
  if (WiFi.status() == WL_CONNECTED) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), I18N.get(StrId::STR_WIFI_CONNECTED_FMT), WiFi.SSID().c_str());
    return buf;
  }
  return tr(STR_WIFI_OFF);
}

uint32_t dayOrdinalFor(const time_t now) { return TimeUtils::getLocalDayOrdinal(static_cast<uint32_t>(now)); }

int weekdayOf(const uint32_t ordinal) { return static_cast<int>((ordinal + 4U) % 7U); }  // 0 = Sunday

bool validClock() { return time(nullptr) > 1700000000; }

uint32_t ordinalDaysInMonth(const uint32_t firstOrdinal) {
  static constexpr uint8_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (!TimeUtils::getDateFromDayOrdinal(firstOrdinal, year, month, day) || month < 1 || month > 12) return 30;
  int days = kDays[month - 1];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++days;
  return static_cast<uint32_t>(days);
}

enum class ToolAction : uint8_t {
  ReadingStats,
  FileTransfer,
  JoinNetwork,
  FileBrowser,
  Woodfish,
  Game2048,
  Sudoku,
  Sokoban,
  Chess,
  Minesweeper,
  Avatar,
  Standby,
};

struct ToolEntry {
  StrId title;
  UIIcon icon;
  ToolAction action;
};

const ToolEntry kReadingTools[] = {
    {StrId::STR_READING_STATS, UIIcon::ReadingStats, ToolAction::ReadingStats},
    {StrId::STR_FILE_TRANSFER, UIIcon::Transfer, ToolAction::FileTransfer},
    {StrId::STR_JOIN_NETWORK, UIIcon::Wifi, ToolAction::JoinNetwork},
    {StrId::STR_BROWSE_FILES, UIIcon::File, ToolAction::FileBrowser},
};

const ToolEntry kBreakTools[] = {
    {StrId::STR_WOODFISH_TITLE, UIIcon::Woodfish, ToolAction::Woodfish},
    {StrId::STR_2048_TITLE, UIIcon::Game2048, ToolAction::Game2048},
    {StrId::STR_SUDOKU_TITLE, UIIcon::Sudoku, ToolAction::Sudoku},
    {StrId::STR_SOKOBAN_TITLE, UIIcon::Sokoban, ToolAction::Sokoban},
};

const ToolEntry kMoreTools[] = {
#ifdef ENABLE_CHINESE_VERSION
    {StrId::STR_CHINESE_CHESS_TITLE, UIIcon::ChineseChess, ToolAction::Chess},
#else
    {StrId::STR_MINESWEEPER_TITLE, UIIcon::Minesweeper, ToolAction::Minesweeper},
#endif
    {StrId::STR_MINESWEEPER_TITLE, UIIcon::Minesweeper, ToolAction::Minesweeper},
    {StrId::STR_UGLY_AVATAR, UIIcon::Avatar, ToolAction::Avatar},
    {StrId::STR_STANDBY_TITLE, UIIcon::Standby, ToolAction::Standby},
};

const ToolEntry* toolAt(const int index) {
  if (index < 0) return nullptr;
  if (index < 4) return &kReadingTools[index];
  if (index < 8) return &kBreakTools[index - 4];
  if (index < 12) return &kMoreTools[index - 8];
  return nullptr;
}
}  // namespace

void MyActivity::onEnter() {
  Activity::onEnter();
  toolSelected_ = 0;
  storageKnown_ = HalStorage::getInstance().getSpace(storageTotalBytes_, storageFreeBytes_);
  requestUpdate();
}

Rect MyActivity::tabRect(const int index) const {
  const Rect content = pageContentRect();
  return Rect{content.x + kMargin + index * 120, content.y, 110, kTabH};
}

Rect MyActivity::bodyRect() const {
  const Rect content = pageContentRect();
  return Rect{content.x, content.y + kTabH, content.width, std::max(0, content.height - kTabH)};
}

Rect MyActivity::cardRect(const int index) const {
  const Rect body = bodyRect();
  // Quick pills (14), search row (52), then 18 px of air before the cards.
  // The old top left only 4 px between the search field and the cards.
  const int top = body.y + kQuickH + 20 + 52 + 18;
  return Rect{body.x + kMargin + (index % 2) * (kCardW + kCardGapX),
              top + (index / 2) * (kCardH + kCardGapY), kCardW, kCardH};
}

Rect MyActivity::cardRowRect(const int card, const int row) const {
  const Rect r = cardRect(card);
  return Rect{r.x, r.y + kCardHeader + row * kCardRow, r.width, kCardRow};
}

Rect MyActivity::quickPillRect(const int index) const {
  const Rect body = bodyRect();
  constexpr int widths[3] = {220, 120, 220};
  int x = body.x + kMargin;
  for (int i = 0; i < index; ++i) x += widths[i] + 10;
  return Rect{x, body.y + 14, widths[index], 44};
}

Rect MyActivity::searchRowRect() const {
  const Rect body = bodyRect();
  return Rect{body.x + kMargin, body.y + kQuickH + 20, body.width - kMargin * 2, 52};
}

Rect MyActivity::aboutRect() const {
  const Rect body = bodyRect();
  const Rect last = cardRect(3);
  return Rect{body.x + kMargin, last.y + kCardH + 16, body.width - kMargin * 2, 64};
}

Rect MyActivity::toolCellRect(const int index) const {
  const Rect body = bodyRect();
  const int group = index / 4;
  const int slot = index % 4;
  const int top = body.y + 12 + group * (kGroupHeader + kToolH + 22);
  return Rect{body.x + kMargin + slot * (kToolW + kToolGap), top + kGroupHeader, kToolW, kToolH};
}

Rect MyActivity::toolPluginRect() const {
  const Rect body = bodyRect();
  return Rect{body.x + kMargin, body.y + 12 + 3 * (kGroupHeader + kToolH + 22) + 4,
              body.width - kMargin * 2, 88};
}

void MyActivity::drawTabs() {
  const Rect content = pageContentRect();
  renderer.drawLine(content.x + kMargin, content.y + kTabH - 1, content.x + content.width - kMargin,
                    content.y + kTabH - 1, true);
  const char* labels[kTabCount] = {I18N.get(StrId::STR_MY_TAB_RECORDS), I18N.get(StrId::STR_MY_TAB_TOOLS),
                                   I18N.get(StrId::STR_SETTINGS_TITLE)};
  for (int i = 0; i < kTabCount; ++i) {
    const Rect r = tabRect(i);
    const bool on = static_cast<int>(tab_) == i;
    renderer.drawText(UI_12_FONT_ID, r.x, r.y + 26, labels[i], true,
                      on ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (on) {
      const int w = renderer.getTextWidth(UI_12_FONT_ID, labels[i], EpdFontFamily::BOLD);
      // Air under the glyphs and above the strip rule: drawing the bar at
      // kTabH - 5 made it merge with the rule into one thick line (device
      // feedback 2026-10-26).
      renderer.fillRoundedRect(r.x, r.y + kTabH - 18, w, 4, 2, Color::Black);
    }
  }
}

void MyActivity::drawWaveLine(const int x, const int y, const int width) {
  constexpr float kTwoPi = 6.2831853f;
  for (int px = 0; px + 1 < width; ++px) {
    const int y0 = y + static_cast<int>(std::lround(2.0f * std::sin(static_cast<float>(px) * kTwoPi / 26.0f)));
    const int y1 =
        y + static_cast<int>(std::lround(2.0f * std::sin(static_cast<float>(px + 1) * kTwoPi / 26.0f)));
    renderer.drawLine(x + px, y0, x + px + 1, y1, true);
  }
}

void MyActivity::drawRipples(const int x, const int y, const int maxRadius) {
  for (int radius = maxRadius / 3; radius <= maxRadius; radius += maxRadius / 3) {
    renderer.drawArc(radius, x, y, -1, -1, 1, true);
  }
}

// ---------------------------------------------------------------- Records --
void MyActivity::drawRecords() {
  const Rect body = bodyRect();
  const int x = body.x + kMargin;
  const int w = body.x + body.width - kMargin - x;
  const uint64_t weekMs = READING_STATS.getRecentReadingMs(7);
  const uint64_t prevMs = READING_STATS.getRecentReadingMs(14) - weekMs;
  const time_t now = time(nullptr);
  const bool clock = validClock();
  const uint32_t today = clock ? dayOrdinalFor(now) : 0;

  renderer.drawText(UI_10_FONT_ID, x, body.y + 28, tr(STR_READING_WEEKLY), true, EpdFontFamily::BOLD);
  if (clock) {
    struct tm local {};
    localtime_r(&now, &local);
    const int mondayOffset = (weekdayOf(today) + 6) % 7;
    const uint32_t monday = today - static_cast<uint32_t>(mondayOffset);
    int y1 = 0, y2 = 0;
    unsigned m1 = 0, m2 = 0, d1 = 0, d2 = 0;
    char range[32] = {};
    if (TimeUtils::getDateFromDayOrdinal(monday, y1, m1, d1) &&
        TimeUtils::getDateFromDayOrdinal(today, y2, m2, d2)) {
      std::snprintf(range, sizeof(range), "%u.%u – %u.%u", m1, d1, m2, d2);
      const int rw = renderer.getTextWidth(UI_10_FONT_ID, range);
      renderer.drawText(UI_10_FONT_ID, x + w - rw, body.y + 28, range);
    }
  }

  const std::string big = compactDuration(weekMs);
  renderer.drawText(NOTOSERIF_TITLE_FONT_ID, x, body.y + 52, big.c_str(), true, EpdFontFamily::BOLD);
  const int bigW = renderer.getTextWidth(NOTOSERIF_TITLE_FONT_ID, big.c_str(), EpdFontFamily::BOLD);
  if (prevMs > 0) {
    const int delta = static_cast<int>(static_cast<int64_t>(weekMs - prevMs) * 100 / static_cast<int64_t>(prevMs));
    char deltaText[32];
    std::snprintf(deltaText, sizeof(deltaText), "%+d%%", delta);
    char vs[64];
    std::snprintf(vs, sizeof(vs), I18N.get(StrId::STR_VS_LAST_WEEK_FMT), deltaText);
    renderer.drawText(UI_10_FONT_ID, x + bigW + 18, body.y + 82, vs);
  }
  drawWaveLine(x, body.y + 122, w);

  // Seven day columns.
  const int barsTop = body.y + 150;
  const int baseline = barsTop + 104;
  const int colW = w / 7;
  std::array<uint64_t, 7> weekMsDays{};
  uint64_t maxMs = 1;
  if (clock) {
    for (const auto& day : READING_STATS.getReadingDays()) {
      if (day.dayOrdinal <= today && today - day.dayOrdinal < 7) {
        weekMsDays[today - day.dayOrdinal] += day.readingMs;
      }
    }
    for (const uint64_t ms : weekMsDays) maxMs = std::max(maxMs, ms);
  }
  for (int i = 0; i < 7; ++i) {
    const size_t daysAgo = 6 - static_cast<size_t>(i);
    const uint64_t ms = clock ? weekMsDays[daysAgo] : 0;
    const int h = ms == 0 ? 4 : 4 + static_cast<int>(ms * 96 / maxMs);
    const int bx = x + i * colW + (colW - 44) / 2;
    if (i == 6) {
      renderer.fillRoundedRect(bx, baseline - h, 44, h, 4, Color::Black);
    } else {
      renderer.fillRoundedRect(bx, baseline - h, 44, h, 4, Color::LightGray);
    }
    if (clock) {
      const uint32_t ordinal = today - static_cast<uint32_t>(daysAgo);
      int year = 0;
      unsigned month = 0;
      unsigned dom = 0;
      if (TimeUtils::getDateFromDayOrdinal(ordinal, year, month, dom)) {
        char label[8];
        std::snprintf(label, sizeof(label), "%u", dom);
        const int lw = renderer.getTextWidth(UI_10_FONT_ID, label);
        renderer.drawText(UI_10_FONT_ID, bx + 22 - lw / 2, baseline + 8, label);
      }
    }
  }

  // Month heatmap: 7 rows (Sun..Sat) x week columns, 20 px cells.
  const int hmTop = body.y + 300;
  renderer.drawText(UI_10_FONT_ID, x, hmTop, tr(STR_HEATMAP_MONTH), true, EpdFontFamily::BOLD);
  {
    const char* less = tr(STR_LESS);
    const char* more = tr(STR_MORE);
    const int moreW = renderer.getTextWidth(UI_10_FONT_ID, more);
    const int lessW = renderer.getTextWidth(UI_10_FONT_ID, less);
    const int legendW = lessW + 8 + 3 * 16 + 2 * 6 + 8 + moreW;
    int lx = x + w - legendW;
    renderer.drawText(UI_10_FONT_ID, lx, hmTop, less);
    lx += lessW + 8;
    for (int i = 0; i < 3; ++i) {
      renderer.fillRect(lx, hmTop + 2, 16, 16, i == 1 ? true : false);
      if (i == 1) renderer.fillRect(lx, hmTop + 2, 16, 16, true);
      if (i == 0) renderer.fillRect(lx + 2, hmTop + 4, 12, 12, false);
      lx += 22;
    }
    renderer.drawText(UI_10_FONT_ID, lx + 2, hmTop, more);
  }

  if (clock) {
    int year = 0;
    unsigned month = 0, dom = 0;
    TimeUtils::getDateFromDayOrdinal(today, year, month, dom);
    const uint32_t first = today - (dom - 1U);
    const uint32_t days = ordinalDaysInMonth(first);
    const int firstWeekday = weekdayOf(first);
    const int columns = (firstWeekday + static_cast<int>(days) + 6) / 7;
    const int gridW = columns * 20 + (columns - 1) * 4;
    const int gx = x + (w - gridW) / 2;
    const int gy = hmTop + 34;
    for (uint32_t d = 0; d < days; ++d) {
      const uint32_t ordinal = first + d;
      const int slot = firstWeekday + static_cast<int>(d);
      const int col = slot / 7;
      const int row = slot % 7;
      int year2 = 0;
      unsigned month2 = 0, dom2 = 0;
      TimeUtils::getDateFromDayOrdinal(ordinal, year2, month2, dom2);
      uint64_t minutes = 0;
      for (const auto& day : READING_STATS.getReadingDays()) {
        if (day.dayOrdinal == ordinal) {
          minutes = day.readingMs / 60000;
          break;
        }
      }
      const int cx = gx + col * 24;
      const int cy = gy + row * 24;
      if (minutes == 0) {
        renderer.drawRoundedRect(cx, cy, 20, 20, 2, 2, true);
      } else if (minutes < 15) {
        renderer.fillRoundedRect(cx, cy, 20, 20, 2, Color::LightGray);
      } else if (minutes < 45) {
        renderer.fillRoundedRect(cx, cy, 20, 20, 2, Color::DarkGray);
      } else {
        renderer.fillRoundedRect(cx, cy, 20, 20, 2, Color::Black);
      }
    }
  }

  // Books with reading time this week.
  const int listTop = body.y + 508;
  renderer.drawText(UI_10_FONT_ID, x, listTop, tr(STR_BOOK_BREAKDOWN), true, EpdFontFamily::BOLD);
  std::vector<std::pair<uint64_t, const ReadingBookStats*>> weekly;
  const uint32_t weekStart = clock ? today - 6 : 0;
  for (const auto& book : READING_STATS.getBooks()) {
    uint64_t ms = 0;
    for (const auto& day : book.readingDays) {
      if (!clock || (day.dayOrdinal >= weekStart && day.dayOrdinal <= today)) ms += day.readingMs;
    }
    if (ms > 0) weekly.push_back({ms, &book});
  }
  std::sort(weekly.begin(), weekly.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  if (weekly.size() > 3) weekly.resize(3);
  const int listHeaderY = listTop;
  {
    char count[32];
    std::snprintf(count, sizeof(count), I18N.get(StrId::STR_BOOKS_THIS_WEEK_FMT),
                  static_cast<unsigned>(weekly.size()));
    const int cw = renderer.getTextWidth(UI_10_FONT_ID, count);
    renderer.drawText(UI_10_FONT_ID, x + w - cw, listHeaderY, count);
  }
  int rowY = listTop + 32;
  for (const auto& entry : weekly) {
    const ReadingBookStats* book = entry.second;
    renderer.drawRect(x, rowY + 10, 44, 62, 2, true);
    drawMaskIcon(renderer, icon_book_48, x + 8, rowY + 26, 28);
    const std::string title = renderer.truncatedText(UI_12_FONT_ID, book->title.c_str(), w - 44 - 60 - 72,
                                                     EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + 60, rowY + 16, title.c_str(), true, EpdFontFamily::BOLD);
    char meta[64];
    std::snprintf(meta, sizeof(meta), "%u%%", static_cast<unsigned>(book->lastProgressPercent));
    renderer.drawText(UI_10_FONT_ID, x + 60, rowY + 48, meta);
    const std::string time = compactDuration(entry.first);
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, time.c_str());
    renderer.drawText(UI_10_FONT_ID, x + w - tw, rowY + 30, time.c_str());
    renderer.drawLine(x, rowY + 84, x + w, rowY + 84, true);
    rowY += 88;
  }

  // Achievements and the reading profile live behind their own page; this
  // quiet row keeps them reachable from the chart without another card.
  const int moreY = body.y + 816;
  if (moreY + 56 < body.y + body.height) {
    renderer.drawLine(x, moreY, x + w, moreY, true);
    drawMaskIcon(renderer, icon_trophy_48, x, moreY + 12, 24);
    renderer.drawText(UI_10_FONT_ID, x + 36, moreY + 14, tr(STR_ACHIEVEMENTS_PROFILE), true,
                      EpdFontFamily::BOLD);
    drawChevron(renderer, x + w - 10, moreY + 18);
  }

  // Session line at the foot.
  const int sessionY = body.y + body.height - 52;
  drawMaskIcon(renderer, icon_clock_48, x, sessionY + 2, 20);
  renderer.drawText(UI_10_FONT_ID, x + 28, sessionY + 4, tr(STR_SESSION_READING));
  const auto& session = READING_STATS.getLastSessionSnapshot();
  const std::string sessionText = session.valid ? compactDuration(session.sessionMs) : std::string("--");
  const int sw = renderer.getTextWidth(UI_10_FONT_ID, sessionText.c_str());
  renderer.drawText(UI_10_FONT_ID, x + w - sw, sessionY + 4, sessionText.c_str());
}

// ------------------------------------------------------------------ Tools --
void MyActivity::drawTools() {
  const Rect body = bodyRect();
  const char* headers[3] = {tr(STR_GROUP_READING), tr(STR_GROUP_BREAK), tr(STR_GROUP_MORE)};
  for (int group = 0; group < 3; ++group) {
    const Rect first = toolCellRect(group * 4);
    const int headerY = first.y - kGroupHeader + 12;
    renderer.drawText(UI_10_FONT_ID, first.x, headerY, headers[group], true, EpdFontFamily::BOLD);
    const std::string count = "4";
    const int cw = renderer.getTextWidth(UI_10_FONT_ID, count.c_str());
    renderer.drawText(UI_10_FONT_ID, first.x + 4 * (kToolW + kToolGap) - kToolGap - cw, headerY, count.c_str());
  }
  for (int i = 0; i < kToolCount; ++i) {
    const ToolEntry* entry = toolAt(i);
    if (entry == nullptr) continue;
    const Rect cell = toolCellRect(i);
    if (i == toolSelected_) {
      renderer.drawRoundedRect(cell.x - 2, cell.y - 2, cell.width + 4, cell.height + 4, 2, 18, true);
    }
    if (i == 0) {
      renderer.fillRoundedRect(cell.x, cell.y, cell.width, cell.height, 16, Color::Black);
      InxAppIcons::draw(renderer, entry->icon, cell.x + (cell.width - 48) / 2, cell.y + 20, 2, false);
      const int labelW = renderer.getTextWidth(UI_10_FONT_ID, I18N.get(entry->title));
      renderer.drawText(UI_10_FONT_ID, cell.x + (cell.width - labelW) / 2, cell.y + 84, I18N.get(entry->title),
                        false);
      // The icon occupies the black tile; redraw it white on black.
      InxAppIcons::draw(renderer, entry->icon, cell.x + (cell.width - 48) / 2, cell.y + 20, 2, true);
    } else {
      InxAppIcons::draw(renderer, entry->icon, cell.x + (cell.width - 48) / 2, cell.y + 20, 2, false);
      const std::string label = renderer.truncatedText(UI_10_FONT_ID, I18N.get(entry->title), cell.width - 8);
      const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
      renderer.drawText(UI_10_FONT_ID, cell.x + (cell.width - labelW) / 2, cell.y + 84, label.c_str());
    }
  }

  const Rect plugin = toolPluginRect();
  renderer.drawLine(plugin.x, plugin.y, plugin.x + plugin.width, plugin.y, true);
  drawMaskIcon(renderer, icon_blocks_48, plugin.x, plugin.y + (plugin.height - 24) / 2, 24);
  renderer.drawText(UI_12_FONT_ID, plugin.x + 36, plugin.y + (plugin.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    tr(STR_PLUGIN_DIRECTORY), true, EpdFontFamily::BOLD);
  drawChevron(renderer, plugin.x + plugin.width - 12, plugin.y + plugin.height / 2 - 6);
}

// --------------------------------------------------------------- Settings --
void MyActivity::drawQuickPills() {
  const std::string wifi = wifiValue();
  const char* labels[3] = {wifi.c_str(), tr(STR_BLUETOOTH), nullptr};
  char sleep[48];
  std::snprintf(sleep, sizeof(sleep), I18N.get(StrId::STR_SLEEP_MINUTES_FMT),
                static_cast<unsigned>(SETTINGS.sleepTimeoutMinutes));
  labels[2] = sleep;
  const freeink::Icon* icons[3] = {&icon_wifi_48, &icon_bluetooth_48, &icon_moon_48};
  for (int i = 0; i < 3; ++i) {
    const Rect r = quickPillRect(i);
    renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 2, r.height / 2, true);
    drawMaskIcon(renderer, *icons[i], r.x + 14, r.y + (r.height - 18) / 2, 18);
    const std::string shown = renderer.truncatedText(SMALL_FONT_ID, labels[i], r.width - 46);
    renderer.drawText(SMALL_FONT_ID, r.x + 38, r.y + (r.height - renderer.getLineHeight(SMALL_FONT_ID)) / 2,
                      shown.c_str());
  }
}

void MyActivity::drawSearchRow() {
  const Rect r = searchRowRect();
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 2, 12, true);
  drawMaskIcon(renderer, icon_search_32, r.x + 16, r.y + (r.height - 20) / 2, 20);
  renderer.drawText(UI_10_FONT_ID, r.x + 48, r.y + (r.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                    tr(STR_SEARCH_SETTINGS));
  drawChevron(renderer, r.x + r.width - 22, r.y + r.height / 2 - 6);
}

void MyActivity::drawSettingsCard(const int index) {
  const Rect card = cardRect(index);
  renderer.fillRoundedRect(card.x, card.y, card.width, card.height, 16, Color::White);
  renderer.drawRoundedRect(card.x, card.y, card.width, card.height, 2, 16, true);

  const freeink::Icon* headerIcon = nullptr;
  StrId title = StrId::STR_CAT_READER;
  switch (index) {
    case 0: headerIcon = &icon_book_48; title = StrId::STR_CAT_READER; break;
    case 1: headerIcon = &icon_sun_48; title = StrId::STR_CAT_DISPLAY; break;
    case 2: headerIcon = &icon_hand_48; title = StrId::STR_CAT_CONTROLS; break;
    case 3: headerIcon = &icon_sliders_horizontal_48; title = StrId::STR_CAT_SYSTEM; break;
  }
  drawMaskIcon(renderer, *headerIcon, card.x + 18, card.y + (kCardHeader - 28) / 2, 28);
  renderer.drawText(UI_12_FONT_ID, card.x + 58, card.y + (kCardHeader - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    I18N.get(title), true, EpdFontFamily::BOLD);
  drawChevron(renderer, card.x + card.width - 26, card.y + kCardHeader / 2 - 6);

  const char* labels[3] = {};
  std::string values[3];
  const freeink::Icon* rowIcons[3] = {};
  bool chevron[3] = {true, true, true};
  switch (index) {
    case 0:
      labels[0] = tr(STR_TEXT_SETTINGS);
      values[0] = std::to_string(SETTINGS.fontPointSize) + " pt";
      rowIcons[0] = &icon_type_48;
      labels[1] = tr(STR_PAGE_TURN);
      values[1] = gestureLabel();
      rowIcons[1] = &icon_hand_48;
      labels[2] = tr(STR_DAILY_GOAL);
      values[2] = compactDuration(getDailyReadingGoalMs());
      rowIcons[2] = &icon_target_48;
      break;
    case 1:
      labels[0] = tr(STR_UI_DENSITY);
      values[0] = densityLabel();
      rowIcons[0] = &icon_layout_grid_48;
      labels[1] = tr(STR_THEME);
      values[1] = tr(STR_THEME_CPY);
      rowIcons[1] = &icon_palette_48;
      labels[2] = tr(STR_STANDBY_SCREEN);
      values[2] = tr(STR_CLOCK);
      rowIcons[2] = &icon_moon_48;
      break;
    case 2:
      labels[0] = tr(STR_SIDE_BTN_LAYOUT);
      values[0] = "";
      rowIcons[0] = &icon_sliders_horizontal_48;
      labels[1] = tr(STR_TOUCH_READER_CONTROLS);
      values[1] = SETTINGS.touchReaderControls ? tr(STR_ON) : tr(STR_OFF);
      rowIcons[1] = &icon_hand_48;
      labels[2] = tr(STR_MOTION_PAGE_TURN);
      values[2] = motionLabel();
      rowIcons[2] = &icon_rotate_cw_48;
      break;
    case 3:
      labels[0] = "Wi-Fi";
      values[0] = WiFi.status() == WL_CONNECTED ? std::string(WiFi.SSID().c_str()) : std::string(tr(STR_CLOSED));
      rowIcons[0] = &icon_wifi_48;
      labels[1] = tr(STR_BLUETOOTH);
      values[1] = SETTINGS.bluetoothEnabled ? tr(STR_ON) : tr(STR_OFF);
      rowIcons[1] = &icon_bluetooth_48;
      labels[2] = tr(STR_STORAGE);
      values[2] = storageKnown_ ? gigabytes(storageFreeBytes_) : std::string("--");
      rowIcons[2] = &icon_folder_48;
      break;
  }

  for (int row = 0; row < 3; ++row) {
    const Rect r = cardRowRect(index, row);
    renderer.drawLine(r.x, r.y, r.x + r.width, r.y, true);
    drawMaskIcon(renderer, *rowIcons[row], r.x + 18, r.y + (r.height - 24) / 2, 24);
    renderer.drawText(UI_10_FONT_ID, r.x + 52,
                      r.y + (r.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, labels[row]);
    if (!values[row].empty()) {
      const std::string shown = renderer.truncatedText(SMALL_FONT_ID, values[row].c_str(), 118);
      const int vw = renderer.getTextWidth(SMALL_FONT_ID, shown.c_str());
      renderer.drawText(SMALL_FONT_ID, r.x + r.width - 26 - vw,
                        r.y + (r.height - renderer.getLineHeight(SMALL_FONT_ID)) / 2, shown.c_str());
    }
    if (chevron[row]) drawChevron(renderer, r.x + r.width - 18, r.y + r.height / 2 - 6);
  }
}

void MyActivity::drawAboutRow() {
  const Rect r = aboutRect();
  const Rect body = bodyRect();
  renderer.drawLine(body.x + kMargin, r.y, body.x + body.width - kMargin, r.y, true);
  drawMaskIcon(renderer, icon_info_48, r.x, r.y + (r.height - 24) / 2, 24);
  renderer.drawText(UI_12_FONT_ID, r.x + 36, r.y + (r.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    tr(STR_ABOUT_DEVICE_ROW), true, EpdFontFamily::BOLD);
  std::string version = CROSSPOINT_VERSION;
  const size_t dash = version.find('-');
  if (dash != std::string::npos) version.resize(dash);
  const int vw = renderer.getTextWidth(UI_10_FONT_ID, version.c_str());
  renderer.drawText(UI_10_FONT_ID, r.x + r.width - 14 - vw, r.y + (r.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                    version.c_str());
  drawChevron(renderer, r.x + r.width - 8, r.y + r.height / 2 - 6);

  const int noteY = r.y + r.height + 8;
  if (noteY + 24 < body.y + body.height) {
    renderer.drawText(SMALL_FONT_ID, r.x, noteY, tr(STR_SETTINGS_EFFECT_NOTE));
  }
  drawRipples(body.x + body.width - 100, body.y + body.height - 60, 62);
}

void MyActivity::drawSettings() {
  drawQuickPills();
  drawSearchRow();
  for (int i = 0; i < 4; ++i) drawSettingsCard(i);
  drawAboutRow();
}

// ------------------------------------------------------------------- Loop --
void MyActivity::activateTab(const Tab tab) {
  tab_ = tab;
  requestUpdate();
}

void MyActivity::openSettingsCategory(const int category) {
  // Child of My: back header + pop, not the main-tab root.
  activityManager.pushActivity(std::make_unique<SettingsActivity>(renderer, mappedInput, category, true));
}

void MyActivity::openTool(const int index) {
  const ToolEntry* entry = toolAt(index);
  if (entry == nullptr) return;
  switch (entry->action) {
    case ToolAction::ReadingStats: activityManager.goToReadingStatsMenu(); return;
    case ToolAction::FileTransfer: activityManager.goToFileTransfer(); return;
    case ToolAction::JoinNetwork: activityManager.goToJoinNetwork(); return;
    case ToolAction::FileBrowser: activityManager.goToFileBrowser(); return;
    case ToolAction::Woodfish: activityManager.goToWoodfish(); return;
    case ToolAction::Game2048: activityManager.goToGame2048(); return;
    case ToolAction::Sudoku: activityManager.goToSudoku(); return;
    case ToolAction::Sokoban: activityManager.goToSokoban(); return;
    case ToolAction::Chess:
#ifdef ENABLE_CHINESE_VERSION
      activityManager.goToChineseChess();
#endif
      return;
    case ToolAction::Minesweeper: activityManager.goToMinesweeper(); return;
    case ToolAction::Avatar: activityManager.goToUglyAvatar(); return;
    case ToolAction::Standby: activityManager.goToStandby(); return;
  }
}

void MyActivity::openBookList() { activityManager.goToReadingStatsMenu(); }

bool MyActivity::handleTabTouch(const int x, const int y) {
  for (int i = 0; i < kTabCount; ++i) {
    const Rect r = tabRect(i);
    if (x >= r.x - 10 && x < r.x + r.width + 10 && y >= r.y && y < r.y + r.height) {
      activateTab(static_cast<Tab>(i));
      return true;
    }
  }
  return false;
}

bool MyActivity::handleRecordTouch(const int x, const int y) {
  const Rect body = bodyRect();
  const int contentX = body.x + kMargin;
  const int contentW = body.width - kMargin * 2;
  if (y >= body.y + 290 && y <= body.y + 430) {
    activityManager.pushActivity(std::make_unique<ReadingHeatmapActivity>(renderer, mappedInput));
    return true;
  }
  if (y >= body.y + 816 && y <= body.y + 872) {
    openBookList();
    return true;
  }
  if (y >= body.y + 500 && y <= body.y + 815) {
    openBookList();
    return true;
  }
  if (y >= body.y + 130 && y <= body.y + 280) {
    const time_t now = time(nullptr);
    if (validClock()) {
      const uint32_t today = dayOrdinalFor(now);
      const int col = std::clamp((x - contentX) * 7 / std::max(1, contentW), 0, 6);
      const uint32_t ordinal = today - static_cast<uint32_t>(6 - col);
      activityManager.pushActivity(
          std::make_unique<ReadingDayDetailActivity>(renderer, mappedInput, ordinal));
    }
    return true;
  }
  return false;
}

bool MyActivity::handleToolTouch(const int x, const int y) {
  for (int i = 0; i < kToolCount; ++i) {
    const Rect cell = toolCellRect(i);
    if (x >= cell.x && x < cell.x + cell.width && y >= cell.y && y < cell.y + cell.height) {
      toolSelected_ = i;
      openTool(i);
      return true;
    }
  }
  const Rect plugin = toolPluginRect();
  if (x >= plugin.x && x < plugin.x + plugin.width && y >= plugin.y && y < plugin.y + plugin.height) {
    openSettingsCategory(3);
    return true;
  }
  return false;
}

bool MyActivity::handleSettingsTouch(const int x, const int y) {
  const Rect search = searchRowRect();
  if (x >= search.x && x < search.x + search.width && y >= search.y && y < search.y + search.height) {
    activityManager.pushActivity(std::make_unique<SettingsSearchActivity>(renderer, mappedInput));
    return true;
  }
  for (int i = 0; i < 3; ++i) {
    const Rect pill = quickPillRect(i);
    if (x >= pill.x && x < pill.x + pill.width && y >= pill.y && y < pill.y + pill.height) {
      // A status chip acts on its own subject: Wi-Fi opens the network chooser,
      // Bluetooth opens its settings, and the sleep chip opens the display
      // category that owns the timeout.
      if (i == 0) {
        activityManager.pushActivity(
            std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*autoConnect=*/false,
                                                    /*keepRadioOnExit=*/false));
      } else if (i == 1) {
        activityManager.pushActivity(std::make_unique<BluetoothSettingsActivity>(renderer, mappedInput));
      } else {
        openSettingsCategory(0);
      }
      return true;
    }
  }
  for (int card = 0; card < 4; ++card) {
    const Rect r = cardRect(card);
    if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) {
      // The dashboard card is a summary of ONE settings category; every tap
      // (header or preview row) opens that category as a child page. The old
      // per-row mapping sent "Wi-Fi" and "登录与账号" to different parents and
      // left two visual models for the same four groups.
      openSettingsCategory(card == 0 ? 1 : card == 1 ? 0 : card == 2 ? 2 : 3);
      return true;
    }
  }
  const Rect about = aboutRect();
  if (x >= about.x && x < about.x + about.width && y >= about.y && y < about.y + about.height) {
    activityManager.pushActivity(std::make_unique<AboutActivity>(renderer, mappedInput));
    return true;
  }
  return false;
}

void MyActivity::debugMoveRing(const int delta) {
  if (delta == 0) return;
  if (tab_ != Tab::Tools) {
    tab_ = Tab::Tools;
  } else {
    toolSelected_ = ((toolSelected_ + delta) % kToolCount + kToolCount) % kToolCount;
  }
  requestUpdate();
}

void MyActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (handleTabTouch(x, y)) return;
    if (tab_ == Tab::Records && handleRecordTouch(x, y)) return;
    if (tab_ == Tab::Tools && handleToolTouch(x, y)) return;
    if (tab_ == Tab::Settings && handleSettingsTouch(x, y)) return;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (tab_ != Tab::Records) {
      activateTab(Tab::Records);
    } else {
      activityManager.goHome();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    activateTab(static_cast<Tab>((static_cast<int>(tab_) + kTabCount - 1) % kTabCount));
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    activateTab(static_cast<Tab>((static_cast<int>(tab_) + 1) % kTabCount));
    return;
  }
  if (tab_ == Tab::Tools) {
    if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
      toolSelected_ = (toolSelected_ + 1) % kToolCount;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
      toolSelected_ = (toolSelected_ + kToolCount - 1) % kToolCount;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openTool(toolSelected_);
      return;
    }
  }
}

void MyActivity::render(RenderLock&&) {
  // 16-level canvas: the same rule as Home and the shelf. A gray page left behind
  // by Home would otherwise force a whole-screen GC16 on the way here (a visible
  // flash), and the tabs must be able to trade places without one.
  const bool gray = renderer.beginGrayscale16();
  if (!gray) renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_MINE));
  drawTabs();
  switch (tab_) {
    case Tab::Records: drawRecords(); break;
    case Tab::Tools: drawTools(); break;
    case Tab::Settings: drawSettings(); break;
  }
  if (gray) {
    renderer.commitGrayscale16(false);
  } else {
    renderer.displayBuffer();
  }
}
