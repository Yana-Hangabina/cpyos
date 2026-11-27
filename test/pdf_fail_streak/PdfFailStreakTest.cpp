// Pins when the failure card escalates from "skip this page" to "this file
// cannot be shown here".
#include "PdfFailStreak.h"

#include <cstdio>

using namespace pdf_fail_streak;

static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    ++checks;                                                                 \
    if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__);        \
                   std::printf(__VA_ARGS__); std::printf("\n"); }             \
  } while (0)

int main() {
  // Literal boundaries, not `kEscalateAfter - 1`: a test written in terms of the
  // constant moves with it and cannot catch the constant changing (that is how
  // mutation 1 survived once).
  CHECK(kEscalateAfter == 3, "the documented threshold is %d, not 3", kEscalateAfter);
  CHECK(!escalate(0) && !escalate(1) && !escalate(2), "a run under three escalated");
  CHECK(escalate(3), "three consecutive failures do not escalate");
  CHECK(escalate(8), "a long run stopped escalating");

  // One bad page in a readable book must never escalate.
  int streak = 0;
  streak = afterPage(streak, true);
  streak = afterPage(streak, false);
  CHECK(streak == 1 && !escalate(streak), "one failure after a success escalated");
  streak = afterPage(streak, true);
  CHECK(streak == 0, "a success did not reset the streak");

  // Consecutive failures do.
  streak = 0;
  for (int i = 0; i < kEscalateAfter; ++i) streak = afterPage(streak, false);
  CHECK(escalate(streak), "three consecutive failures did not escalate");

  std::printf("pdf fail streak: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
