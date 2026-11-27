#pragma once

#include <cstddef>
#include <cstdint>

#include "MemoryLedgerRules.h"

namespace memory {
namespace ledger {

// True only when this build actually passes the --wrap flags this file's
// definitions need. Everything else answers 0 and the dump says so; an empty
// table that reads like an idle system is the failure mode this whole module
// exists to avoid.
bool wired();

// Clears the table, records the baseline watermarks and switches recording on.
// Called once from setup(), before any UI or network work, so the baseline is a
// real "system at rest" number rather than a mid-session one.
void begin();

struct Live {
  uint32_t caller;
  uint32_t bytes;
  uint32_t blocks;
};

// Most-heavy callers first, up to `cap`. Returns the number written.
size_t snapshot(Live* out, size_t cap);

Counters counters();
Watermark baseline();
Watermark measured();
CrossCheck check(uint32_t toleranceBytes);

// Bytes attributed to a region, for the dump header.
uint32_t attributed(Region region);

}  // namespace ledger
}  // namespace memory
