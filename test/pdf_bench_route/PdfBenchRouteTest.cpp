#include "PdfBenchRoute.h"

#include <cstdio>

using namespace pdf_bench;

static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    ++checks;                                                                 \
    if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__);        \
                   std::printf(__VA_ARGS__); std::printf("\n"); }             \
  } while (0)

int main() {
  CHECK(route(true, true) == Route::Forward, "a foreground reader did not receive the command");
  CHECK(route(true, false) == Route::Forward, "a foreground reader was treated as absent");
  // The case the fix is about: reader present, modal on top -> dismiss, then resend.
  CHECK(route(false, true) == Route::DismissAndResend, "a modal over the reader was ignored");
  // Only genuinely absent readers are ignored.
  CHECK(route(false, false) == Route::Ignore, "a closed reader caused a pop");
  std::printf("pdf bench route: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
