#pragma once

// The two rules reflow's page build depends on, kept away from the renderer so
// they can be tested without a device:
//
//   * AdvanceMemo -- measure each distinct codepoint once per paragraph.
//   * fromWhole   -- the factor between the sum of per-character advances and
//                    the width the drawn string actually has.
//
// Both look optional and both were measured to matter.  Without the memo a CJK
// paragraph called `getTextWidth(font, oneChar)` once per character: a
// 3,913-codepoint paragraph cost 9,978 ms of a 12 s page build, and the page's
// total measure time was ~1.7 s per paragraph.  With it the same paragraph costs
// 20 ms -- *including* the whole-string call below, so the exact factor is cheap
// and there is no reason to estimate it.
//
// Without the factor, lines fit the model but overflow the panel: a
// one-character measurement and the drawn string resolve different metrics
// (594 px vs 682 px measured for one line).
//
// The reason is now known from the drawing code, not inferred: EpdFont::draw
// advances by `fp4::toPixel(previousAdvanceFP + kern(prev, cp))` -- the advance
// and the kern towards the *next* glyph are summed in 12.4 fixed point and
// rounded ONCE -- and it substitutes ligatures before measuring.  So the drawn
// width is not "sum of per-character pixel widths plus per-pair pixel kerns":
// the rounding is per pair and the glyph count can differ from the codepoint
// count.  That is why the measured scale moves with the script mix (1.09-1.20)
// rather than being a constant, and why a hand-rolled kern sum in the wrapper
// would be wrong by up to half a pixel per pair -- changing where lines break.
// Keep the exact measurement.
//
// What is deliberately NOT here is a sampling rule.  An earlier revision of this
// file carried one, built to dodge a whole-string measurement that turned out to
// be cheap once the memo existed.  It also needed a third per-codepoint array,
// and per-codepoint arrays in `buildReflow()` are *memory*, not free memory:
// after one page turn the device sat at `Free 10155 / MaxAlloc 2932` internal
// and never recovered.  Measure the exact value; do not add an array to it.

#include <cstddef>
#include <cstdint>

namespace reflow_metrics {

// Codepoint -> advance for one paragraph.
//
// A full or colliding table is not an error: `find` returns false and the caller
// measures again.  It can never return another codepoint's width, because every
// probe compares the key before reading the value.
struct AdvanceMemo {
  static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
  // 512 slots x (4 + 2) bytes of stack.  Small on purpose: this is built on the
  // render task, whose stack was already the site of a canary crash.
  static constexpr unsigned kSlots = 512;

  uint32_t keys[kSlots];
  int16_t values[kSlots];
  unsigned used = 0;

  AdvanceMemo() {
    for (unsigned i = 0; i < kSlots; ++i) keys[i] = kEmpty;
  }

  static constexpr unsigned slotOf(const uint32_t cp) {
    return static_cast<unsigned>((cp * 2654435761u) >> 23) & (kSlots - 1);
  }

  bool find(const uint32_t cp, int& out) const {
    const unsigned start = slotOf(cp);
    for (unsigned probe = 0; probe < 8; ++probe) {
      const unsigned slot = (start + probe) & (kSlots - 1);
      if (keys[slot] == kEmpty) return false;
      if (keys[slot] == cp) {
        out = values[slot];
        return true;
      }
    }
    return false;  // an unprobed collision: measure, do not guess
  }

  void insert(const uint32_t cp, const int width) {
    if (cp == kEmpty) return;
    if (used * 4u >= kSlots * 3u) return;  // load factor 0.75: stop caching, stay correct
    if (width < -32768 || width > 32767) return;  // never truncate into a wrong advance
    const unsigned start = slotOf(cp);
    for (unsigned probe = 0; probe < 8; ++probe) {
      const unsigned slot = (start + probe) & (kSlots - 1);
      if (keys[slot] == kEmpty) {
        keys[slot] = cp;
        values[slot] = static_cast<int16_t>(width);
        ++used;
        return;
      }
      if (keys[slot] == cp) return;
    }
  }
};

// A scale outside this band is not a metrics difference, it is a broken
// measurement (an empty paragraph, a face that resolved differently for the
// string, a zero sum).  Falling back to 1.0 keeps lines *conservative* instead
// of trusting garbage.
constexpr float kMinPlausibleScale = 0.5f;
constexpr float kMaxPlausibleScale = 2.0f;

inline bool plausible(const float scale) {
  return scale >= kMinPlausibleScale && scale <= kMaxPlausibleScale;
}

// `wholeWidth` is the drawn width of the paragraph; `advanceSum` the sum of the
// per-character advances measured for it.
inline float fromWhole(const int wholeWidth, const int advanceSum) {
  if (advanceSum <= 0 || wholeWidth <= 0) return 1.0f;
  const float scale = static_cast<float>(wholeWidth) / advanceSum;
  return plausible(scale) ? scale : 1.0f;
}


}  // namespace reflow_metrics
