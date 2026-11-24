#pragma once

#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

// Reader font size is stored as an actual point size (see CrossPointSettings::
// fontPointSize), not an abstract Small/Medium/Large slot. The selectable sizes
// therefore come from whichever family is active: the built-in or vector set
// below, or the .cpfont files a user installed for an SD family.

// One curated ladder for every scalable face (the built-in system pack and
// vector TTF/OTF/TTC families). At 150 DPI the weights map to ~21..83 px;
// 40pt leaves roughly ten lines on Read Pico / Plus (684x1216), which is the
// large-print end. The ladder keeps every legacy reader point
// (so upgrades do not move an existing setting) plus 36/40pt; 15 steps is
// still short enough to scan on the device.
inline constexpr uint8_t READER_POINT_SIZES[] = {10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 40};
inline constexpr size_t READER_POINT_SIZE_COUNT = std::size(READER_POINT_SIZES);
inline constexpr uint8_t READER_MIN_POINT_SIZE = READER_POINT_SIZES[0];
inline constexpr uint8_t READER_MAX_POINT_SIZE = READER_POINT_SIZES[READER_POINT_SIZE_COUNT - 1];

// The absolute ladder is device-independent; what a particular panel should
// offer is derived from its portrait height. Read Pico / Plus are 1216 px tall
// and get the full ladder; a 3.97" A4-class panel (~768 px) tops out at the
// 26pt step, and an 800 px panel also at 26pt. This is deliberately not a
// setting: it is the same kind of compile/board-derived bound as the content
// box in ResponsiveUi. A new panel only needs its BoardConfig height.
struct ReaderPointRange {
  uint8_t min;
  uint8_t max;
};

inline ReaderPointRange readerPointRangeForPortraitHeight(const uint16_t portraitHeight) {
  constexpr uint16_t kReferenceHeight = 1216;  // Read Pico / Plus
  // Ceil the proportional maximum so a 768 px panel reaches 26pt rather than
  // falling through to 24pt, then snap down to a real ladder point.
  int target = (static_cast<int>(portraitHeight) * READER_MAX_POINT_SIZE + kReferenceHeight - 1) / kReferenceHeight;
  if (target < 18) target = 18;
  uint8_t maxPt = READER_MIN_POINT_SIZE;
  for (const uint8_t pt : READER_POINT_SIZES) {
    if (pt <= target) maxPt = pt;
  }
  return {READER_MIN_POINT_SIZE, maxPt};
}

// The range for the active board. Implemented in the .cpp so this header stays
// usable by host tests that have no BoardConfig.
ReaderPointRange readerPointRange();

// Snap a stored/requested size into the active board's range and ladder. Used
// by settings migration and by the reader when an old setting came from a
// different panel.
uint8_t readerClampPointSizeToDevice(uint8_t pt);

// The unified firmware keeps one offline reader fallback. The bitmap profile
// still has exactly one size; the scalable system pack can drive every size the
// reader asks for, so only the *selectable* list grows. The default stays 12pt
// (CrossPointSettings::DEFAULT_FONT_POINT_SIZE) — this list must not make the
// out-of-box text larger.
#if defined(CROSSPOINT_SYSTEM_FONT_PACK) && defined(BOARD_HAS_PSRAM)
inline constexpr uint8_t BUILTIN_READER_POINT_SIZES[] = {10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 40};
// A fixed .cpfont family can borrow the built-in scalable face for sizes it
// does not ship. No-PSRAM targets keep one small internal-RAM face and cannot
// fund 40pt glyphs, so the fallback is deliberately PSRAM-only.
inline constexpr bool READER_HAS_SCALABLE_FALLBACK = true;
#elif defined(CROSSPOINT_SYSTEM_FONT_PACK)
// No PSRAM: the pack keeps one small internal-RAM face, so cap the reader menu
// at the sizes the old bitmap profile exposed.
inline constexpr uint8_t BUILTIN_READER_POINT_SIZES[] = {10, 12, 14, 16, 18};
inline constexpr bool READER_HAS_SCALABLE_FALLBACK = false;
#else
inline constexpr uint8_t BUILTIN_READER_POINT_SIZES[] = {12};
inline constexpr bool READER_HAS_SCALABLE_FALLBACK = false;
#endif

// Vector (.ttf/.otf/.ttc) fonts rasterize on demand, so they use the same
// curated ladder as the scalable pack. On a no-PSRAM build keep the compact
// set: a 40pt CJK face cannot fit the glyph cache that target can fund.
#if defined(BOARD_HAS_PSRAM)
inline constexpr uint8_t VECTOR_READER_POINT_SIZES[] = {10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 40};
#else
inline constexpr uint8_t VECTOR_READER_POINT_SIZES[] = {10, 12, 14, 16, 18};
#endif

// Merge a fixed bitmap family's native sizes with the device range endpoints.
// The fixed family keeps its exact sizes; the panel minimum/maximum are added
// only when the file does not cover them, so the list stays short (a 12/14/16/18
// family becomes 10 + those four + the panel maximum). Intermediate fallback
// points are available by choosing a scalable family in the Font tab; putting
// every ladder point here would turn a four-size .cpfont into a fifteen-row
// picker. Empty/out-of-range native input returns the full scalable ladder.
// The result is sorted and duplicate-free.
inline std::vector<uint8_t> mergeReaderPointSizes(const std::vector<uint8_t>& native, const uint8_t* scalable,
                                                  const size_t scalableCount, const uint8_t minPt,
                                                  const uint8_t maxPt) {
  std::vector<uint8_t> filtered;
  filtered.reserve(native.size());
  for (const uint8_t pt : native) {
    if (pt >= minPt && pt <= maxPt) filtered.push_back(pt);
  }
  std::sort(filtered.begin(), filtered.end());
  filtered.erase(std::unique(filtered.begin(), filtered.end()), filtered.end());

  std::vector<uint8_t> out = filtered;
  if (filtered.empty()) {
    for (size_t i = 0; i < scalableCount; ++i) {
      if (scalable[i] >= minPt && scalable[i] <= maxPt) out.push_back(scalable[i]);
    }
  } else {
    if (minPt < filtered.front()) out.push_back(minPt);
    if (maxPt > filtered.back()) out.push_back(maxPt);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// Point sizes selectable for the active reader font, ascending: the vector set
// or installed .cpfont sizes for a known SD family, otherwise the built-in set.
// Fixed .cpfont families get the scalable tail only on PSRAM builds where the
// system pack can actually render it. Never returns empty.
std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName);

// True when `pt` is a size the selected .cpfont file actually ships. Built-in
// and vector families are always native (they rasterize any ladder point).
// Callers use this to mark fallback rows and to switch to the built-in face
// instead of silently snapping to a smaller bitmap.
bool readerFontPointSizeIsNative(const SdCardFontRegistry* registry, const char* sdFamilyName, uint8_t pt);

// True when the active fixed family has at least one ladder point it cannot
// render itself, i.e. some rows will use the built-in scalable face.
bool readerFontHasSystemSizeFallback(const SdCardFontRegistry* registry, const char* sdFamilyName);

// Closest entry in `sizes` (ascending, `count` > 0) to `pt`; ties resolve to the
// smaller size. Takes a raw range rather than a vector because getReaderFontId()
// runs inside the page render loop and must not allocate.
uint8_t snapToNearestPointSize(const uint8_t* sizes, size_t count, uint8_t pt);

inline uint8_t snapToNearestPointSize(const std::vector<uint8_t>& sizes, const uint8_t pt) {
  return sizes.empty() ? pt : snapToNearestPointSize(sizes.data(), sizes.size(), pt);
}
