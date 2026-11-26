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

#include "AppCatalog.h"
#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "activities/ActivityManager.h"
#include "activities/apps/reading-stats/ReadingDayDetailActivity.h"
#include "activities/apps/reading-stats/ReadingHeatmapActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/AboutActivity.h"
#if !defined(SIMULATOR)
#include "activities/settings/BluetoothSettingsActivity.h"
#endif
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
constexpr int kToolW = 139;
constexpr int kToolH = 120;
constexpr int kToolGap = 16;

void drawMaskIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const int size) {
  renderer.drawMaskAA(icon.bits, icon.w, icon.h, x, y, size, size, true, true);
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
#if defined(CROSSPOINT_PLUS_PORT)
  // 4G: the compat layer reports the modem link as a connected station and
  // SSID() as "4G", which would read like a Wi-Fi join. Show the transport the
  // user selected instead, plus the modem's own lease when there is one.
  if (SETTINGS.networkType == CrossPointSettings::NETWORK_CELLULAR) {
    const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
    char buf[64];
    if (ip.length() > 0) {
      std::snprintf(buf, sizeof(buf), "%s %s", tr(STR_NETWORK_4G), ip.c_str());
    } else {
      std::snprintf(buf, sizeof(buf), "%s", tr(STR_NETWORK_4G));
    }
    return buf;
  }
#endif
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

}  // namespace

void MyActivity::onEnter() {
  Activity::onEnter();
  if (initialTab_ >= 0 && initialTab_ < kTabCount) tab_ = static_cast<Tab>(initialTab_);
  initialTab_ = -1;
  toolSelected_ = 0;
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  // Installed native plugins are part of the tool drawer, not a second launcher.
  app_catalog::refreshPlugins();
#endif
  storageKnown_ = HalStorage::getInstance().getSpace(storageTotalBytes_, storageFreeBytes_);
#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)
  signInReminder_.maybeShow(renderer, mappedInput);
#endif
  requestUpdate();
}

void MyActivity::onExit() {
#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)
  signInReminder_.dismiss();
#endif
  Activity::onExit();
}

