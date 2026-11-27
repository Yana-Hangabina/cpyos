#include <cstdint>
#include <cstdio>

#include "PdfDeadline.h"

// The budget rule, pinned at its boundaries.  The wrap cases are the reason this
// is a function and not an inline comparison: a device left on for weeks must
// not have its render budget flip inside out.
static int checks = 0;
static int failures = 0;
#define CHECK(cond, what)                \
  do {                                   \
    ++checks;                            \
    if (!(cond)) {                       \
      ++failures;                        \
      std::printf("FAIL: %s\n", (what)); \
    }                                    \
  } while (0)

int main() {
  using pdf_deadline::expired;

  CHECK(!expired(0, 0), "a zero deadline counted as expired");
  CHECK(!expired(0, 123456), "a zero deadline expired later");
  CHECK(!expired(1000, 999), "an unexpired budget was expired");
  CHECK(expired(1000, 1000), "the deadline instant is not 'expired'");
  CHECK(expired(1000, 4000), "a passed budget was not expired");
  CHECK(!expired(90000, 89), "90 s budget expired at 89 ms");

  // Wrap: deadline at 2^32-10, now at 5 -> crossed by 15 ms.
  const uint32_t justBeforeWrap = 0xFFFFFFF6u;
  CHECK(expired(justBeforeWrap, 5u), "a wrap crossed the deadline and was missed");
  CHECK(!expired(justBeforeWrap, 0xFFFF0000u), "a deadline 65 s away expired early");
  // The long-gap form must stay sane either side of the wrap.
  CHECK(!expired(500u, 0xFFFFFFF0u), "a far-future (post-wrap) deadline expired");
  CHECK(expired(0xFFFFFFF0u, 500u), "a post-wrap elapsed budget was missed");

  std::printf("pdf deadline: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
