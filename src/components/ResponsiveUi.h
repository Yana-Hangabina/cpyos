#pragma once

#include <algorithm>

// Shared breakpoints for CpyOS layouts rendered in the portrait UI frame.
// Panels are described by the oriented content box (the space left after the
// tab/status bands), not by the panel's native landscape dimensions: a
// 800x480 Metalio and a 768x552 EEGO A4 both present a 480/552 px wide content
// column once the UI rotates them.
//
// Keep new pages on these predicates instead of introducing per-board
// `#ifdef FREEINK_DEVICE_*` branches. A page that needs its own geometry gets a
// host test (see src/home_dashboard_layout) with the compact and standard
// viewports listed in docs/engineering/small-screen-targets.md.
namespace responsive_ui {

inline constexpr int compactWidth = 560;
// The CPY home dashboard's standard cards need ~852 px of height; the update
// row adds 108 px.
inline constexpr int standardDashboardHeight = 900;
inline constexpr int standardDashboardHeightWithExtraCard = 1000;

constexpr bool isCompact(int width, int height, const bool extraCard = false) {
  return width < compactWidth || height < (extraCard ? standardDashboardHeightWithExtraCard
                                                     : standardDashboardHeight);
}

// Shrinks a design-space value to the viewport while keeping a floor, for
// callers that only need a proportional size (cover heights, gutters).
constexpr int fit(const int design, const int available, const int floor) {
  return std::max(floor, std::min(design, available));
}

}  // namespace responsive_ui
