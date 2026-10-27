#!/usr/bin/env python3
"""CPY theme constants pinned at compile time: the metrics the theme contract
depends on (title band, battery pill, flat list rows) and the InxRecent layout
enumeration the dashboard plumbing relies on. Compiled against the real headers
so a silent metric change fails here before it reaches a device."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

program = r'''
#include <cassert>
#include <cstdio>

#include "InxRecentLayout.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/cpy/CpyTheme.h"
#include "components/themes/upstream/UpstreamStyleTheme.h"

int main() {
  // The dashboard slot sits between Cover and Count; the picker arrays in
  // SettingsList keep the five INX layouts only.
  static_assert(static_cast<uint8_t>(InxRecentLayout::Dashboard) == 5);
  static_assert(static_cast<uint8_t>(InxRecentLayout::Count) == 6);
  static_assert(static_cast<uint8_t>(InxRecentLayout::Flow) == 0);

  // BASE must stay unbanded: only CPY reserves a main-tab title band.
  static_assert(BaseMetrics::values.mainTabTitleHeight == 0);

  // CPY signature values the frozen design depends on.
  static_assert(CpyMetrics::values.mainTabTitleHeight == (UiHighDpiProfile::enabled ? 88 : 64));
  // The high-DPI pill must fit the three-digit worst case in its cavity
  // (measured: "100" = 46px in UI_10, cavity = width - 6).
  static_assert(CpyMetrics::values.batteryWidth == (UiHighDpiProfile::enabled ? 64 : 46));
  static_assert(CpyMetrics::values.batteryHeight == 26);
  static_assert(CpyMetrics::values.listRowGap == 0);
  static_assert(CpyMetrics::values.listRowRadius == 0);
  static_assert(CpyMetrics::values.listSelectionStyle == 0);  // InvertFill
  static_assert(CpyMetrics::values.capsuleRadius == 255);
  static_assert(CpyMetrics::values.mainTabTitleHeight > 0);

  std::puts("CPY theme constants: layout enum, title band and metric signature pass");
}
'''

with tempfile.TemporaryDirectory(prefix='cpyconsts-') as tmp:
    src = Path(tmp) / 'check.cpp'
    src.write_text(program)
    binary = Path(tmp) / 'check'
    subprocess.run(['c++', '-std=c++20', '-I', str(ROOT / 'src'), str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
