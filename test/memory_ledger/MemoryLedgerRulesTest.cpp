// Host check for the allocation-ledger rules (lib/Memory/MemoryLedgerRules.h).
//
// The rules decide what gets recorded, where, and whether the recorded totals
// agree with the measured heaps. The agreement test is the reason this file
// exists: a ledger that quietly under-counts is worse than no ledger, because it
// reads like a measurement. Each mutation below re-creates one of the ways that
// happens.
#include "MemoryLedgerRules.h"

#include <cstdio>
#include <cstdint>
#include <string_view>

using namespace memory::ledger;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++checks;                                                             \
    if (!(cond)) {                                                        \
      ++failures;                                                         \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                     \
  } while (0)

static void floorBoundary() {
  CHECK(!tracks(kFloorBytes - 1));
  CHECK(tracks(kFloorBytes));
  CHECK(tracks(kFloorBytes + 1));
  CHECK(tracks(1u << 22));
  // The floor must still exclude the everyday churn it was raised to exclude: the
  // 2 KiB blocks are what filled the table with evictions on the first device run.
  CHECK(!tracks(512));
  CHECK(!tracks(1024));
  CHECK(!tracks(2048));
  CHECK(!tracks(4096));
  CHECK(tracks(8192));
  CHECK(!tracks(kFloorBytes - 1));
}

static size_t distinctSlots(const uintptr_t base, const uintptr_t stride, const size_t count) {
  bool seen[kSlots] = {};
  size_t distinct = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t s2 = slotFor(base + i * stride);
    if (!seen[s2]) {
      seen[s2] = true;
      ++distinct;
    }
  }
  return distinct;
}

static void slotProperties() {
  CHECK(slotFor(0x3c000100u) < kSlots);
  CHECK(slotFor(0) < kSlots);
  CHECK(slotFor(0x3c000100u) == slotFor(0x3c000100u));

  // The distribution is the property that decides whether the table measures or
  // merely collides. Heaps hand out neighbours at the sizes this ledger records,
  // so each realistic stride has to spread across the rows. A shift-and-mask
  // index passed every other check in this file and mapped a 4 KiB-strided heap
  // onto 16 rows -- that is the failure these numbers exist to catch.
  const uintptr_t dram = 0x3fc80000u;   // internal data heap shape
  const uintptr_t psram = 0x3c800000u;  // external heap shape
  CHECK(distinctSlots(dram, 2 * 1024u, 64) >= 48);
  CHECK(distinctSlots(dram, 4 * 1024u, 64) >= 48);
  CHECK(distinctSlots(psram, 4 * 1024u, 64) >= 48);
  CHECK(distinctSlots(psram, 64 * 1024u, 64) >= 48);
  CHECK(distinctSlots(psram, 1024u * 1024u, 64) >= 48);
  // A full 1 MiB run of 4 KiB blocks must not concentrate in one corner of the
  // table: this is the shape of a system that has just loaded a font pack.
  CHECK(distinctSlots(psram, 4 * 1024u, 256) >= 180);

  // Adjacent tracked blocks land on different rows, and the aliasing that does
  // exist is bounded: 256 rows is a decision, and the steal counter is what makes
  // exceeding it visible rather than silent.
  CHECK(slotFor(dram) != slotFor(dram + kFloorBytes));
  size_t collisions = 0;
  for (size_t i = 0; i < kSlots * 2; ++i) {
    for (size_t j = i + 1; j < kSlots * 2; ++j) {
      if (slotFor(dram + i * 4096u) == slotFor(dram + j * 4096u)) ++collisions;
    }
  }
  CHECK(collisions < kSlots * kSlots / 2);  // birthday-ish, not a collapse
}

static void regionTruthTable() {
  CHECK(regionOf(true, false) == Region::Psram);
  CHECK(regionOf(false, true) == Region::Internal);
  // Both flags at once cannot happen; if it ever did, it must not be silently
  // counted as internal, because the internal reserve is the scarce one.
  CHECK(regionOf(true, true) == Region::Psram);
  CHECK(regionOf(false, false) == Region::Other);
}

static void evictionPolicy() {
  CHECK(insertAction(false) == Action::Insert);
  CHECK(insertAction(true) == Action::Steal);
}

static void countersAggregate() {
  Counters c;
  c.internalBytes = 100;
  c.psramBytes = 200;
  c.otherBytes = 3;
  CHECK(trackedBytes(c) == 303);
}

