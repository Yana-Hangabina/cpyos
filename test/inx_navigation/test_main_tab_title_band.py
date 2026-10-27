#!/usr/bin/env python3
"""MainTabLayout title-band geometry: the CPY home reserves a title band above
the content; the band must never overlap the status strip, the tab bar or the
content rect, and titleHeight=0 must reproduce the pre-band geometry exactly."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

program = r'''
#include <cassert>
#include <cstdio>

#include "activities/MainTab.h"

int main() {
  const Rect safe{5, 5, 674, 1006};
  constexpr int kStatus = 48;
  constexpr int kTabBar = 96;
  constexpr int kTitle = 132;  // masthead strip (44) + title row (88), shipped

  // Bottom tabs, status bar, title band: the band sits between status and
  // content, and content starts right below it.
  const MainTabLayout banded = MainTabs::layout(safe, 0, kTabBar, true, kStatus, kTitle);
  assert(banded.statusBar.y + banded.statusBar.height <= banded.title.y);
  assert(banded.title.height == kTitle);
  assert(banded.content.y == banded.title.y + kTitle);
  assert(banded.content.y + banded.content.height <= banded.tabBar.y);
  assert(banded.title.width == safe.width);
  assert(banded.title.x == safe.x);

  // titleHeight = 0 reproduces the historical geometry exactly.
  const MainTabLayout legacy = MainTabs::layout(safe, 0, kTabBar, true, kStatus);
  const MainTabLayout plain = MainTabs::layout(safe, 0, kTabBar, true, kStatus, 0);
  assert(plain.title.height == 0);
  assert(plain.content.y == legacy.content.y);
  assert(plain.content.height == legacy.content.height);

  // Top tabs are never banded, even when a height is passed: the C3 geometry
  // must not move a single pixel.
  const MainTabLayout topLegacy = MainTabs::layout(safe, 0, 66, false, kStatus);
  const MainTabLayout top = MainTabs::layout(safe, 0, 66, false, kStatus, kTitle);
  assert(top.title.height == 0);
  assert(top.content.y == topLegacy.content.y);
  assert(top.content.height == topLegacy.content.height);

  std::puts("MainTabLayout title-band geometry: overlap, content offset and zero-height legacy parity pass");
}
'''

with tempfile.TemporaryDirectory(prefix='maintab-') as tmp:
    src = Path(tmp) / 'check.cpp'
    src.write_text(program)
    binary = Path(tmp) / 'check'
    subprocess.run(['c++', '-std=c++20', '-I', str(ROOT / 'src'), str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
