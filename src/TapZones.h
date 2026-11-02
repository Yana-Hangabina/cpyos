#pragma once

// Reader tap-zone model: the persisted action enums and the pure geometry both
// the reader hit-test and the zone editor share. This header deliberately has
// no HAL / Arduino / I18n dependency so it can be compiled on the host (see
// test/inx_navigation/test_tap_zone_geometry.py); everything that needs the
// display size or the saved settings lives in
// src/activities/reader/ReaderUtils.h.
//
// Layout: a 3x3 main grid (row-major, top-left first) plus six small edge
// zones (four corners and the top/bottom edge middles). The small zones are
// nested in the corner / edge main cells and win over them, so a tap in a
// corner hot spot acts on the mini zone's own action and otherwise falls back
// to the main cell it sits in.

#include <cstdint>

namespace tapzones {

// Short-press action of a zone. PREV/NEXT act on a tap only while the matching
// direction's gesture accepts taps (Controls -> Next/Previous Page); MENU only
// opens the reader menu while "Show reader menu" is set to Tap.
enum Action : uint8_t {
  NONE = 0,
  PREV = 1,
  NEXT = 2,
  MENU = 3,
  BOOKMARK = 4,
  DICTIONARY = 5,
  CHAPTER = 6,      // jump to the next spine chapter
  FOOTNOTE = 7,     // open the footnote / link picker (EPUB)
  JUMP_PERCENT = 8, // jump-to-percent picker (EPUB)
  KOREADER = 9,     // push progress to KOReader (EPUB)
  AUTO_TURN = 10,   // toggle automatic page turning (EPUB)
  GO_HOME = 11,
  REFRESH = 12, // force a ghost-cleaning full refresh
  ROTATE_CW = 13,
  ROTATE_CCW = 14,
  ACTION_COUNT
};

// Long-press action of a main cell (fires once the finger has been held for
// ReaderUtils::BOOKMARK_HOLD_MS). A separate enum keeps the picker's long-press
// list short: PREV/NEXT have no long-press twin (a held page-turn cell still
// turns the page), and MENU is mostly useful as a hold action.
enum LongAction : uint8_t {
  LONG_NONE = 0,
  LONG_BOOKMARK = 1,
  LONG_DICTIONARY = 2,
  LONG_CHAPTER = 3,
  LONG_MENU = 4,
  LONG_FOOTNOTE = 5,
  LONG_JUMP_PERCENT = 6,
  LONG_KOREADER = 7,
  LONG_AUTO_TURN = 8,
  LONG_GO_HOME = 9,
  LONG_REFRESH = 10,
  LONG_ROTATE_CW = 11,
  LONG_ROTATE_CCW = 12,
  LONG_ACTION_COUNT
};

// The stock outer-thirds split, expressed as a grid: the left column turns
// back, the right column turns forward, the middle cell of the centre column
// opens the reader menu, and the rest of the centre column (above/below the
// menu third) turns forward like any tap beyond the left third did. With the
// default 6 px safe margin / 6 px gaps every main-cell pixel stays inside the
// corresponding legacy third (see the host test), so switching to the custom
// grid with this seed does not change where taps land for page turns.
constexpr uint8_t kDefaultZones[9] = {
    PREV, NEXT, NEXT,  // top row
    PREV, MENU, NEXT,  // middle row
    PREV, NEXT, NEXT,  // bottom row
};

constexpr uint8_t kDefaultLongZones[9] = {
    LONG_NONE, LONG_NONE, LONG_NONE, LONG_NONE, LONG_NONE,
    LONG_NONE, LONG_NONE, LONG_NONE, LONG_NONE,
};

constexpr uint8_t kDefaultMiniZones[6] = {NONE, NONE, NONE, NONE, NONE, NONE};

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
};

