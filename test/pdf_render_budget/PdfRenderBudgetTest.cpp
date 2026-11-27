// Pins two things: the 8 MB device keeps the measured constants exactly, and
// every value a smaller board computes is actually attainable on that board.
#include "PdfRenderBudget.h"

#include <cstdio>
#include <initializer_list>

using namespace pdf_render_budget;

static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    ++checks;                                                                 \
    if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__);        \
                   std::printf(__VA_ARGS__); std::printf("\n"); }             \
  } while (0)

int main() {
  // Read Pico: 8,373,840 bytes of PSRAM measured by the firmware.
  const Budget big = forPsram(8373840u);
  CHECK(big.fullPageSlack == 1536u * 1024, "full-page slack changed on the 8 MB device");
  CHECK(big.pixmapSlack == 256u * 1024, "pixmap slack changed on the 8 MB device");
  CHECK(big.prepReserve == 4u * 1024 * 1024, "prep reserve changed on the 8 MB device");
  CHECK(big.stripGate == 3u * 1024 * 1024, "strip gate changed on the 8 MB device");

  // The threshold itself: at exactly 6 MB the measured branch still applies.
  const Budget edge = forPsram(6u << 20);
  CHECK(edge.prepReserve == 4u * 1024 * 1024, "the 6 MB boundary fell to the scaled branch");

  // Below it, nothing may exceed the heap it describes.
  for (const uint32_t total : {2u << 20, 4u << 20, (6u << 20) - 1}) {
    const Budget b = forPsram(total);
    CHECK(b.prepReserve <= total, "prep reserve %u exceeds a %u-byte heap", b.prepReserve, total);
    CHECK(b.stripGate <= total, "strip gate %u exceeds a %u-byte heap", b.stripGate, total);
    CHECK(b.fullPageSlack <= total, "full-page slack %u exceeds a %u-byte heap", b.fullPageSlack, total);
    CHECK(b.pixmapSlack <= b.fullPageSlack, "the contiguous slack exceeds the free-space slack");
    CHECK(b.pixmapSlack > 0, "the contiguous slack collapsed to zero");
  }

  // A 2 MB board must still be able to run the strip fallback: the gate has to
  // be small enough that fragmentation can actually cross it.
  const Budget small = forPsram(2u << 20);
  CHECK(small.stripGate < (2u << 20), "the strip gate is unreachable on a 2 MB board");

  // Scaling, not just boundedness: every margin must track the heap it describes,
  // so a smaller board is never handed a larger-board budget (mutation 2 lived
  // through a bounds-only test exactly that way).
  CHECK(small.fullPageSlack == (2u << 20) / 8, "full-page slack is not total/8 on a 2 MB board");
  CHECK(small.prepReserve == (2u << 20) / 2, "prep reserve is not total/2 on a 2 MB board");
  {
    const Budget two = forPsram(2u << 20);
    const Budget three = forPsram(3u << 20);
    const Budget five = forPsram(5u << 20);
    CHECK(two.fullPageSlack < three.fullPageSlack && three.fullPageSlack < five.fullPageSlack,
          "full-page slack does not grow with the heap");
    CHECK(two.prepReserve < three.prepReserve && three.prepReserve < five.prepReserve,
          "prep reserve does not grow with the heap");
    CHECK(two.stripGate < five.stripGate, "strip gate does not grow with the heap");
  }

  std::printf("pdf render budget: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