static Counters attributed(const uint32_t internal, const uint32_t psram, const uint32_t small = 0) {
  Counters c;
  c.internalBytes = internal;
  c.psramBytes = psram;
  c.smallAllocBytes = small;
  return c;
}

static void crossCheckStates() {
  const Watermark base{80000, 40000, 5000000, 3000000};
  // Not wired: the table is empty by construction, and that must never be
  // reported as "consistent".
  CHECK(crossCheck(false, base, base, attributed(0, 0), 4096).verdict == Verdict::NotWired);

  // Nothing moved, nothing attributed.
  auto ok = crossCheck(true, base, base, attributed(0, 0), 4096);
  CHECK(ok.verdict == Verdict::Consistent);
  CHECK(ok.internalGap == 0);
  CHECK(ok.psramGap == 0);
  CHECK(ok.unexplainedBytes == 0);

  // 10 KB consumed, 10 KB attributed: the ledger explains the heap.
  Watermark now{70000, 30000, 4990000, 2990000};
  auto good = crossCheck(true, base, now, attributed(10000, 10000), 4096);
  CHECK(good.verdict == Verdict::Consistent);
  CHECK(good.internalGap == 0);
  CHECK(good.psramGap == 0);

  // The 0.8.2 shape: the heaps lost 40 KB of PSRAM and the ledger saw only 1 KB
  // of it (the font release goes through paths that predate the wrap). Must be a
  // gap, with the unexplained amount named.
  Watermark leaked{80000, 40000, 4960000, 2900000};
  auto gap = crossCheck(true, base, leaked, attributed(0, 1000), 4096);
  CHECK(gap.verdict == Verdict::Gap);
  CHECK(gap.psramGap == 39000);
  CHECK(gap.unexplainedBytes == 39000);

  // The contradiction shape: attributing more than the heap lost means a block
  // was recorded twice or a free missed its slot. Reporting that as a gap would
  // point the reader at the wrong suspect.
  auto bad = crossCheck(true, base, now, attributed(40000, 0), 4096);
  CHECK(bad.verdict == Verdict::Contradiction);
  CHECK(bad.internalGap == -30000);
  CHECK(bad.unexplainedBytes == 30000);

  // Tolerance boundaries, both sides, for both signs.
  Watermark slight{79000, 40000, 5000000, 3000000};
  CHECK(crossCheck(true, base, slight, attributed(1000, 0), 4096).verdict == Verdict::Consistent);
  CHECK(crossCheck(true, base, slight, attributed(0, 0), 4096).verdict == Verdict::Consistent);  // 1 KB under tolerance
  Watermark over{74000, 40000, 5000000, 3000000};
  CHECK(crossCheck(true, base, over, attributed(0, 0), 4096).verdict == Verdict::Gap);  // 6 KB unexplained
  CHECK(crossCheck(true, base, over, attributed(2000, 0), 4096).verdict == Verdict::Consistent);  // 4 KB left, inside tolerance
  CHECK(crossCheck(true, base, over, attributed(1000, 0), 4096).verdict == Verdict::Gap);         // 5 KB left, outside
  CHECK(crossCheck(true, base, base, attributed(5000, 0), 4096).verdict == Verdict::Contradiction);
  CHECK(crossCheck(true, base, base, attributed(4096, 0), 4096).verdict == Verdict::Consistent);
}