Rect MyActivity::tabRect(const int index) const {
  const Rect content = pageContentRect();
  // Slot widths follow the measured label (bold when active) instead of a
  // fixed 120 px pitch. The pitch was tuned for 2-3 glyph CJK labels; a
  // proportional script ("Records") is wider and ran into the next tab.
  const char* labels[kTabCount] = {I18N.get(StrId::STR_MY_TAB_RECORDS), I18N.get(StrId::STR_MY_TAB_TOOLS),
                                   I18N.get(StrId::STR_SETTINGS_TITLE)};
  int x = content.x + kMargin;
  for (int i = 0; i < kTabCount; ++i) {
    const bool on = static_cast<int>(tab_) == i;
    const int w = renderer.getTextWidth(UI_12_FONT_ID, labels[i],
                                        on ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int slot = w + 56;  // trailing air keeps the old 120 px CJK rhythm
    if (i == index) return Rect{x, content.y, slot, kTabH};
    x += slot;
  }
  return Rect{x, content.y, 110, kTabH};
}

Rect MyActivity::bodyRect() const {
  const Rect content = pageContentRect();
  return Rect{content.x, content.y + kTabH, content.width, std::max(0, content.height - kTabH)};
}

MyActivity::RecordsLayout MyActivity::recordsLayout() const {
  const Rect body = bodyRect();
  const uint16_t uiPermille = renderer.uiViewport().scalePermille;
  const auto scaled = [uiPermille](const int value) { return value * uiPermille / 1000; };
  RecordsLayout l;
  // Header block stacked by real line heights: the serif "9h34" is taller than
  // the fixed 52/82 offsets assumed, so the delta line and the wave used to be
  // painted through the digits whenever there was weekly reading time.
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int bigLine = renderer.getLineHeight(NOTOSERIF_TITLE_FONT_ID);
  l.titleY = body.y + scaled(28);
  l.bigY = l.titleY + line10 + 4;
  l.deltaY = l.bigY + bigLine - 2;
  l.waveY = l.deltaY + line10 + 8;
  l.barsTop = std::max(body.y + scaled(150), l.waveY + 16);
  const int bottomLimit = body.y + body.height - 52 - 8;  // session line + air
  // The absolute stack (baseline 104, 24 px heat cells, three book rows and the
  // achievements row) needs 872 px plus the 52 px session line. Keep it
  // byte-identical when the logical viewport is at least that tall.
  constexpr int kBaseNeed = 872 + 52 + 8;
  // The header stack can push the bars below the authored 150 px; everything
  // below moves with it so the month title never lands on the weekday labels.
  const int headerPush = l.barsTop - (body.y + scaled(150));
  if (body.height >= kBaseNeed) {
    l.heatTop = body.y + scaled(300) + headerPush;
    l.listTop = body.y + scaled(508) + headerPush;
    l.moreY = body.y + scaled(816) + headerPush;
    return l;
  }
  const int deficit = kBaseNeed - body.height;
  l.barMaxH = std::clamp(96 - deficit / 3, 36, 96);
  l.cellPitch = std::clamp(24 - deficit / 24, 16, 24);
  l.heatTop = l.barsTop + l.barMaxH + 8 + 46;
  l.listTop = l.heatTop + 34 + 7 * l.cellPitch + 6;
  l.moreY = std::min(body.y + scaled(816) + headerPush, bottomLimit - 56);
  const int rowsTop = l.listTop + 32;
  if (l.moreY <= rowsTop + 44) l.moreY = 0;
  const int rowsBottom = l.moreY > 0 ? l.moreY - 12 : bottomLimit;
  l.rows = std::clamp((rowsBottom - rowsTop) / 88, 0, 3);
  return l;
}

Rect MyActivity::cardRect(const int index) const {
  const Rect body = bodyRect();
  // Quick pills (14), search row (52), then 18 px of air before the cards.
  // The old top left only 4 px between the search field and the cards.
  const int top = body.y + kQuickH + 20 + 52 + 18;
  // Two cards fill the content axis exactly. The fixed 292 pair was 604 wide
  // inside a 594 content band, so the grid ended 9-10 px past the right margin
  // (left 45 / right 36 on a 684 px screen).
  const int w = (body.width - kMargin * 2 - kCardGapX) / 2;
  // A UI scale above 100% shrinks the logical viewport, so the 296 px tiles
  // plus the about row no longer fit. Keep 296 whenever it does, and shrink the
  // tiles (their rows follow) instead of drawing under the tab bar.
  constexpr int kAboutBlock = 16 + 64;
  const int avail = body.y + body.height - 8 - top - kAboutBlock - kCardGapY;
  const int h = std::clamp(avail / 2, kCardHeader + 3 * 48, kCardH);
  return Rect{body.x + kMargin + (index % 2) * (w + kCardGapX),
              top + (index / 2) * (h + kCardGapY), w, h};
}

Rect MyActivity::cardRowRect(const int card, const int row) const {
  const Rect r = cardRect(card);
  const int rowH = std::max(48, (r.height - kCardHeader) / 3);
  return Rect{r.x, r.y + kCardHeader + row * rowH, r.width, rowH};
}

Rect MyActivity::quickPillRect(const int index) const {
  const Rect body = bodyRect();
  // Fill the card axis: the old fixed 220/120/220 row stopped 24 px short of
  // the card grid's right edge. The middle pill keeps a fixed width (it only
  // holds "Bluetooth"); the two status pills share the remainder.
  constexpr int kGap = 10;
  constexpr int kMidW = 120;
  const int available = body.width - kMargin * 2;
  const int sideW = std::max(80, (available - kGap * 2 - kMidW) / 2);
  int x = body.x + kMargin;
  for (int i = 0; i < index; ++i) x += (i == 1 ? kMidW : sideW) + kGap;
  return Rect{x, body.y + 14, index == 1 ? kMidW : sideW, 44};
}

Rect MyActivity::searchRowRect() const {
  const Rect body = bodyRect();
  return Rect{body.x + kMargin, body.y + kQuickH + 20, body.width - kMargin * 2, 52};
}

Rect MyActivity::aboutRect() const {
  const Rect body = bodyRect();
  const Rect last = cardRect(3);
  return Rect{body.x + kMargin, last.y + last.height + 16, body.width - kMargin * 2, 64};
}

Rect MyActivity::toolCellRect(const int index) const {
  const Rect body = bodyRect();
  const int column = index % kToolColumns;
  const int row = index / kToolColumns;
  const int total = toolCount();
  const int rows = (total + kToolColumns - 1) / kToolColumns;
  // A larger UI scale shrinks the logical height while the catalogue keeps
  // growing (18 tiles = six rows). Compress the row pitch instead of letting
  // the last row slide under the tab bar. 100% keeps kToolH (body ~940 px).
  int gap = kToolGap;
  int cellH = kToolH;
  if (rows > 0) {
    const int avail = body.height - 12 - 8;
    int fit = (avail - (rows - 1) * kToolGap) / rows;
    if (fit < kToolH) {
      if (fit < 96) gap = 8;
      fit = (avail - (rows - 1) * gap) / rows;
      cellH = std::clamp(fit, 80, kToolH);
    }
  }
  const int top = body.y + 12 + row * (cellH + gap);
  // Fill the content axis: 4 * 139 + 3 * 16 = 604 was 10 px wider than the
  // 594 px band, so the last column ran under the right margin.
  const int w = (body.width - kMargin * 2 - (kToolColumns - 1) * kToolGap) / kToolColumns;
  return Rect{body.x + kMargin + column * (w + kToolGap), top, w, cellH};
}

int MyActivity::toolCount() const {
  return 2 + app_catalog::visibleCount(SETTINGS.hiddenAppsMask, OPDS_STORE.hasServers()) + 1;
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
      // Derive the bar from the glyph bottom. The fixed `kTabH - 18` was drawn
      // for the 12 pt font; the UI_12 role is 16 pt now and the bar covered the
      // bottom 5 px of the characters (device feedback 2026-11-04).
      const int textY = r.y + 26;
      const int underlineY = textY + renderer.getFontAscenderSize(UI_12_FONT_ID) + 6;
      renderer.fillRoundedRect(r.x, underlineY, w, 3, 1, Color::Black);
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

  const RecordsLayout layout = recordsLayout();
  renderer.drawText(UI_10_FONT_ID, x, layout.titleY, tr(STR_READING_WEEKLY), true, EpdFontFamily::BOLD);
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
      renderer.drawText(UI_10_FONT_ID, x + w - rw, layout.titleY, range);
    }
  }

  const std::string big = compactDuration(weekMs);
  renderer.drawText(NOTOSERIF_TITLE_FONT_ID, x, layout.bigY, big.c_str(), true, EpdFontFamily::BOLD);
  const int bigW = renderer.getTextWidth(NOTOSERIF_TITLE_FONT_ID, big.c_str(), EpdFontFamily::BOLD);
  if (prevMs > 0) {
    const int delta = static_cast<int>(static_cast<int64_t>(weekMs - prevMs) * 100 / static_cast<int64_t>(prevMs));
    char deltaText[32];
    std::snprintf(deltaText, sizeof(deltaText), "%+d%%", delta);
    char vs[64];
    std::snprintf(vs, sizeof(vs), I18N.get(StrId::STR_VS_LAST_WEEK_FMT), deltaText);
    renderer.drawText(UI_10_FONT_ID, x + bigW + 18, layout.deltaY, vs);
  }
  drawWaveLine(x, layout.waveY, w);

  // Seven day columns.
  const int barsTop = layout.barsTop;
  const int baseline = barsTop + layout.barMaxH + 8;
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
    const int h = ms == 0 ? 4 : 4 + static_cast<int>(ms * layout.barMaxH / maxMs);
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

  // Month heatmap: 7 rows (Sun..Sat) x week columns.
  const int hmTop = layout.heatTop;
  const int cellPitch = layout.cellPitch;
  const int cellSize = cellPitch - 4;
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
    const int gridW = columns * cellSize + (columns - 1) * 4;
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
      const int cx = gx + col * cellPitch;
      const int cy = gy + row * cellPitch;
      if (minutes == 0) {
        renderer.drawRoundedRect(cx, cy, cellSize, cellSize, 2, 2, true);
      } else if (minutes < 15) {
        renderer.fillRoundedRect(cx, cy, cellSize, cellSize, 2, Color::LightGray);
      } else if (minutes < 45) {
        renderer.fillRoundedRect(cx, cy, cellSize, cellSize, 2, Color::DarkGray);
      } else {
        renderer.fillRoundedRect(cx, cy, cellSize, cellSize, 2, Color::Black);
      }
    }
  }

  // Books with reading time this week.
  const int listTop = layout.listTop;
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
  const size_t maxRows = static_cast<size_t>(std::max(0, layout.rows));
  if (weekly.size() > maxRows) weekly.resize(maxRows);
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
  const int moreY = layout.moreY;
  if (moreY > 0 && moreY + 56 <= body.y + body.height - 52) {
    renderer.drawLine(x, moreY, x + w, moreY, true);
    drawMaskIcon(renderer, icon_trophy_48, x, moreY + 12, 24);
    renderer.drawText(UI_10_FONT_ID, x + 36, moreY + 14, tr(STR_ACHIEVEMENTS_PROFILE), true,
                      EpdFontFamily::BOLD);
    drawChevron(renderer, x + w - 10, moreY + 18);
  }

  // Session line at the foot. A compressed viewport puts it right against the
  // tab bar's duck/ripple art, so lift it by the ornament's overhang.
  const int sessionY = body.y + body.height - 52 - (body.height < 932 ? 20 : 0);
  drawMaskIcon(renderer, icon_clock_48, x, sessionY + 2, 20);
  renderer.drawText(UI_10_FONT_ID, x + 28, sessionY + 4, tr(STR_SESSION_READING));
  const auto& session = READING_STATS.getLastSessionSnapshot();
  const std::string sessionText = session.valid ? compactDuration(session.sessionMs) : std::string("--");
  const int sw = renderer.getTextWidth(UI_10_FONT_ID, sessionText.c_str());
  renderer.drawText(UI_10_FONT_ID, x + w - sw, sessionY + 4, sessionText.c_str());
}

