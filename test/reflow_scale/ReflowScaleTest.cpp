// Pins the rule that decides when reflow may sample the width scale instead of
// measuring the whole paragraph.  Everything here is about the *decision*, not
// the renderer: the renderer's job (measure a slice) is verified on device, and
// the cost this rule trades against is measured there too.
#include "ReflowScale.h"

#include <cstdio>
#include <vector>

using namespace reflow_metrics;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                              \
  do {                                                                \
    ++checks;                                                          \
    if (!(cond)) {                                                    \
      ++failures;                                                      \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
      std::printf(__VA_ARGS__);                                        \
      std::printf("\n");                                                \
    }                                                                 \
  } while (0)



int main() {
  // 9. The advance memo: a repeated codepoint is answered from the table.
  {
    AdvanceMemo memo;
    int out = 0;
    CHECK(!memo.find(0x8BC1, out), "an empty table answered a lookup");
    memo.insert(0x8BC1, 45);
    CHECK(memo.find(0x8BC1, out) && out == 45, "a stored advance was not found (out=%d)", out);
    CHECK(!memo.find(0x8BC2, out), "a different codepoint hit the stored slot's value");
  }

  // 10. Collisions must never return *another* codepoint's width.  Two keys that
  //     share the 9-bit slot are enough to prove the key comparison happens.
  {
    AdvanceMemo memo;
    // slotOf() is (cp * 2654435761) >> 23 & 511; these differ only in bits that
  // the shift discards for the slot, i.e. they collide by construction.
    const uint32_t a = 0x00004E00u;
    uint32_t b = a + 1;
    while (AdvanceMemo::slotOf(b) != AdvanceMemo::slotOf(a) && b < a + 4096) ++b;
    CHECK(AdvanceMemo::slotOf(a) == AdvanceMemo::slotOf(b),
          "no colliding pair found below +4096 (a=%u b=%u)", a, b);
    memo.insert(a, 30);
    int out = 0;
    CHECK(memo.find(b, out) == false, "a colliding key read the first key's width");
    memo.insert(b, 31);
    CHECK(memo.find(a, out) && out == 30, "the first key lost its width after a collision (out=%d)", out);
    CHECK(memo.find(b, out) && out == 31, "the colliding key was not stored (out=%d)", out);
  }

  // 11. The load factor stops caching rather than corrupting: past 3/4 the table
  //     silently declines inserts, and lookups stay correct for what is in it.
  {
    AdvanceMemo memo;
    for (unsigned i = 0; i < AdvanceMemo::kSlots; ++i) {
      memo.insert(0x100u + i * 7u, static_cast<int>(i + 1));
    }
    CHECK(memo.used * 4u <= AdvanceMemo::kSlots * 3u,
          "the table filled past its load factor (used=%u)", memo.used);
    int out = 0;
    CHECK(memo.find(0x100u, out) && out == 1, "an early key was not retrievable (out=%d)", out);
  }

  // 12. A width that does not fit int16 is dropped, not truncated: a truncated
  //     advance would silently change where a line breaks.
  {
    AdvanceMemo memo;
    memo.insert(0x4E00, 70000);
    int out = 0;
    CHECK(!memo.find(0x4E00, out), "a 70000 px advance was truncated into the table");
    memo.insert(AdvanceMemo::kEmpty, 12);
    CHECK(!memo.find(AdvanceMemo::kEmpty, out), "the empty key was stored as a real codepoint");
  }

  // 13. The memo's *purpose*: repeated text must not re-measure.  This is the
  //     10 s -> 2.4 s saving, expressed as a property of the table rather than a
  //     log line, so a regression is caught without a device.
  {
    AdvanceMemo memo;
    unsigned calls = 0;
    int out = 0;
    for (unsigned i = 0; i < 2000; ++i) {
      const uint32_t cp = 0x4E00u + (i % 40u);  // 40 distinct glyphs, 2000 uses
      if (!memo.find(cp, out)) {
        ++calls;
        memo.insert(cp, static_cast<int>(cp & 0x1F));
      }
    }
    CHECK(calls == 40u, "a repeating paragraph cost %u measurements (want 40)", calls);
  }

  // 14. The plausibility band: a scale that far off is a broken measurement,
  //     not a face, and trusting it would silently reshape every line.
  {
    CHECK(!plausible(0.49f), "0.49 should be implausible");
    CHECK(plausible(0.5f), "0.5 is the documented lower bound");
    CHECK(plausible(2.0f), "2.0 is the documented upper bound");
    CHECK(!plausible(2.01f), "2.01 should be implausible");
    CHECK(fromWhole(3000, 600) == 1.0f, "a 5x ratio is garbage, not a face");
    CHECK(fromWhole(6, 600) == 1.0f, "a 0.01x ratio is garbage, not a face");
    CHECK(fromWhole(0, 600) == 1.0f, "a zero width must not scale the page to nothing");
    CHECK(fromWhole(690, 0) == 1.0f, "a zero sum must not divide");
    CHECK(fromWhole(-5, 600) == 1.0f, "a negative width must not invert the layout");
    CHECK(fromWhole(690, 600) > 1.149f && fromWhole(690, 600) < 1.151f, "690/600 should be 1.15");
  }

  std::printf("reflow scale: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
