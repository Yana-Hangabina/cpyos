#pragma once

#include <cstddef>
#include <cstdint>

// The decision half of the allocation ledger: what gets recorded, where it is
// recorded, and how the recorded totals are judged against the measured heaps.
// Kept free of HAL/ESP includes so a host test can enumerate every combination
// (test/memory_ledger/), the same split that FontRestorePolicy.h and
// FilePublishRules.h use.
//
// Why a ledger at all: four separate incidents were the same shape -- something
// held internal RAM or a contiguous block and no log line could say who. The
// allocator that quietly used internal RAM instead of PSRAM, the 40 font switches
// that took the largest PSRAM block from 2.75 MB to 1.18 MB, the TLS handshake
// that died at largest=7156 while 19 KB was free, and the render-memory reclaim
// that never gave the fonts back. Each was diagnosed by hand after the fact from
// a heap dump with no owner column. This adds the column. It records and never
// refuses: an allocation decision made here would be the 0.7.x allocator bug
// again, only with more code behind it.
namespace memory::ledger {

// Blocks below this size pass through unrecorded. The floor is what keeps the
// wrapper off the small-block path (one compare for malloc, one probe for free)
// and keeps the 256-row table from thrashing.
//
// It started at 2 KiB and the first device dump answered for it: 43 live blocks
// at boot, 17 steals within one network session, and a `released` line for the UI
// script carrier that was really an eviction -- the block was alive, the row was
// gone. A table that evicts the things a reader wants to compare is worse than a
// smaller table with fewer things in it, and every incident this exists for
// involved a block of at least 32 KiB.
constexpr size_t kFloorBytes = 8u * 1024u;

// Direct-mapped table, power of two so the index is a shift. A collision is an
// observable event rather than a silent loss, which is the whole difference
// between a measurement and a guess.
constexpr size_t kSlots = 256;

// Where a live block physically sits. Decided from the returned pointer, not from
// the requested caps: malloc() has no caps, and the pointer is the truth about
// what the allocator actually did -- which is exactly the thing the 0.7.x bug got
// wrong when a component described its own allocation instead of observing it.
enum class Region : uint8_t {
  Internal,  // includes DMA-capable internal RAM
  Psram,
  Other,     // cannot happen on these targets; recorded rather than assumed away
};

enum class Action : uint8_t {
  Passthrough,  // below the floor, or before begin(): record nothing
  Insert,       // free slot
  Steal,        // slot occupied: the newer block wins, the evicted one becomes untracked
};

constexpr bool tracks(const size_t bytes) { return bytes >= kFloorBytes; }

constexpr size_t slotFor(const uintptr_t ptr) {
  // Multiplicative, not a shift-and-mask. The heap hands out neighbours at the
  // sizes this table cares about (2 KiB floor, 4 KiB chunks, 64 KiB font and
  // buffer blocks), and a plain `(ptr >> 8) & 255` maps every 4 KiB-spaced set
  // onto 16 rows -- the table would steal from itself while the system sat idle.
  // Knuth's multiplier spreads the same addresses over all rows; test/memory_ledger
  // pins the distribution for exactly those strides.
  const uint32_t mixed = static_cast<uint32_t>((ptr >> 8u) * 2654435761u);
  return mixed >> (32u - 8u);  // kSlots == 256
}

constexpr Region regionOf(const bool external, const bool internal) {
  if (external) return Region::Psram;
  if (internal) return Region::Internal;
  return Region::Other;
}

// The eviction policy, stated as a pure function so the host test can pin it:
// newest wins. Keeping the older block would preserve a total that no longer
// matches any live pointer, because the newer owner is the one whose free() can
// find the slot.
constexpr Action insertAction(const bool slotOccupied) { return slotOccupied ? Action::Steal : Action::Insert; }

// A free() whose pointer is not in the table. Expected for blocks allocated
// before begin() (bootloader, static ctors, the IDF libraries that reach
// multi_heap directly) and for evicted blocks. Counted, never guessed at.
struct Counters {
  uint32_t internalBytes = 0;
  uint32_t psramBytes = 0;
  uint32_t otherBytes = 0;
  uint32_t blocks = 0;
  uint32_t steals = 0;
  uint32_t earlySkips = 0;
  uint32_t untrackedFrees = 0;
  // Cumulative bytes allocated *below* the floor since begin(). Never decremented:
  // a freed small block leaves no trace, so this is churn, not live bytes. It is
  // here because the first device dump attributed 4.72 MB of PSRAM and only 14 KB
  // of internal RAM, and the honest question about the remaining 112 KB was
  // "is this the floor or is this a path we do not wrap" -- those two need
  // different fixes and only one of them can be answered without changing code.
  uint32_t smallAllocBytes = 0;
  uint32_t smallAllocBlocks = 0;
};

constexpr uint32_t trackedBytes(const Counters& c) { return c.internalBytes + c.psramBytes + c.otherBytes; }

// Measured state of the two heaps, taken at begin() and again when the ledger is
// read. Free space only ever tells part of the story; largest-block is the number
// every one of those incidents actually turned on.
struct Watermark {
  uint32_t internalFree = 0;
  uint32_t internalLargest = 0;
  uint32_t psramFree = 0;
  uint32_t psramLargest = 0;
};

enum class Verdict : uint8_t {
  NotWired,   // this build has no --wrap flags: the table is empty by construction
  Consistent, // attributed growth matches measured consumption within tolerance
  Gap,        // the heaps shrank by more than the ledger can attribute
  Contradiction, // the ledger attributes more than the heaps lost: a bookkeeping bug
};

// What a gap is most likely made of. Plausibility, not proof: sub-floor churn is
// an upper bound on what the floor hid, and it cannot distinguish "the floor is
// too high" from "an allocator path is not wrapped". The two differ in the fix,
// so the dump names which one it is closer to instead of leaving the reader to
// guess from a bare byte count.
enum class GapCause : uint8_t {
  None,             // no gap to explain
  PlausiblySubFloor, // at least as many bytes were allocated below the floor as are unexplained
  Unexplained,      // the floor cannot account for it: something allocates outside the wrapped paths
  // A contradiction is not a hidden allocation: the ledger counted more than the
  // heap lost, so the reader is looking at double counting or a free that missed
  // its row. Naming the floor here would send them to the wrong file.
  Bookkeeping,
};

struct CrossCheck {
  Verdict verdict = Verdict::NotWired;
  int32_t internalGap = 0;
  int32_t psramGap = 0;
  uint32_t unexplainedBytes = 0;
  GapCause gapCause = GapCause::None;
};

// The honesty step. Consumed bytes since begin() are (baselineFree - nowFree);
// anything the ledger did not attribute is reported as a gap instead of being
// smoothed into the total. `tolerance` covers the tracking table's own footprint,
// the fragmentation the allocator hides, and the counters racing each other.
constexpr CrossCheck crossCheck(const bool wired, const Watermark& baseline, const Watermark& now,
                                const Counters& counters, const uint32_t tolerance) {
  CrossCheck check;
  if (!wired) {
    check.verdict = Verdict::NotWired;
    return check;
  }
  check.internalGap = static_cast<int32_t>(baseline.internalFree - now.internalFree) -
                      static_cast<int32_t>(counters.internalBytes);
  check.psramGap = static_cast<int32_t>(baseline.psramFree - now.psramFree) -
                   static_cast<int32_t>(counters.psramBytes);
  const int32_t worst = check.internalGap > check.psramGap ? check.internalGap : check.psramGap;
  const int32_t best = check.internalGap < check.psramGap ? check.internalGap : check.psramGap;
  // Both comparisons stay in int32_t. Mixing signs here would promote the gap to
  // unsigned and turn "attributed more than the heap lost" into the largest
  // possible gap, i.e. a contradiction reported as the opposite problem.
  const int32_t bound = tolerance > static_cast<uint32_t>(INT32_MAX) ? INT32_MAX : static_cast<int32_t>(tolerance);
  if (best < -bound) {
    // Attributed bytes exceed what the heaps lost: a block was recorded twice, or
    // a free did not find its slot. The numbers cannot both be true.
    check.verdict = Verdict::Contradiction;
    check.unexplainedBytes = static_cast<uint32_t>(-best);
  } else if (worst > bound) {
    check.verdict = Verdict::Gap;
    check.unexplainedBytes = static_cast<uint32_t>(worst);
  } else {
    check.verdict = Verdict::Consistent;
  }
  if (check.verdict == Verdict::Consistent) {
    check.gapCause = GapCause::None;
  } else if (check.verdict == Verdict::Contradiction) {
    check.gapCause = GapCause::Bookkeeping;
  } else {
    check.gapCause = counters.smallAllocBytes >= check.unexplainedBytes ? GapCause::PlausiblySubFloor
                                                                        : GapCause::Unexplained;
  }
  return check;
}

constexpr const char* name(const Verdict verdict) {
  switch (verdict) {
    case Verdict::NotWired:
      return "not-wired";
    case Verdict::Consistent:
      return "consistent";
    case Verdict::Gap:
      return "gap";
    case Verdict::Contradiction:
      return "contradiction";
  }
  return "unknown";
}

constexpr const char* name(const GapCause cause) {
  switch (cause) {
    case GapCause::None:
      return "none";
    case GapCause::PlausiblySubFloor:
      return "below-floor";
    case GapCause::Unexplained:
      return "unwrapped-path";
    case GapCause::Bookkeeping:
      return "bookkeeping";
  }
  return "unknown";
}

constexpr const char* name(const Region region) {
  switch (region) {
    case Region::Internal:
      return "internal";
    case Region::Psram:
      return "psram";
    case Region::Other:
      return "other";
  }
  return "unknown";
}

}  // namespace memory::ledger
