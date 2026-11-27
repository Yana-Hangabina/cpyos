// Host regression for the PDF engine updater's stall rule.
//
// Each block here corresponds to a mistake that was actually made on device, so
// the tests are written as "the old wrong behaviour must be caught", not as
// coverage for its own sake.
#include "PdfUpdateWatchdog.h"

#include <cstdio>
#include <cstdint>

using namespace pdf_update;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                         \
  do {                                                                      \
    ++checks;                                                               \
    if (!(cond)) {                                                          \
      ++failures;                                                           \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                       \
  } while (0)

// Bug #1: the baseline was 0, i.e. "from power-on", so the first tick after a
// long session cancelled a transfer that had just begun.
static void testNoBaselineCannotExpire() {
  const auto d = checkStall(StallStage::Body, 0, 127000u, kNoBaseline);
  CHECK(!d.fired);
  CHECK(d.limitMs == 0);
  // And the caller's re-base must supply a real baseline on entry.
  const uint32_t baseline = nextBaseline(StallStage::Body, StallStage::Manifest, 127000u, kNoBaseline, 0u, 0u);
  CHECK(baseline == 127000u);
  CHECK(!checkStall(StallStage::Body, 0, 127000u, baseline).fired);
}

// Bug #2: time spent in the manifest stage charged to the body stage. Measured on
// the hotspot AP: the manifest itself took ~50 s because TLS 1.2 was retried.
static void testStageEntryRebases() {
  const uint32_t bodyStart = 60000u;
  CHECK(nextBaseline(StallStage::Body, StallStage::Manifest, bodyStart, /*current*/ 1000u, 0u, 0u) == bodyStart);
  // 50 s after the *session* began but only 40 s into the body stage: alive.
  CHECK(!checkStall(StallStage::Body, 0, 100000u, bodyStart).fired);
}

// First byte is a different failure than a stopped stream.
static void testFirstByteWindow() {
  const uint32_t start = 10000u;
  CHECK(!checkStall(StallStage::Body, 0, start + kFirstByteMs - 1u, start).fired);
  CHECK(checkStall(StallStage::Body, 0, start + kFirstByteMs, start).fired);
  const auto first = checkStall(StallStage::Body, 0, start + kFirstByteMs, start);
  CHECK(first.limitMs == kFirstByteMs);
}

static void testMidStreamWindow() {
  const uint32_t start = 10000u;
  CHECK(!checkStall(StallStage::Body, 900000u, start + kMidStreamMs - 1u, start).fired);
  const auto dead = checkStall(StallStage::Body, 900000u, start + kMidStreamMs, start);
  CHECK(dead.fired);
  CHECK(dead.limitMs == kMidStreamMs);
  CHECK(dead.elapsedMs == kMidStreamMs);
  // The mid-stream bound is shorter than the first-byte bound, on purpose.
  CHECK(kMidStreamMs < kFirstByteMs);
}

// Real progress must re-base, and the baseline must be preserved otherwise.
static void testProgressRebases() {
  const uint32_t old = 50000u;
  CHECK(nextBaseline(StallStage::Body, StallStage::Body, 60000u, old, 4096u, 0u) == 60000u);
  CHECK(nextBaseline(StallStage::Body, StallStage::Body, 60000u, old, 4096u, 4096u) == old);
  // Outside the body stage nothing re-bases (verify is local, not a download).
  CHECK(nextBaseline(StallStage::Verify, StallStage::Body, 60000u, old, 4096u, 0u) == old);
}

// millis() wraps at 2^32. The difference of two stamps is still the elapsed time,
// so a straddle must neither invent a stall nor hide a real one.
static void testWrapArithmetic() {
  // baseline taken 4096 ms before the wrap, now 1000 ms after it: 5096 ms elapsed.
  const auto d = checkStall(StallStage::Body, 100u, 0x000003E8u, 0xFFFFF000u);
  CHECK(d.elapsedMs == 5096u);
  CHECK(!d.fired);
  // A full stall that straddles the wrap must fire, not clamp to zero.
  const uint32_t before = 0xFFFFF000u;
  const uint32_t after = before + (kMidStreamMs + 5000u);  // wraps
  const auto dead = checkStall(StallStage::Body, 100u, after, before);
  CHECK(dead.fired);
  CHECK(dead.elapsedMs == kMidStreamMs + 5000u);
  // A baseline ahead of `now` is not a case the API supports: callers must take the
  // stamp on entry. What protects the uninitialized case is kNoBaseline, asserted in
  // testNoBaselineCannotExpire, and it is checked before any arithmetic -- which is
  // why the mutation that reads an unset baseline as 0 has to be caught there.
}

// Other stages must never fire, however old their baseline is.
static void testOnlyBodyStageFires() {
  const uint32_t ancient = 1000u;
  CHECK(!checkStall(StallStage::Idle, 0u, 900000u, ancient).fired);
  CHECK(!checkStall(StallStage::Manifest, 0u, 900000u, ancient).fired);
  CHECK(!checkStall(StallStage::Verify, 500000u, 900000u, ancient).fired);
}

int main() {
  testNoBaselineCannotExpire();
  testStageEntryRebases();
  testFirstByteWindow();
  testMidStreamWindow();
  testProgressRebases();
  testWrapArithmetic();
  testOnlyBodyStageFires();
  printf("pdf_update_watchdog: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
