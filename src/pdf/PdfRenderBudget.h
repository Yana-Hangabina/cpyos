#pragma once

// Working-memory margins for the render ladder, derived from the PSRAM a device
// actually has instead of hard-coded for this one.
//
// The numbers below were measured on the 8 MB Read Pico and are what the ladder
// has always used there; `forPsram()` must return exactly them for that device
// (the host test asserts it), so adding portability does not change behaviour
// here.  On a smaller-PSRAM board the same rules scaled naively would be
// impossible -- a 4 MB "reopen the document" reserve on a 2 MB part means every
// page pays the 0.6 s reopen, and a 3 MB strip gate means the strip fallback
// never runs exactly where it is needed most.
//
// Scaling law, deliberately simple: the big device keeps its measured margins;
// below 6 MB everything is a fraction of the heap, so each value stays possible
// by construction.

#include <cstdint>

namespace pdf_render_budget {

struct Budget {
  uint32_t fullPageSlack;   // free space a whole-page pass wants beyond its pixmap
  uint32_t pixmapSlack;     // contiguous block a whole-page pass wants beyond its pixmap
  uint32_t prepReserve;     // free space below which the document is reopened first
  uint32_t stripGate;       // largest-block level below which the strip fallback runs
};

inline Budget forPsram(const uint32_t totalPsramBytes) {
  // The measured configuration.  Kept as one branch rather than a formula so the
  // 8 MB values cannot drift with arithmetic.
  if (totalPsramBytes >= (6u << 20)) {
    return {1536u << 10, 256u << 10, 4u << 20, 3u << 20};
  }
  const uint32_t eighth = totalPsramBytes / 8;
  return {eighth, eighth / 4, totalPsramBytes / 2, totalPsramBytes / 3};
}

}  // namespace pdf_render_budget
