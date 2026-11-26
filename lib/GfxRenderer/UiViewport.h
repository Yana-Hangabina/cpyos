#pragma once

#include <algorithm>
#include <cstdint>

// Logical -> native coordinate contract for the UI layer.
//
// The renderer owns one viewport per display orientation. UI code keeps
// working in the viewport's design units; every drawing primitive maps its
// geometry through this value at the renderer boundary. Reader content and
// framebuffer operations stay in native pixels and never use this mapping.
//
// The scale is a rational percentage (1000 == 1.0). It is composed from:
//   * the theme's authored design size (reported by BaseTheme),
//   * the panel's oriented native size,
//   * the user's UI scale preference.
//
// This file is pure integer math so host tests can cover every boundary.
namespace ui_viewport {

struct DesignSpec {
  // Portrait design size. Zero means "native" (no logical mismatch); a theme
  // that wants DPI adaptation reports the pixels it was authored for.
  int16_t width = 0;
  int16_t height = 0;
};

struct Viewport {
  int16_t width = 0;         // design width (logical units)
  int16_t height = 0;        // design height
  int16_t nativeWidth = 0;   // oriented native width
  int16_t nativeHeight = 0;  // oriented native height
  uint16_t scalePermille = 1000;

  bool identity() const {
    return scalePermille == 1000 && width == nativeWidth && height == nativeHeight;
  }

  int toNative(int design) const {
    return static_cast<int>((static_cast<int64_t>(design) * scalePermille + (design < 0 ? -500 : 500)) / 1000);
  }

  int toDesign(int native) const {
    return static_cast<int>((static_cast<int64_t>(native) * 1000 + (native < 0 ? -scalePermille / 2 : scalePermille / 2)) /
                            scalePermille);
  }

  // Length mapping keeps a non-zero source at least one native pixel. A 1px
  // hairline must not disappear at 75%, and a 1px gap must not become zero.
  int lengthToNative(int designLength) const {
    if (designLength == 0) return 0;
    const int mapped = toNative(designLength);
    return designLength > 0 ? std::max(1, mapped) : std::min(-1, mapped);
  }

  int lengthToDesign(int nativeLength) const {
    if (nativeLength == 0) return 0;
    const int mapped = toDesign(nativeLength);
    return nativeLength > 0 ? std::max(1, mapped) : std::min(-1, mapped);
  }
};

inline uint16_t clampUserPercent(uint16_t percent) { return std::clamp<uint16_t>(percent, 50, 200); }

inline Viewport resolve(const DesignSpec& spec, const int nativeWidth, const int nativeHeight,
                        const uint16_t userPercent) {
  Viewport v;
  v.nativeWidth = static_cast<int16_t>(std::max(1, nativeWidth));
  v.nativeHeight = static_cast<int16_t>(std::max(1, nativeHeight));

  int designWidth = spec.width;
  int designHeight = spec.height;
  if (designWidth > 0 && designHeight > 0) {
    // A portrait design spec is reused in landscape by swapping its axes, so
    // one theme does not need two tables.
    if ((v.nativeWidth > v.nativeHeight) != (designWidth > designHeight)) {
      std::swap(designWidth, designHeight);
    }
  } else {
    designWidth = v.nativeWidth;
    designHeight = v.nativeHeight;
  }

  const int64_t fitW = static_cast<int64_t>(v.nativeWidth) * 1000 / designWidth;
  const int64_t fitH = static_cast<int64_t>(v.nativeHeight) * 1000 / designHeight;
  int fit = static_cast<int>(std::min(fitW, fitH));
  fit = std::clamp(fit, 500, 2000);

  const int scaled = fit * static_cast<int>(clampUserPercent(userPercent)) / 100;
  v.scalePermille = static_cast<uint16_t>(std::clamp(scaled, 500, 2000));
  v.width = static_cast<int16_t>(std::clamp<int64_t>(static_cast<int64_t>(v.nativeWidth) * 1000 / v.scalePermille,
                                                     1, 32767));
  v.height = static_cast<int16_t>(std::clamp<int64_t>(static_cast<int64_t>(v.nativeHeight) * 1000 / v.scalePermille,
                                                      1, 32767));
  return v;
}

}  // namespace ui_viewport
