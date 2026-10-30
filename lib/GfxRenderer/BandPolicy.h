#pragma once

#include <cstdint>

// Ghost-debt arithmetic for the periodic GC16 sweep and the local clean.
//
// Kept dependency-free so `src/band_policy/run_band_policy_test.sh` can pin the
// rules the device actually runs:
//   - only a full-page pass (or a band covering >= kBandDebtPercent of the panel)
//     advances the debt counter, because a narrow band barely drives anything;
//   - the sweep is due on the pass where the counter would reach the interval,
//     and the caller must decide that BEFORE offering the pass to the band path,
//     so the pass that must clean is never downgraded to a band;
//   - with a local clean enabled, the clean covers the union of everything driven
//     since the last one, and only when that union stays below the threshold;
//     otherwise the pass becomes a normal whole-screen GC16.
namespace band_policy {

// A band that drives less than this share of the panel adds no meaningful ghost
// debt (a cursor row, the clock strip).
inline constexpr long kBandDebtPercent = 10;
// A band larger than this share is not worth a windowed push at all.
inline constexpr long kMaxBandPercent = 55;
// The dirty union above this share is cleaned with a whole-screen GC16 instead.
inline constexpr long kMaxLocalCleanPercent = 60;

inline bool sweepDue(const int count, const int interval) { return count + 1 >= interval; }

inline bool bandChargesDebt(const long bandArea, const long screenArea) {
  return screenArea > 0 && bandArea * 100 >= screenArea * kBandDebtPercent;
}

inline bool bandFitsWindow(const long bandArea, const long screenArea) {
  return screenArea > 0 && bandArea * 100 <= screenArea * kMaxBandPercent;
}

// `maxPercent` is a parameter so the photo A/B can sweep the threshold at runtime
// (CMD:CLEANMAX) without a rebuild; the shipped default is the constant above.
inline bool localCleanFits(const long unionArea, const long screenArea,
                           const long maxPercent = kMaxLocalCleanPercent) {
  return screenArea > 0 && unionArea * 100 <= screenArea * maxPercent;
}

// A rect clean is dilated by this many pixels on every side. A rect edge can sit
// exactly on the last driven pixel, where charge sharing with the untouched
// neighbour is most likely to leave a faint boundary; growing the clean into
// already-clean pixels costs a few columns and removes that risk. 8 px also keeps
// the result byte-aligned.
inline constexpr long kCleanDilatePx = 8;

// Grow (x,y,w,h) by `margin` on each side, clamp to the panel and round x/width
// out to whole 8-pixel bytes so the result satisfies the windowed-push invariant.
struct Rect {
  int x;
  int y;
  int w;
  int h;
};

inline Rect dilateRect(const Rect in, const int screenW, const int screenH, const long margin = kCleanDilatePx) {
  Rect out = in;
  if (out.w <= 0 || out.h <= 0 || screenW <= 0 || screenH <= 0) return Rect{0, 0, 0, 0};
  int x0 = out.x - static_cast<int>(margin);
  int y0 = out.y - static_cast<int>(margin);
  int x1 = out.x + out.w + static_cast<int>(margin);
  int y1 = out.y + out.h + static_cast<int>(margin);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > screenW) x1 = screenW;
  if (y1 > screenH) y1 = screenH;
  x0 &= ~7;
  x1 = (x1 + 7) & ~7;
  if (x1 > screenW) x1 = screenW;
  out.x = x0;
  out.y = y0;
  out.w = x1 - x0;
  out.h = y1 - y0;
  if (out.w <= 0 || out.h <= 0) return Rect{0, 0, 0, 0};
  return out;
}

}  // namespace band_policy