// Grid geometry for an arbitrary area. The reader builds it from the full
// display; the editor from its preview band. Both draw and hit-test through
// the same instance, so painted cells and tap targets cannot drift apart.
struct Grid {
  static constexpr int kSafeMargin = 6; // inset from the area edges
  static constexpr int kGap = 6;        // visible gap between cells
  static constexpr int kMiniCount = 6;  // corner + edge-middle small zones

  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int cellWidth = 0;
  int cellHeight = 0;
  int miniWidth = 0;
  int miniHeight = 0;
  int miniMidWidth = 0; // top/bottom edge-middle strips
  int miniMidHeight = 0;

  Grid(const int left, const int top, const int areaWidth, const int areaHeight) {
    x = left + kSafeMargin;
    y = top + kSafeMargin;
    width = areaWidth - kSafeMargin * 2;
    height = areaHeight - kSafeMargin * 2;
    if (width < 3) width = 3;
    if (height < 3) height = 3;
    // Two internal gaps per axis; any remainder widens the outer gaps, never
    // the visible cells, so cells keep one integer size everywhere.
    cellWidth = (width - kGap * 2) / 3;
    cellHeight = (height - kGap * 2) / 3;
    if (cellWidth < 1) cellWidth = 1;
    if (cellHeight < 1) cellHeight = 1;
    // Corner zones: a third of a main cell wide, a quarter tall, floored to a
    // usable touch target. Edge-middle zones are wide strips of the same area
    // budget instead of tall slivers.
    miniWidth = cellWidth / 3;
    miniHeight = cellHeight / 4;
    if (miniWidth < 40) miniWidth = 40;
    if (miniHeight < 44) miniHeight = 44;
    miniMidWidth = (cellWidth * 2) / 3;
    miniMidHeight = miniHeight / 2;
    if (miniMidHeight < 40) miniMidHeight = 40;
    if (miniWidth > width) miniWidth = width;
    if (miniHeight > height) miniHeight = height;
    if (miniMidWidth > width) miniMidWidth = width;
    if (miniMidHeight > height) miniMidHeight = height;
  }

  Rect cell(const int row, const int col) const {
    return Rect{static_cast<int16_t>(x + col * (cellWidth + kGap)),
                static_cast<int16_t>(y + row * (cellHeight + kGap)), static_cast<int16_t>(cellWidth),
                static_cast<int16_t>(cellHeight)};
  }

  // Small zone index 0..5:
  //   0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right,
  //   4 top-middle, 5 bottom-middle.
  Rect miniCell(const int index) const {
    int cx = x;
    int cy = y;
    switch (index) {
      case 1:
        cx = x + width - miniWidth;
        break;
      case 2:
        cy = y + height - miniHeight;
        break;
      case 3:
        cx = x + width - miniWidth;
        cy = y + height - miniHeight;
        break;
      case 4:
        cx = x + (width - miniMidWidth) / 2;
        break;
      case 5:
        cx = x + (width - miniMidWidth) / 2;
        cy = y + height - miniMidHeight;
        break;
      case 0:
      default:
        break;
    }
    const bool mid = index == 4 || index == 5;
    return Rect{static_cast<int16_t>(cx), static_cast<int16_t>(cy),
                static_cast<int16_t>(mid ? miniMidWidth : miniWidth),
                static_cast<int16_t>(mid ? miniMidHeight : miniHeight)};
  }

  // Zone index for a point: 0..8 main grid, 9..14 small zones, -1 in a gap or
  // outside the area. Small zones win where they are nested in a main cell.
  int zoneAt(const int px, const int py) const {
    for (int i = 0; i < kMiniCount; ++i) {
      const Rect r = miniCell(i);
      if (px >= r.x && px < r.x + r.width && py >= r.y && py < r.y + r.height) {
        return 9 + i;
      }
    }
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        const Rect r = cell(row, col);
        if (px >= r.x && px < r.x + r.width && py >= r.y && py < r.y + r.height) {
          return row * 3 + col;
        }
      }
    }
    return -1;
  }
};

// The main cell a small zone falls back to when its own action is NONE.
inline int miniFallbackZone(const int zone) {
  switch (zone - 9) {
    case 0:
      return 0; // top-left -> top-left
    case 1:
      return 2; // top-right -> top-right
    case 2:
      return 6; // bottom-left -> bottom-left
    case 3:
      return 8; // bottom-right -> bottom-right
    case 4:
      return 1; // top-middle -> top-middle
    case 5:
      return 7; // bottom-middle -> bottom-middle
    default:
      return -1;
  }
}