static void gapCauseIsNamed() {
  const Watermark base{100000, 50000, 4000000, 2000000};
  Watermark now = base;
  now.internalFree -= 60000;  // 60 KB consumed, nothing attributed
  // No sub-floor traffic recorded: the floor cannot be blamed, so the answer is
  // that some allocator path is not wrapped. That is a different fix, and the
  // dump has to say which one the reader is looking at.
  auto unwrapped = crossCheck(true, base, now, attributed(0, 0, 0), 4096);
  CHECK(unwrapped.verdict == Verdict::Gap);
  CHECK(unwrapped.gapCause == GapCause::Unexplained);
  // Enough sub-floor churn to cover the gap: plausible, and stated as plausible
  // rather than as proven, because the counter is cumulative and never freed.
  auto floor = crossCheck(true, base, now, attributed(0, 0, 60000), 4096);
  CHECK(floor.verdict == Verdict::Gap);
  CHECK(floor.gapCause == GapCause::PlausiblySubFloor);
  auto partial = crossCheck(true, base, now, attributed(0, 0, 59000), 4096);
  CHECK(partial.gapCause == GapCause::Unexplained);
  // Nothing to explain must not name a cause.
  CHECK(crossCheck(true, base, base, attributed(0, 0, 100000), 4096).gapCause == GapCause::None);
  CHECK(crossCheck(false, base, now, attributed(0, 0, 100000), 4096).gapCause == GapCause::None);
  // A contradiction is a bookkeeping bug; blaming the floor would send the reader
  // after the wrong suspect, even when the sub-floor counter happens to be large.
  auto bad = crossCheck(true, base, base, attributed(80000, 0, 80000), 4096);
  CHECK(bad.verdict == Verdict::Contradiction);
  CHECK(bad.gapCause == GapCause::Bookkeeping);
  CHECK(std::strcmp(name(GapCause::Bookkeeping), name(GapCause::PlausiblySubFloor)) != 0);
  CHECK(std::strcmp(name(GapCause::Bookkeeping), name(GapCause::Unexplained)) != 0);
  CHECK(std::strcmp(name(GapCause::None), name(GapCause::Unexplained)) != 0);
  CHECK(std::strcmp(name(GapCause::PlausiblySubFloor), name(GapCause::Unexplained)) != 0);
}

static void verdictExhaustive() {
  // Enumerate a grid and assert the invariants that make the dump readable:
  // exactly one verdict, unexplainedBytes non-zero unless Consistent, and the
  // gap sign matching the verdict.
  const uint32_t consumedInternal[] = {0, 2048, 8192, 65536};
  const uint32_t consumedPsram[] = {0, 4096, 100000, 1000000};
  const uint32_t attributedInternal[] = {0, 1024, 8192, 20000};
  const uint32_t attributedPsram[] = {0, 1000, 100000, 400000};
  int cases = 0;
  for (const uint32_t ci : consumedInternal) {
    for (const uint32_t cp : consumedPsram) {
      for (const uint32_t ai : attributedInternal) {
        for (const uint32_t ap : attributedPsram) {
          const Watermark base{100000, 50000, 4000000, 2000000};
          Watermark now = base;
          now.internalFree -= ci;
          now.psramFree -= cp;
          const auto check = crossCheck(true, base, now, attributed(ai, ap), 4096);
          ++cases;
          CHECK(check.verdict == Verdict::Consistent || check.verdict == Verdict::Gap ||
                check.verdict == Verdict::Contradiction);
          if (check.verdict == Verdict::Consistent) {
            CHECK(check.unexplainedBytes == 0);
          } else {
            CHECK(check.unexplainedBytes > 0);
          }
          if (check.verdict == Verdict::Gap) {
            CHECK(check.internalGap > 0 || check.psramGap > 0);
          }
          if (check.verdict == Verdict::Consistent) {
            CHECK(check.gapCause == GapCause::None);
          } else if (check.verdict == Verdict::Contradiction) {
            CHECK(check.gapCause == GapCause::Bookkeeping);
          } else {
            CHECK(check.gapCause == GapCause::PlausiblySubFloor || check.gapCause == GapCause::Unexplained);
          }
          if (check.verdict == Verdict::Contradiction) {
            CHECK(check.internalGap < 0 || check.psramGap < 0);
          }
        }
      }
    }
  }
  CHECK(cases == 256);
}

static void labelsAreDistinct() {
  const Verdict all[] = {Verdict::NotWired, Verdict::Consistent, Verdict::Gap, Verdict::Contradiction};
  for (const Verdict a : all) {
    for (const Verdict b : all) {
      if (a != b) CHECK(std::string_view(name(a)) != std::string_view(name(b)));
    }
  }
  const Region regions[] = {Region::Internal, Region::Psram, Region::Other};
  for (const Region a : regions) {
    for (const Region b : regions) {
      if (a != b) CHECK(std::string_view(name(a)) != std::string_view(name(b)));
    }
  }
}

int main() {
  floorBoundary();
  slotProperties();
  regionTruthTable();
  evictionPolicy();
  countersAggregate();
  crossCheckStates();
  gapCauseIsNamed();
  verdictExhaustive();
  labelsAreDistinct();
  std::printf("memory_ledger: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