// ------------------------------------------------------------------ Tools --
void MyActivity::drawTools() {
  const int visibleApps = app_catalog::visibleCount(SETTINGS.hiddenAppsMask, OPDS_STORE.hasServers());
  const int count = toolCount();
  if (toolSelected_ >= count) toolSelected_ = count > 0 ? count - 1 : 0;
  for (int i = 0; i < count; ++i) {
    const Rect cell = toolCellRect(i);
    const char* title = nullptr;
    UIIcon icon = UIIcon::File;
    const freeink::Icon* maskIcon = nullptr;
    if (i == 0) {
      title = tr(STR_JOIN_NETWORK);
      icon = UIIcon::Wifi;
    } else if (i == 1) {
      title = tr(STR_BROWSE_FILES);
      icon = UIIcon::File;
    } else if (i < 2 + visibleApps) {
      const int appIndex = app_catalog::visibleToAppIndex(SETTINGS.hiddenAppsMask, OPDS_STORE.hasServers(), i - 2);
      if (appIndex < 0) continue;
      title = app_catalog::title(appIndex);
      icon = app_catalog::at(appIndex).icon;
      // InxAppIcons has no UIIcon::Image entry (its switch falls through to
      // nullptr), so a catalog row that asks for the image glyph would draw no
      // icon at all -- the same failure the Wifi/File tiles had. The Lucide
      // 48 px image asset already exists for list rows, so route it through
      // the mask path the plugin tile uses. Only the camera tile uses it.
      if (icon == UIIcon::Image) maskIcon = &icon_image_48;
#if defined(CROSSPOINT_PLUS_PORT)
      // The voice memo tile: UIIcon has no audio glyph and BaseTheme.h is not
      // part of this port's edit scope, so the catalog row borrows the existing
      // Lucide 48 px music note through the same mask path.
      if (app_catalog::at(appIndex).id == appVisibility::AppId::VoiceMemo && maskIcon == nullptr) {
        maskIcon = &icon_music_48;
      }
      // AI voice client: same problem, different asset — the Lucide speaker.
      if (app_catalog::at(appIndex).id == appVisibility::AppId::VoiceClient && maskIcon == nullptr) {
        maskIcon = &icon_volume_2_48;
      }
#endif
    } else {
      title = tr(STR_PLUGIN_DIRECTORY);
      maskIcon = &icon_blocks_48;
    }

    if (i == toolSelected_) {
      renderer.drawRoundedRect(cell.x - 2, cell.y - 2, cell.width + 4, cell.height + 4, 2, 16, true);
    }
    // The tile reserves 48 px (the layout was drawn around that box); draw at
    // exactly 48. InxAppIcons::draw(scale=2) renders 64 px and pushed the label
    // out of the tile.
    constexpr int kIconPx = 48;
    const int ascender = renderer.getFontAscenderSize(UI_10_FONT_ID);
    // Bottom padding is measured from the glyph bottom, not from a fixed y: the
    // UI font is scalable (Compact..XL), so only an ink-relative anchor keeps
    // the label inside the tile.
    const int labelY = cell.y + cell.height - 14 - ascender;
    const int iconY = cell.y + std::max(4, (labelY - cell.y - kIconPx) / 2);
    if (maskIcon != nullptr) {
      drawMaskIcon(renderer, *maskIcon, cell.x + (cell.width - kIconPx) / 2, iconY, kIconPx);
    } else {
      InxAppIcons::drawSized(renderer, icon, cell.x + (cell.width - kIconPx) / 2, iconY, kIconPx, false);
    }
    const std::string label = renderer.truncatedText(UI_10_FONT_ID, title, cell.width - 8);
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
    renderer.drawText(UI_10_FONT_ID, cell.x + (cell.width - labelW) / 2, labelY, label.c_str());
  }
}

