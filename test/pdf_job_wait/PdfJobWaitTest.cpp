// Pins the wait rule the PDF worker depends on.  The failure this prevents is a
// device that stops responding with *nothing in the log*, which is the one
// outcome no post-mortem can attribute.
#include "PdfJobWait.h"

#include <cstdio>

using namespace pdf_job_wait;

static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                                    \
  do {                                                                      \
    ++checks;                                                                \
    if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__);      \
                   std::printf(__VA_ARGS__); std::printf("\n"); }            \
  } while (0)

int main() {
  // 1. A wait that has just begun says nothing.  Every page turn waits on the
  //    worker, so announcing on entry would put a `pdf worker waiting` line in
  //    the log for the normal case -- and a normal-looking diagnostic line in
  //    the log is how the real ones stop being believed (this happened once
  //    already: the first build of this rule printed at 0 ms, twice per page).
  {
    const Progress p = decide(1000, 1000, kDefaultTimeoutMs, 0);
    CHECK(!p.announce, "announced the instant the wait began");
    CHECK(!p.timedOut, "a fresh wait cannot already be timed out");
    CHECK(!decide(1000 + kProgressEveryMs - 1, 1000, kDefaultTimeoutMs, 0).announce,
          "announced before the first interval");
    CHECK(decide(1000 + kProgressEveryMs, 1000, kDefaultTimeoutMs, 0).announce,
          "missed the first scheduled announcement");
  }

  // 2. Cadence: nothing between the announcements, one per kProgressEveryMs.
  {
    CHECK(!decide(1000 + 2 * kProgressEveryMs - 1, 1000, kDefaultTimeoutMs, 1).announce,
          "announced early (after one line)");
    CHECK(decide(1000 + 2 * kProgressEveryMs, 1000, kDefaultTimeoutMs, 1).announce,
          "missed the second announcement");
    CHECK(decide(1000 + 4 * kProgressEveryMs, 1000, kDefaultTimeoutMs, 3).announce,
          "missed the fourth announcement");
  }

  // 3. Timeout fires at the boundary, and the announcement comes with it so the
  //    last state is in the log before the restart.
  {
    const Progress justBefore = decide(5000 + kDefaultTimeoutMs - 1, 5000, kDefaultTimeoutMs, 20);
    const Progress at = decide(5000 + kDefaultTimeoutMs, 5000, kDefaultTimeoutMs, 20);
    CHECK(!justBefore.timedOut, "timed out one millisecond early");
    CHECK(at.timedOut && at.announce, "the timeout did not announce itself");
  }

  // 4. millis() wraps after ~49.7 days.  A signed or clamped difference would
  //    either announce for hours or never time out; modular arithmetic is the
  //    only correct answer, and it is the one a reader is tempted to "fix".
  {
    const uint32_t started = 0xFFFFFFF0u;  // 16 ms before the wrap
    const uint32_t now = started + 20000;  // wraps: 0x4e2
    const Progress p = decide(now, started, kDefaultTimeoutMs, 1);
    CHECK(p.elapsed == 20000u, "elapsed across the wrap is %u (want 20000)", p.elapsed);
    CHECK(!p.timedOut, "a 20 s wait across the wrap must not look expired");
    const Progress late = decide(started + kDefaultTimeoutMs, started, kDefaultTimeoutMs, 30);
    CHECK(late.timedOut, "a full timeout across the wrap was not recognised");
  }

  // 5. A zero timeout cannot mean "wait forever": the very first check expires.
  {
    const Progress p = decide(7000, 7000, 0, 0);
    CHECK(p.timedOut, "a zero timeout became unbounded");
  }

  // 6. The documented budget must cover the worst measured page, or the fix
  //    turns slow-but-correct renders into restarts. 67 s is the measured
  //    embedded-font page; 180 s is the current constant.
  {
    CHECK(kDefaultTimeoutMs >= 120000u, "the timeout is below the worst measured page");
    CHECK(kProgressEveryMs < kDefaultTimeoutMs, "no announcement would ever be due");
  }

  std::printf("pdf job wait: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
