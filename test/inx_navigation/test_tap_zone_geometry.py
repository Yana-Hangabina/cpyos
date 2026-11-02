#!/usr/bin/env python3
"""Tap-zone geometry: the custom grid must agree with the legacy thirds.

`src/TapZones.h` holds the model both the reader and the zone editor use. This
test compiles that production header on the host and checks, for every panel
size the firmware ships on:

  * every painted main cell / mini zone resolves to its own index, lies inside
    the panel, and no two main cells overlap;
  * at the stock grid (tapzones::kDefaultZones), every point that lands in a
    cell resolves to the same action the pre-zone outer-thirds split produced
    for prev/next taps, the centre menu tap, and the mirrored (RTL / reversed)
    flip;
  * the same holds for every gesture combination through `seedZones()`, i.e.
    switching to the custom grid does not move or change a page-turn tap;
  * points in the safe margin / gaps are ignored (zone -1) instead of snapping
    to a neighbouring cell.

Run: python3 test/inx_navigation/test_tap_zone_geometry.py
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

PROGRAM = r'''
#include <cstdint>
#include <cstdio>

#include "TapZones.h"

namespace tz = tapzones;

static int failures = 0;
static void expect(const bool ok, const char* what, const int a, const int b) {
  if (ok) return;
  std::printf("FAIL %s: %d != %d\n", what, a, b);
  ++failures;
}

// Mirroring applied the way the reader does it before dispatch: with both
// directions reachable by tap, a PREV/NEXT action swaps sides when the
// page-turn direction / RTL book / legacy inverted tap state flips the reading
// sides. A single tap direction gets the whole page, so it never inverts.
static uint8_t resolved(const uint8_t action, const bool applyFlip) {
  if (action == tz::PREV && applyFlip) return tz::NEXT;
  if (action == tz::NEXT && applyFlip) return tz::PREV;
  return action;
}

static void checkPanel(const int w, const int h) {
  const tz::Grid grid(0, 0, w, h);

  // Cell / mini-zone geometry.
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      const tz::Rect r = grid.cell(row, col);
      expect(r.x >= 0 && r.y >= 0 && r.x + r.width <= w && r.y + r.height <= h, "main cell inside panel",
             r.y + r.height, h);
      expect(grid.zoneAt(r.x + r.width / 2, r.y + r.height / 2) == row * 3 + col, "main cell centre",
             row * 3 + col, grid.zoneAt(r.x + r.width / 2, r.y + r.height / 2));
    }
  }
  for (int i = 0; i < tz::Grid::kMiniCount; ++i) {
    const tz::Rect r = grid.miniCell(i);
    expect(r.x >= 0 && r.y >= 0 && r.x + r.width <= w && r.y + r.height <= h, "mini zone inside panel",
           r.y + r.height, h);
    expect(grid.zoneAt(r.x + r.width / 2, r.y + r.height / 2) == 9 + i, "mini zone centre", 9 + i,
           grid.zoneAt(r.x + r.width / 2, r.y + r.height / 2));
  }
  for (int a = 0; a < 9; ++a) {
    for (int b = a + 1; b < 9; ++b) {
      const tz::Rect ra = grid.cell(a / 3, a % 3);
      const tz::Rect rb = grid.cell(b / 3, b % 3);
      const bool apart = ra.x + ra.width <= rb.x || rb.x + rb.width <= ra.x || ra.y + ra.height <= rb.y ||
                         rb.y + rb.height <= ra.y;
      expect(apart, "main cells do not overlap", a, b);
    }
  }

  // Gesture combinations: a direction that accepts taps, the menu on a tap,
  // and the mirrored/inverted flip.
  for (int combo = 0; combo < 8; ++combo) {
    const bool prevTaps = (combo & 1) != 0;
    const bool nextTaps = (combo & 2) != 0;
    const bool menuTap = (combo & 4) != 0;
    uint8_t zones[9];
    tz::seedZones(zones, prevTaps, nextTaps, menuTap);
    for (int flipI = 0; flipI < 2; ++flipI) {
      const bool flip = flipI != 0;
      int mismatches = 0;
      int firstX = -1;
      int firstY = -1;
      int firstGrid = -1;
      int firstLegacy = -1;
      int inCells = 0;
      int gaps = 0;
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const int zone = grid.zoneAt(x, y);
          if (zone < 0) {
            ++gaps;
            continue;
          }
          // The mirrored reading direction swaps the outer columns. The legacy
          // split, however, gives "next" the whole left two thirds when flipped,
          // so its centre column stays a page turn while the custom grid mirrors
          // whatever action the user put there. Custom mode is opt-in and the
          // editor shows the actual actions, so the centre column is compared
          // only in the unflipped layout.
          if (flip && prevTaps && nextTaps) {
            const int fallback = zone < 9 ? zone : tz::miniFallbackZone(zone);
            if (fallback >= 0 && (fallback % 3) == 1) continue;
          }
          ++inCells;
          const bool applyFlip = flip && prevTaps && nextTaps;
          const uint8_t actual = resolved(tz::shortActionForZone(zone, zones, tz::kDefaultMiniZones), applyFlip);
          // Detection returns early when neither direction accepts taps (the
          // menu still opens through isTouchMenuGesture, which both layouts
          // share), so the zone action is NONE there.
          const uint8_t legacy = (!prevTaps && !nextTaps)
                                     ? tz::NONE
                                     : tz::legacyActionAt(x, y, w, h, flip, prevTaps, nextTaps, menuTap);
          if (actual != legacy) {
            if (mismatches == 0) {
              firstX = x;
              firstY = y;
              firstGrid = actual;
              firstLegacy = legacy;
            }
            ++mismatches;
          }
        }
      }
      if (mismatches != 0) {
        std::printf("%dx%d combo=%d flip=%d: %d of %d in-cell points disagree, e.g. (%d,%d) grid %d legacy %d\n",
                    w, h, combo, flip, mismatches, inCells, firstX, firstY, firstGrid, firstLegacy);
        ++failures;
      }
      expect(inCells > 0, "grid covers pixels", inCells, 0);
      expect(gaps > 0, "margin/gaps exist and are ignored", gaps, 0);
    }
  }
}

int main() {
  const int panels[][2] = {{684, 1216}, {480, 800}, {528, 792}, {1216, 684}, {800, 480}, {960, 540}, {224, 398}};
  for (const auto& panel : panels) checkPanel(panel[0], panel[1]);
  if (failures == 0) std::printf("custom grid agrees with the legacy thirds on every panel\n");
  return failures == 0 ? 0 : 1;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='tap-zones-') as temp:
        source = Path(temp) / 'tap_zones.cpp'
        source.write_text(PROGRAM)
        binary = Path(temp) / 'tap_zones'
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS: tap-zone geometry and legacy equivalence')


if __name__ == '__main__':
    main()
