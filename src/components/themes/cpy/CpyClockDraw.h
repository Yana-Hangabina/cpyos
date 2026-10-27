#pragma once

// The CPY "Songti extra-bold" wall clock used by the standby duck face and the
// sleep screen (design: 页标题/锁屏时间 = 宋体特黑; frame 34 / frame 08 place it
// as a centred HH:MM with a language-neutral date line under it).
//
// Both callers render into their own coordinate space, so the helpers take an
// absolute centre line and top; line heights come from the live renderer so
// C3's non-high-DPI profile stays correct.
#include <GfxRenderer.h>
#include <I18n.h>
#include <SloppyDigits.h>
#include <time.h>

#include <cstdio>

#include "fontIds.h"

namespace cpy_clock {

inline constexpr int kDateFont = NOTOSERIF_12_FONT_ID;

// The clock reads "21:47" like the mockups; the date row mirrors the title
// band's language-neutral "%d/%d" plus a localised weekday (the almanac
// face's CAL_WEEKDAY keys), e.g. "10/16 · 星期四". Nothing date-like is drawn
// until the clock is plausible, matching the band and the standby faces.
inline bool clockPlausible() { return time(nullptr) > 1700000000; }

// Draws "HH:MM" centred at centerX with its top edge at top. Returns the
// block height so callers can stack the date under it.
//
// The time renders through the sloppy digits' procedural stroke engine (its
// Geometric alphabet with wobble/slant/rotation pinned to zero, so the
// strokes come out clean) instead of a text face: the built-in serif fonts
// cap out at 18px, which read as a footnote against the design's 96px clock
// on the device (user feedback, 2026-10-16). The engine is font- and
// SD-independent, so the face renders identically on every target.
inline int drawTime(const GfxRenderer& renderer, const int centerX, const int top, const unsigned hh, const unsigned mm) {
  sloppy::Style style{};
  style.alphabet = sloppy::AlphabetId::Geometric;
  style.wobble = 0.0f;
  style.strokeWidth = 6;
  style.slantDeg = 0.0f;
  style.digitRotateMax = 0.0f;
  style.digitGap = 14;
  style.oneIsPlain = false;
  sloppy::Seeds seeds{};

  // Template geometry (SloppyDigitAlphabets): digits are 100x160 units and a
  // two-digit group is 100 + gap + 100 wide. The box height is picked so the
  // uniform fit (which pads 8%) lands the digit ink at ~96px tall — the
  // mockups' clock size; the group box keeps its aspect so height binds.
  constexpr float kPad = 0.92f;
  constexpr int kBoxH = static_cast<int>(96 / kPad) + 1;                       // 105
  constexpr int kBoxW = static_cast<int>((214 * 0.62f) / kPad) + 2;            // ~146
  constexpr int kGroupW = 128;   // (2*100+14) * (96/160), the served ink width
  constexpr int kColonW = 12;
  constexpr int kSideGap = 26;
  constexpr int kGroupCenter = kColonW / 2 + kSideGap + kGroupW / 2;           // 96

  char left[4];
  char right[4];
  std::snprintf(left, sizeof(left), "%02u", hh);
  std::snprintf(right, sizeof(right), "%02u", mm);
  sloppy::draw(renderer, style, seeds, left,
               sloppy::Bounds{centerX - kGroupCenter - kBoxW / 2, top, kBoxW, kBoxH});
  sloppy::draw(renderer, style, seeds, right,
               sloppy::Bounds{centerX + kGroupCenter - kBoxW / 2, top, kBoxW, kBoxH});

  // The colon: two square dots at the classic 1/3 and 2/3 heights.
  renderer.fillRect(centerX - kColonW / 2, top + kBoxH * 27 / 100, kColonW, kColonW);
  renderer.fillRect(centerX - kColonW / 2, top + kBoxH * 62 / 100, kColonW, kColonW);
  return kBoxH;
}

// Draws the date row centred at centerX with its top edge at top (no-op until
// the clock is plausible). Returns the drawn height, 0 when skipped.
inline int drawDateLine(const GfxRenderer& renderer, const int centerX, const int top) {
  if (!clockPlausible()) return 0;
  static constexpr StrId kWeekdays[7] = {
      StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE,
      StrId::STR_CAL_WEEKDAY_WED, StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI,
      StrId::STR_CAL_WEEKDAY_SAT,
  };
  struct tm local {};
  const time_t now = time(nullptr);
  localtime_r(&now, &local);
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%d/%d \xc2\xb7 %s", local.tm_mon + 1, local.tm_mday,
                I18N.get(kWeekdays[local.tm_wday % 7]));
  const int width = renderer.getTextWidth(kDateFont, buf);
  renderer.drawText(kDateFont, centerX - width / 2, top, buf);
  return renderer.getLineHeight(kDateFont);
}

}  // namespace cpy_clock
