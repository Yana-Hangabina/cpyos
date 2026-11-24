#include "ReaderFontSizes.h"

#include <BoardConfig.h>

#include <algorithm>
#include <iterator>
#include <vector>

namespace {

std::vector<uint8_t> filterReaderPointSizes(const std::vector<uint8_t>& sizes, const ReaderPointRange range) {
  std::vector<uint8_t> out;
  out.reserve(sizes.size());
  for (const uint8_t pt : sizes) {
    if (pt >= range.min && pt <= range.max) out.push_back(pt);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

const SdCardFontFamilyInfo* activeFixedFamily(const SdCardFontRegistry* registry, const char* sdFamilyName) {
  if (!registry || !sdFamilyName || sdFamilyName[0] == '\0') return nullptr;
  const auto* family = registry->findFamily(sdFamilyName);
  return family && !family->vector ? family : nullptr;
}

}  // namespace

std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName) {
  const ReaderPointRange range = readerPointRange();
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') {
    if (const auto* family = registry->findFamily(sdFamilyName)) {
      if (family->vector) {
        return filterReaderPointSizes(
            std::vector<uint8_t>(std::begin(VECTOR_READER_POINT_SIZES), std::end(VECTOR_READER_POINT_SIZES)),
            range);
      }
      const auto native = family->availableSizes();
      if (!native.empty()) {
        if constexpr (READER_HAS_SCALABLE_FALLBACK) {
          // A fixed .cpfont keeps its exact bitmap sizes. The ladder only adds
          // points outside the file's range, where the built-in scalable face
          // takes over (the UI marks these rows and switches family on select).
          auto merged = mergeReaderPointSizes(native, READER_POINT_SIZES, READER_POINT_SIZE_COUNT, range.min,
                                              range.max);
          if (!merged.empty()) return merged;
        } else {
          auto filtered = filterReaderPointSizes(native, range);
          if (!filtered.empty()) return filtered;
        }
        return native;
      }
    }
  }
  return filterReaderPointSizes(
      std::vector<uint8_t>(std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES)),
      range);
}

bool readerFontPointSizeIsNative(const SdCardFontRegistry* registry, const char* sdFamilyName, const uint8_t pt) {
  const auto* family = activeFixedFamily(registry, sdFamilyName);
  if (!family) return true;  // built-in or vector: every ladder point is native
  return family->findFile(pt, 0) != nullptr;
}

bool readerFontHasSystemSizeFallback(const SdCardFontRegistry* registry, const char* sdFamilyName) {
  if constexpr (!READER_HAS_SCALABLE_FALLBACK) {
    (void)registry;
    (void)sdFamilyName;
    return false;
  } else {
    const auto* family = activeFixedFamily(registry, sdFamilyName);
    if (!family) return false;
    const ReaderPointRange range = readerPointRange();
    for (size_t i = 0; i < READER_POINT_SIZE_COUNT; ++i) {
      const uint8_t pt = READER_POINT_SIZES[i];
      if (pt < range.min || pt > range.max) continue;
      if (family->findFile(pt, 0) == nullptr) return true;
    }
    return false;
  }
}

ReaderPointRange readerPointRange() {
  const uint16_t portraitHeight =
      std::max<uint16_t>(BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
  return readerPointRangeForPortraitHeight(portraitHeight);
}

uint8_t readerClampPointSizeToDevice(const uint8_t pointSize) {
  const ReaderPointRange range = readerPointRange();
  const uint8_t clamped = std::clamp(pointSize, range.min, range.max);
  uint8_t snapped = snapToNearestPointSize(READER_POINT_SIZES, READER_POINT_SIZE_COUNT, clamped);
  if (snapped < range.min) snapped = range.min;
  if (snapped > range.max) snapped = range.max;
  return snapped;
}

uint8_t snapToNearestPointSize(const uint8_t* sizes, const size_t count, const uint8_t pt) {
  if (!sizes || count == 0) return pt;

  uint8_t best = sizes[0];
  uint8_t bestDelta = best > pt ? best - pt : pt - best;
  for (size_t i = 1; i < count; i++) {
    const uint8_t delta = sizes[i] > pt ? sizes[i] - pt : pt - sizes[i];
    // Strictly-less keeps the smaller size on a tie, since `sizes` is ascending.
    if (delta < bestDelta) {
      best = sizes[i];
      bestDelta = delta;
    }
  }
  return best;
}
