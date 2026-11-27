// Host check for the memory contract table (lib/Memory/MemoryContracts.h).
//
// The table is the only place that says what a feature is allowed to hold and who
// puts it back after the radio took the room. Each mutation re-creates a way that
// statement stops meaning anything: an unpaired release (the 0.8.2 tofu), a limit
// smaller than the ask it is supposed to bound, a yield order that ignores
// releasability, or a suite that passes because the table was emptied.
#include "MemoryContracts.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

using namespace memory::contract;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    ++checks;                                                      \
    if (!(cond)) {                                                 \
      ++failures;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
    }                                                              \
  } while (0)

static void tableIsPopulated() {
  // A lower bound, so "green" cannot be produced by deleting rows.
  CHECK(kEntryCount >= 25);
  // The framebuffer row is the sum of the planes the first device dump found, so
  // pinning it pins the observation too, not just the shape of the table.
  CHECK(kEntries[0].declaredLimitBytes == 4u * 103968u);
  CHECK(kEntries[0].largestAskBytes == 103968u);
}

static void namesAreUniqueAndNonEmpty() {
  for (size_t i = 0; i < kEntryCount; ++i) {
    CHECK(std::strlen(kEntries[i].feature) > 0);
    for (size_t j = i + 1; j < kEntryCount; ++j) {
      CHECK(std::strcmp(kEntries[i].feature, kEntries[j].feature) != 0);
    }
  }
}

static void everyReleaseIsPaired() {
  size_t unpaired = 0;
  for (const Entry& e : kEntries) {
    const bool isUnpaired = e.releases == Release::ForNetwork && e.restorer == Restorer::None;
    if (isUnpaired) ++unpaired;
    // paired() must agree with the raw shape, otherwise it is decoration and a
    // change to it goes unnoticed.
    CHECK(paired(e) == !isUnpaired);
  }
  // The debt list must match reality exactly: too small hides new debt, too big
  // claims debt that has already been fixed.
  CHECK(unpaired == kKnownUnpairedCount);

  // Fixtures, because the table currently carries no debt: the rule has to be
  // tested against the shape it exists to reject, not only against a table that
  // happens to satisfy it today.
  const Entry unpairedCase{"fixture.unpaired", 1u, 1u, Release::ForNetwork, Restorer::None, true};
  CHECK(!paired(unpairedCase));
  const Entry pairedCase{"fixture.paired", 1u, 1u, Release::ForNetwork, Restorer::RenderMemory, true};
  CHECK(paired(pairedCase));
  const Entry neverCase{"fixture.never", 1u, 1u, Release::Never, Restorer::None, true};
  CHECK(paired(neverCase));
}

static void restorersImplyRelease() {
  // A restorer on a feature that is never released is decoration at best and a
  // lie about the pairing at worst: the pairing exists to cover releases.
  for (const Entry& e : kEntries) {
    if (e.restorer != Restorer::None) {
      CHECK(e.releases == Release::ForNetwork);
    }
  }
}

static void limitsBoundTheirAsks() {
  for (const Entry& e : kEntries) {
    if (e.declaredLimitBytes == 0) {
      // Declared as "no cap yet": that is a finding, and it must be flagged
      // unmeasured rather than left to read like a verified zero.
      CHECK(!e.measured);
      continue;
    }
    CHECK(e.declaredLimitBytes >= e.largestAskBytes);
  }
}

static void yieldOrderIsReleasableAndSorted() {
  size_t out[kEntryCount];
  const size_t n = yieldOrder(kEntries, kEntryCount, out, kEntryCount);
  CHECK(n > 0);
  size_t releasable = 0;
  for (const Entry& e : kEntries) {
    if (e.releases == Release::ForNetwork) ++releasable;
  }
  CHECK(n == releasable);
  for (size_t i = 0; i < n; ++i) {
    CHECK(kEntries[out[i]].releases == Release::ForNetwork);
    for (size_t j = i + 1; j < n; ++j) {
      CHECK(kEntries[out[i]].declaredLimitBytes >= kEntries[out[j]].declaredLimitBytes);
      CHECK(out[i] != out[j]);  // no index listed twice
    }
  }
  // A truncated request must still be a prefix of the same order, not a subset in
  // arbitrary slot order.
  size_t few[3];
  const size_t m = yieldOrder(kEntries, kEntryCount, few, 3);
  CHECK(m == 3);
  for (size_t i = 0; i < m; ++i) CHECK(few[i] == out[i]);
  CHECK(yieldOrder(kEntries, kEntryCount, few, 0) == 0);
}

static void withinDeclaredBoundaries() {
  const Entry face = {"x", 1000u, 500u, Release::ForNetwork, Restorer::RenderMemory, true};
  CHECK(withinDeclared(face, 999));
  CHECK(withinDeclared(face, 1000));
  CHECK(!withinDeclared(face, 1001));
  const Entry none = {"y", 0u, 0u, Release::Never, Restorer::None, false};
  CHECK(!withinDeclared(none, 0));  // no cap means no confirmation, not a free pass
  CHECK(!withinDeclared(none, 1u));
}

static void labelsAreDistinct() {
  CHECK(std::strcmp(name(Restorer::None), name(Restorer::RenderMemory)) != 0);
  CHECK(std::strcmp(name(Restorer::RenderMemory), name(Restorer::SelfLimited)) != 0);
  CHECK(std::strcmp(name(Release::Never), name(Release::ForNetwork)) != 0);
}

static void theKnownIncidentsAreRepresented() {
  // The font supplements: released for the radio, restored by the pairing. This
  // row is the §20 fix written down as data, so a later refactor that drops the
  // restorer fails here instead of shipping tofu.
  bool foundFontRelease = false;
  for (const Entry& e : kEntries) {
    if (std::strcmp(e.feature, "font.readerFace") == 0) {
      foundFontRelease = true;
      CHECK(e.releases == Release::ForNetwork);
      CHECK(e.restorer == Restorer::RenderMemory);
      CHECK(e.measured);
    }
  }
  CHECK(foundFontRelease);
  // The display's contiguous internal-RAM ask must be in the table: it is the one
  // number a black screen with no message turns on.
  bool foundScan = false;
  for (const Entry& e : kEntries) {
    if (std::strcmp(e.feature, "display.scanInternal") == 0) {
      foundScan = true;
      CHECK(e.largestAskBytes == 45056u);
      CHECK(e.releases == Release::Never);
      // The ledger cannot see this one; the row must admit it.
      CHECK(!e.measured);
    }
  }
  CHECK(foundScan);
}

int main() {
  tableIsPopulated();
  namesAreUniqueAndNonEmpty();
  everyReleaseIsPaired();
  restorersImplyRelease();
  limitsBoundTheirAsks();
  yieldOrderIsReleasableAndSorted();
  withinDeclaredBoundaries();
  labelsAreDistinct();
  theKnownIncidentsAreRepresented();
  std::printf("memory_contracts: %d checks, %d failures\n", checks, failures);
  for (const Entry& e : kEntries) {
    if (!e.measured) {
      std::printf("  unmeasured: %s (limit=%u ask=%u)\n", e.feature, e.declaredLimitBytes, e.largestAskBytes);
    }
  }
  return failures == 0 ? 0 : 1;
}
