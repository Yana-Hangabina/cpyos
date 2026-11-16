#pragma once

#include <algorithm>

#include "components/Rect.h"
#include "components/ResponsiveUi.h"

// Geometry for the CPY home dashboard, kept out of the Activity so it can be
// exercised on the host for every panel size. The standard layout reproduces
// the 604 px design byte-for-byte; the compact layout fits the same cards into
// the smaller 480x800 / 552x768 panels by sizing the continue card, the reading
// strip and the recent covers from the available content box instead of fixed
// pixel counts.
namespace home_dashboard {

struct Layout {
  bool compact = false;

  Rect firstCard;
  Rect todayCard;
  Rect recentCard;
  Rect syncRow;
  Rect recentAll;

  // Continue card.
  Rect firstCover;
  int firstTextX = 0;
  int firstTextW = 0;
  int firstHeaderY = 0;
  int firstTitleY = 0;
  int firstAuthorY = 0;
  int firstChapterY = 0;
  bool firstShowChapter = true;
  int firstPillsY = 0;
  int firstPillH = 0;
  int firstActionY = 0;
  int firstActionH = 0;
  int firstWaveY = 0;
  int firstWaveH = 0;

  // Reading strip.
  int todayColumns = 4;
  int todayColW = 0;
  int todayIconY = 0;
  int todayValueY = 0;
  int todayLabelY = 0;
  bool todayShowBars = true;
  int todayBarX = 0;
  int todayBarW = 0;
  int todayBaselineY = 0;

  // Recent covers.
  Rect recentItems[3];
  Rect recentCovers[3];
  int recentHeaderY = 0;
  int recentTitleDY = 0;
  int recentAuthorDY = 0;
  int recentMetaDY = 0;
  bool recentShowAuthor = true;
  Rect recentChevron;