// --------------------------------------------------------------- Settings --
void MyActivity::drawQuickPills() {
  const std::string wifi = wifiValue();
  const char* labels[3] = {wifi.c_str(), tr(STR_BLUETOOTH), nullptr};
  char sleep[48];
  if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
    // "Never" is the top of the value range, not a number of minutes; printing
    // the raw value showed "31 min" instead of the never label the settings row
    // already uses. The moon icon already says what the pill is about.
    std::snprintf(sleep, sizeof(sleep), "%s", tr(STR_SLEEP_NEVER));
  } else {
    std::snprintf(sleep, sizeof(sleep), I18N.get(StrId::STR_SLEEP_MINUTES_FMT),
                  static_cast<unsigned>(SETTINGS.sleepTimeoutMinutes));
  }
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
  drawChevron(renderer, r.x + r.width - 18, r.y + r.height / 2 - 6);
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
  // Same label and chevron axis as the three rows below (52 / width - 18);
  // the old 58 / width - 26 put the header 6 px off the row column.
  renderer.drawText(UI_12_FONT_ID, card.x + 52, card.y + (kCardHeader - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    I18N.get(title), true, EpdFontFamily::BOLD);
  drawChevron(renderer, card.x + card.width - 18, card.y + kCardHeader / 2 - 6);

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
    const int labelX = r.x + 52;
    // Long translations ("Texteinstellungen", "Touch-Steuerung") used to run
    // under the right-aligned value: truncate the label to the space actually
    // left of it instead of drawing both and hoping they fit.
    int labelMax = r.x + r.width - 26;
    std::string shown;
    int valueWidth = 0;
    if (!values[row].empty()) {
      shown = renderer.truncatedText(SMALL_FONT_ID, values[row].c_str(), 118);
      valueWidth = renderer.getTextWidth(SMALL_FONT_ID, shown.c_str());
      labelMax = r.x + r.width - 26 - valueWidth - 10;
    }
    const std::string label = renderer.truncatedText(UI_10_FONT_ID, labels[row], std::max(40, labelMax - labelX));
    renderer.drawText(UI_10_FONT_ID, labelX, r.y + (r.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                      label.c_str());
    if (!shown.empty()) {
      renderer.drawText(SMALL_FONT_ID, r.x + r.width - 26 - valueWidth,
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
  renderer.drawText(UI_10_FONT_ID, r.x + r.width - 26 - vw,
                    r.y + (r.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, version.c_str());
  drawChevron(renderer, r.x + r.width - 18, r.y + r.height / 2 - 6);

  const int noteY = r.y + r.height + 8;
  const bool noteVisible = noteY + 24 < body.y + body.height;
  if (noteVisible) {
    renderer.drawText(SMALL_FONT_ID, r.x, noteY, tr(STR_SETTINGS_EFFECT_NOTE));
  }
  // The ripples are a footer ornament: when the compressed layout pushes the
  // about row down to the body edge they would paint over its version label,
  // so they only appear when the note row still leaves room for them.
  if (noteVisible) drawRipples(body.x + body.width - 100, body.y + body.height - 60, 62);
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
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  if (tab == Tab::Tools) app_catalog::refreshPlugins();
#endif
  requestUpdate();
}

void MyActivity::openSettingsCategory(const int category) {
  // Child of My: back header + pop, not the main-tab root.
  activityManager.pushActivity(std::make_unique<SettingsActivity>(renderer, mappedInput, category, true));
}

void MyActivity::openTool(const int index) {
  if (index < 0 || index >= toolCount()) return;
  const int visibleApps = app_catalog::visibleCount(SETTINGS.hiddenAppsMask, OPDS_STORE.hasServers());
  if (index == 0) {
    activityManager.goToJoinNetwork();
    return;
  }
  if (index == 1) {
    activityManager.goToFileBrowser();
    return;
  }
  if (index >= 2 && index < 2 + visibleApps) {
    const int appIndex = app_catalog::visibleToAppIndex(SETTINGS.hiddenAppsMask, OPDS_STORE.hasServers(), index - 2);
    if (appIndex >= 0) app_catalog::activate(appIndex);
    return;
  }
  openSettingsCategory(3);
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
  const RecordsLayout layout = recordsLayout();
  const int contentX = body.x + kMargin;
  const int contentW = body.width - kMargin * 2;
  const int baseline = layout.barsTop + layout.barMaxH + 8;
  const int rowsTop = layout.listTop + 32;
  if (y >= layout.heatTop - 6 && y <= layout.heatTop + 34 + 7 * layout.cellPitch + 6) {
    activityManager.pushActivity(std::make_unique<ReadingHeatmapActivity>(renderer, mappedInput));
    return true;
  }
  if (layout.rows > 0 && y >= rowsTop && y <= rowsTop + layout.rows * 88) {
    openBookList();
    return true;
  }
  if (layout.moreY > 0 && y >= layout.moreY && y <= layout.moreY + 56) {
    openBookList();
    return true;
  }
  if (y >= layout.barsTop - 20 && y <= baseline + 30) {
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
  const int count = toolCount();
  for (int i = 0; i < count; ++i) {
    const Rect cell = toolCellRect(i);
    if (x >= cell.x && x < cell.x + cell.width && y >= cell.y && y < cell.y + cell.height) {
      toolSelected_ = i;
      openTool(i);
      return true;
    }
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
                                                    /*keepRadioOnExit=*/true));
      } else if (i == 1) {
#if !defined(SIMULATOR)
#if !defined(SIMULATOR)
        activityManager.pushActivity(std::make_unique<BluetoothSettingsActivity>(renderer, mappedInput));
#else
        openSettingsCategory(2);
#endif
#else
        openSettingsCategory(2);
#endif
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

void MyActivity::debugSelect(const int index) {
  tab_ = Tab::Tools;
  const int count = toolCount();
  if (count > 0) toolSelected_ = std::clamp(index, 0, count - 1);
  requestUpdate();
}

void MyActivity::debugMoveRing(const int delta) {
  if (delta == 0) return;
  if (tab_ != Tab::Tools) {
    tab_ = Tab::Tools;
  } else {
    const int count = toolCount();
    toolSelected_ = ((toolSelected_ + delta) % count + count) % count;
  }
  requestUpdate();
}

void MyActivity::debugConfirm() {
  switch (tab_) {
    case Tab::Tools:
      openTool(toolSelected_);
      break;
    case Tab::Records:
      // The chart's "achievements and profile" row opens the reading-stats hub.
      openBookList();
      break;
    case Tab::Settings:
      openSettingsCategory(0);
      break;
  }
}

void MyActivity::loop() {
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  // Installing/removing through the plugin manager marks the catalog stale.
  // Pop back into this page does not call onEnter(), so refresh here before a
  // render can use the old row count.
  if (tab_ == Tab::Tools && app_catalog::pluginsDirty()) {
    app_catalog::refreshPlugins();
    requestUpdate();
  }
#endif
#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)
  if (signInReminder_.handleInput(mappedInput, [this] { requestUpdate(); })) return;
#endif
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
      toolSelected_ = (toolSelected_ + 1) % toolCount();
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
      toolSelected_ = (toolSelected_ + toolCount() - 1) % toolCount();
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
  if (gray) {
    // The gray canvas is retained between pages: without this fill, a page that
    // came from another activity keeps the previous layout's lines in the gaps
    // My never paints (seen after a UI-scale change left the settings list's
    // row dividers crossing the dashboard labels).
    renderer.fillRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), false);
    // fillRect's nibble fast path does not always bring the 1-bit proxy along;
    // the proxy is what overlays and screenshot dumps read, and it kept the
    // previous scale's icons. Clear it explicitly after the white fill.
    renderer.clearScreen();
  } else {
    renderer.clearScreen();
  }
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
#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)
  if (signInReminder_.processRender(renderer, mappedInput)) return;
#endif
}
