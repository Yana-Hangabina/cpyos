// Portrait touch coordinates -> normalized panel frame (CPYOS convention).
//
// The vendor FT6336U driver reports a 684x1216 portrait point. CPYOS's
// GfxRenderer::tapToLogical for Orientation::Portrait on a 1216x684 panel is
//   outX = (panelH-1) - ny*(panelH-1)
//   outY = nx*panelW
// so feeding the normalized pair below reproduces the raw portrait coordinates.
// Kept free of ESP-IDF includes so the transform has host tests.
#pragma once

namespace plus_touch {

inline constexpr int kPortraitWidth = 684;
inline constexpr int kPanelWidth = 1216;
inline constexpr int kPanelHeight = 684;

inline void portraitToPanel(int lx, int ly, float& nx, float& ny) {
  if (lx < 0) lx = 0;
  if (lx > kPortraitWidth - 1) lx = kPortraitWidth - 1;
  if (ly < 0) ly = 0;
  if (ly > kPanelWidth - 1) ly = kPanelWidth - 1;
  nx = static_cast<float>(ly) / static_cast<float>(kPanelWidth - 1);
  ny = static_cast<float>(kPanelHeight - 1 - lx) / static_cast<float>(kPanelHeight - 1);
}

}  // namespace plus_touch