inline uint8_t clampAction(const uint8_t value) {
  return value < ACTION_COUNT ? value : static_cast<uint8_t>(NONE);
}

inline uint8_t clampLongAction(const uint8_t value) {
  return value < LONG_ACTION_COUNT ? value : static_cast<uint8_t>(LONG_NONE);
}

// Configured short action of a zone (what the editor shows), without the
// mini-zone fallback.
inline uint8_t rawShortAction(const int zone, const uint8_t zones[9], const uint8_t mini[6]) {
  if (zone < 0) return NONE;
  if (zone < 9) return clampAction(zones[zone]);
  if (zone - 9 >= Grid::kMiniCount) return NONE;
  return clampAction(mini[zone - 9]);
}

// Effective short action of a zone: a small zone with no action of its own
// reuses the main cell it sits in, so a corner tap never dead-ends unless the
// main cell is unset too.
inline uint8_t shortActionForZone(const int zone, const uint8_t zones[9], const uint8_t mini[6]) {
  if (zone < 0) return NONE;
  if (zone < 9) return clampAction(zones[zone]);
  if (zone - 9 >= Grid::kMiniCount) return NONE;
  const uint8_t own = clampAction(mini[zone - 9]);
  if (own != NONE) return own;
  const int fallback = miniFallbackZone(zone);
  return fallback >= 0 ? clampAction(zones[fallback]) : static_cast<uint8_t>(NONE);
}

// Long-press action of a zone; small zones are tap-only.
inline uint8_t longActionForZone(const int zone, const uint8_t longZones[9]) {
  if (zone < 0 || zone >= 9) return LONG_NONE;
  return clampLongAction(longZones[zone]);
}

// Seed the custom grid from the legacy configuration when the user switches to
// it. The legacy split gives the whole page to the sole tap-enabled direction
// (the centre third still opens the menu when the menu is a tap), so a
// one-direction setup seeds every cell with that direction; with both
// directions enabled it starts from the stock columns. A menu that is not a
// centre tap becomes a page turn, matching what a centre tap used to do.
inline void seedZones(uint8_t out[9], const bool prevTaps, const bool nextTaps, const bool menuTap) {
  for (int i = 0; i < 9; ++i) out[i] = kDefaultZones[i];
  if (!menuTap) {
    const uint8_t replacement = nextTaps ? NEXT : (prevTaps ? PREV : NONE);
    for (int i = 0; i < 9; ++i) {
      if (out[i] == MENU) out[i] = replacement;
    }
  }
  if (prevTaps && !nextTaps) {
    for (int i = 0; i < 9; ++i) out[i] = PREV;
  } else if (!prevTaps && nextTaps) {
    for (int i = 0; i < 9; ++i) out[i] = NEXT;
  } else if (!prevTaps && !nextTaps) {
    for (int i = 0; i < 9; ++i) out[i] = NONE;
  }
  if (menuTap && (prevTaps || nextTaps)) out[4] = MENU;
}

// Oracle for the pre-zone outer-thirds behaviour, kept next to the grid so the
// host test can prove the default grid still resolves taps the way the legacy
// split does. `flip` is the caller's mirrored XOR inverted-tap state.
inline uint8_t legacyActionAt(const int px, const int py, const int width, const int height, const bool flip,
                              const bool prevTaps, const bool nextTaps, const bool menuTap) {
  if (width <= 0 || height <= 0) return NONE;
  if (menuTap && px >= width / 3 && px < width - width / 3 && py >= height / 3 && py < height - height / 3) {
    return MENU;
  }
  if (!prevTaps && !nextTaps) return NONE;
  const bool nextZone = flip ? px < (width * 2) / 3 : px >= width / 3;
  if (nextTaps && (!prevTaps || nextZone)) return NEXT;
  if (prevTaps && (!nextTaps || !nextZone)) return PREV;
  return NONE;
}

} // namespace tapzones