  // Update row.
  int syncIconSize = 28;
  int syncPillH = 48;
  int syncPad = 24;
};

inline Layout compute(const Rect& content, const bool showSync, const int smallLineH, const int bodyLineH) {
  Layout out;
  const int width = std::max(1, content.width);
  // Standard cards need ~852 px (960 with the update row); a panel that cannot
  // fit them uses the compact contract even when it is wide enough for four
  // columns.
  out.compact = responsive_ui::isCompact(width, content.height, showSync);

  if (!out.compact) {
    const int pad = 32;
    const int x = content.x + pad;
    const int w = std::max(1, content.width - pad * 2);
    int y = content.y + 4;

    out.firstCard = Rect{x, y, w, 318};
    y += out.firstCard.height + 12;
    out.todayCard = Rect{x, y, w, 140};
    y += out.todayCard.height + 12;
    out.recentCard = Rect{x, y, w, 370};
    out.recentAll = Rect{x + w - 156, y, 132, 44};
    for (int i = 0; i < 3; ++i) {
      out.recentItems[i] = Rect{x + 22 + i * (168 + 26), y + 50, 168, 300};
      out.recentCovers[i] = Rect{out.recentItems[i].x, out.recentItems[i].y, 132, 184};
    }
    y += out.recentCard.height + 12;
    out.syncRow = Rect{x, y, w, 96};

    out.firstCover = Rect{out.firstCard.x + 24, out.firstCard.y + 58, 140, 212};
    out.firstTextX = out.firstCover.x + out.firstCover.width + 24;
    out.firstTextW = std::max(1, out.firstCard.x + out.firstCard.width - 24 - out.firstTextX);
    out.firstHeaderY = out.firstCard.y + 18;
    out.firstTitleY = out.firstCard.y + 58;
    out.firstAuthorY = out.firstCard.y + 100;
    out.firstChapterY = out.firstCard.y + 132;
    out.firstPillsY = out.firstCard.y + 175;
    out.firstPillH = 34;
    out.firstActionY = out.firstCard.y + 214;
    out.firstActionH = 56;
    out.firstWaveY = out.firstCard.y + out.firstCard.height - 38;
    out.firstWaveH = 34;

    out.todayColumns = 4;
    out.todayColW = 100;
    out.todayIconY = out.todayCard.y + 16;
    out.todayValueY = out.todayCard.y + 52;
    out.todayLabelY = out.todayCard.y + 90;
    out.todayShowBars = true;
    out.todayBarX = out.todayCard.x + out.todayCard.width - 148;
    out.todayBarW = 128;
    out.todayBaselineY = out.todayCard.y + 88;

    out.recentHeaderY = out.recentCard.y + 16;
    out.recentTitleDY = 192;
    out.recentAuthorDY = 220;
    out.recentMetaDY = 248;
    out.recentShowAuthor = true;
    out.recentChevron = Rect{out.recentCard.x + out.recentCard.width - 20, out.recentHeaderY, 16, 16};

    out.syncIconSize = 28;
    out.syncPillH = 48;
    out.syncPad = 24;
    return out;
  }

  // ---- Compact ------------------------------------------------------------
  const int pad = 20;
  const int x = content.x + pad;
  const int w = std::max(1, content.width - pad * 2);
  const int gap = 10;
  const int cards = showSync ? 4 : 3;
  const int y0 = content.y + 2;

  int firstH = std::clamp(content.height * 34 / 100, 200, 232);
  int todayH = std::clamp(content.height * 15 / 100, 84, 110);
  // Four metrics need an icon row plus the value and label lines; CJK fallback
  // faces can make those lines taller than the Latin design metrics.
  const int minTodayH = 32 + std::max(bodyLineH, 1) + std::max(smallLineH, 1) + 4;
  todayH = std::max(todayH, minTodayH);
  int syncH = showSync ? std::clamp(content.height * 10 / 100, 56, 76) : 0;
  int recentH = content.height - 4 - firstH - todayH - syncH - gap * (cards - 1);
  if (recentH < 150) {
    const int deficit = 150 - recentH;
    const int takeFirst = std::min(deficit, std::max(0, firstH - 180));
    firstH -= takeFirst;
    recentH += takeFirst;
  }
  if (recentH < 150) {
    const int takeToday = std::min(150 - recentH, std::max(0, todayH - minTodayH));
    todayH -= takeToday;
    recentH += takeToday;
  }
  if (recentH < 150 && showSync) {
    const int takeSync = std::min(150 - recentH, std::max(0, syncH - 48));
    syncH -= takeSync;
    recentH += takeSync;
  }
  recentH = std::max(90, recentH);

  int y = y0;
  out.firstCard = Rect{x, y, w, firstH};
  y += firstH + gap;
  out.todayCard = Rect{x, y, w, todayH};
  y += todayH + gap;
  out.recentCard = Rect{x, y, w, recentH};
  y += recentH + gap;
  out.syncRow = Rect{x, y, w, syncH};

  // Continue card: a smaller cover on the left, the same text column on the
  // right. Cover height follows the card instead of a fixed 212 px.
  const int inner = 16;
  int coverH = std::clamp(firstH - 64, 96, 150);
  int coverW = coverH * 188 / 250;
  const int maxCoverW = std::max(72, w / 3);
  if (coverW > maxCoverW) {
    coverW = maxCoverW;
    coverH = coverW * 250 / 188;
  }
  out.firstCover = Rect{x + inner, out.firstCard.y + 32, coverW, coverH};
  out.firstTextX = out.firstCover.x + coverW + inner;
  out.firstTextW = std::max(1, out.firstCard.x + w - inner - out.firstTextX);
  out.firstHeaderY = out.firstCard.y + 8;
  out.firstTitleY = out.firstHeaderY + std::max(smallLineH, 1) + 2;
  out.firstAuthorY = out.firstTitleY + std::max(bodyLineH, 1) + 2;
  out.firstChapterY = out.firstAuthorY + std::max(smallLineH, 1) + 2;
  out.firstPillsY = out.firstChapterY + std::max(bodyLineH, 1) + 4;
  out.firstPillH = 28;
  out.firstActionH = 44;
  out.firstActionY = out.firstCard.y + firstH - out.firstActionH - 22;
  // Real CJK fallback line heights can be larger than the design's Latin
  // metrics. If the chapter line, pills and action cannot fit, drop the
  // chapter line first, then shrink the action; never let it leave the card.
  if (out.firstPillsY + out.firstPillH + 4 + out.firstActionH > out.firstCard.y + firstH) {
    out.firstShowChapter = false;
    out.firstChapterY = out.firstAuthorY;
    out.firstPillsY = out.firstAuthorY + std::max(smallLineH, 1) + 4;
  }
  out.firstActionY = out.firstCard.y + firstH - out.firstActionH - 22;
  out.firstActionY = std::max(out.firstActionY, out.firstPillsY + out.firstPillH + 4);
  if (out.firstActionY + out.firstActionH > out.firstCard.y + firstH) {
    out.firstActionH = std::max(36, out.firstCard.y + firstH - out.firstActionY);
  }
  out.firstWaveH = 14;
  out.firstWaveY = out.firstCard.y + firstH - out.firstWaveH - 2;
  if (out.firstActionY + out.firstActionH > out.firstWaveY) {
    // A very short card cannot host both: keep the action, drop the wave.
    out.firstWaveH = 0;
    out.firstWaveY = out.firstCard.y + firstH;
  }

  // Reading strip: four columns, no seven-day bars (the bars need 148 px that
  // a 480 px panel does not have beside four metrics).
  out.todayColumns = 4;
  out.todayColW = std::max(1, (w - 24) / 4);
  out.todayIconY = out.todayCard.y + 8;
  out.todayValueY = out.todayCard.y + 32;
  out.todayLabelY = out.todayValueY + std::max(bodyLineH, 1) + 1;
  out.todayShowBars = false;
  out.todayBarX = 0;
  out.todayBarW = 0;
  out.todayBaselineY = 0;

  // Recent covers: three columns, cover height capped by whatever remains
  // under the card header.
  const int recentInner = 12;
  const int itemGap = 10;
  const int cols = 3;
  int recentCoverW = std::max(1, (w - recentInner * 2 - itemGap * (cols - 1)) / cols);
  int recentCoverH = recentCoverW * 250 / 188;
  const int textH = std::max(smallLineH, 1) * 2 + 8;
  const int maxCoverH = std::max(40, recentH - 34 - 6 - textH);
  if (recentCoverH > maxCoverH) recentCoverH = maxCoverH;
  const int itemY = out.recentCard.y + 34;
  for (int i = 0; i < 3; ++i) {
    const int ix = x + recentInner + i * (recentCoverW + itemGap);
    out.recentCovers[i] = Rect{ix, itemY, recentCoverW, recentCoverH};
    out.recentItems[i] = Rect{ix, itemY, recentCoverW, std::max(1, recentH - 34)};
  }
  out.recentHeaderY = out.recentCard.y + 8;
  out.recentTitleDY = recentCoverH + 6;
  out.recentAuthorDY = 0;
  out.recentMetaDY = out.recentTitleDY + std::max(smallLineH, 1) + 2;
  out.recentShowAuthor = false;
  out.recentAll = Rect{x + w - recentInner - 104, out.recentCard.y + 4, 104, 32};
  out.recentChevron = Rect{x + w - recentInner - 12, out.recentHeaderY, 12, 12};

  out.syncIconSize = 24;
  out.syncPillH = 36;
  out.syncPad = 16;
  return out;
}

}  // namespace home_dashboard
