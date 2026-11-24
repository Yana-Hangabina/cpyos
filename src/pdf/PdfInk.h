#pragma once

#include <cstddef>
#include <cstdint>

// Did a degraded render actually put anything on the page?
//
// The fallback ladder ends with text-only passes (images skipped).  On a scan
// the images ARE the content, so that pass succeeds and writes a white sheet --
// a silent blank page, which is the one outcome the failure card exists to
// prevent.  The rule only applies to those passes: a genuinely blank page in a
// book never gets there, because the normal attempts render it fine.

namespace pdf_ink {

// Fraction of the page that differs from its paper tone, from the same
// histogram stretchContrast() already builds.  A flat page reports 0.
constexpr float kMinInk = 5e-4f;  // 0.05% of the page

inline bool blank(const float inkRatio) { return inkRatio < kMinInk; }

// The ratio itself, kept here so it has a host test: `low`/`high` are the
// percentile clip points stretchContrast() found, and the answer is the share of
// samples below their midpoint.  A page with no paper/ink separation (low+24 >
// high, i.e. one tone) has no ink to count -- reporting "everything is below the
// midpoint" there would call a blank scan a fully inked page.
inline float inkFromHistogram(const size_t* hist, const unsigned low, const unsigned high,
                              const size_t total) {
  if (total == 0) return 0.0f;
  if (high < low + 24u) return 0.0f;
  const unsigned mid = (low + high) / 2;
  size_t dark = 0;
  for (unsigned v = 0; v <= mid; ++v) dark += hist[v];
  return static_cast<float>(dark) / static_cast<float>(total);
}

}  // namespace pdf_ink
