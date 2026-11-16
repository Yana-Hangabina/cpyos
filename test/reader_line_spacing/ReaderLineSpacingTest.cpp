#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "ReaderLineSpacing.h"

namespace {

int failures = 0;

void expectNear(const char* what, const float actual, const float expected) {
  if (std::fabs(actual - expected) > 1e-6f) {
    std::printf("FAIL %s: got %.6f, expected %.6f\n", what, actual, expected);
    ++failures;
  }
}

void expectEqual(const char* what, const int actual, const int expected) {
  if (actual != expected) {
    std::printf("FAIL %s: got %d, expected %d\n", what, actual, expected);
    ++failures;
  }
}

void check(const char* what, const bool ok) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using reader_line_spacing::resolve;

  // 1. 0 means off: every per-family factor the four levels can produce must
  //    come through untouched. These are exactly the literals in
  //    CrossPointSettings::getReaderLineCompression() (SD/vector 0.95/1.0/1.3/1.6,
  //    built-in serif 0.95/1.0/1.1/1.2, built-in sans 0.90/0.95/1.0/1.05).
  const float familyFactors[] = {0.90f, 0.95f, 1.0f, 1.05f, 1.1f, 1.2f, 1.3f, 1.6f};
  for (const float factor : familyFactors) {
    expectNear("off keeps the family factor", resolve(factor, 0), factor);
  }
  expectNear("off keeps sans tight", resolve(0.90f, reader_line_spacing::PERCENT_OFF), 0.90f);
  expectNear("off keeps sd wide", resolve(1.3f, reader_line_spacing::PERCENT_OFF), 1.3f);
  expectNear("off keeps sd extra wide", resolve(1.6f, reader_line_spacing::PERCENT_OFF), 1.6f);

  // 2. The requested mapping: 100/130/160 % become exactly 1.0/1.3/1.6.
  expectNear("100%", resolve(0.95f, 100), 1.0f);
  expectNear("130%", resolve(0.95f, 130), 1.3f);
  expectNear("160%", resolve(0.95f, 160), 1.6f);

  // 3. The override replaces whatever the level/family pair produced, on every
  //    family (otherwise "percent" would silently scale the levels instead).
  for (const float factor : familyFactors) {
    expectNear("100% ignores the family factor", resolve(factor, 100), 1.0f);
    expectNear("130% ignores the family factor", resolve(factor, 130), 1.3f);
    expectNear("160% ignores the family factor", resolve(factor, 160), 1.6f);
  }

  // 4. The rest of the grid the row/stepper exposes.
  expectNear("80%", resolve(1.0f, 80), 0.8f);
  expectNear("90%", resolve(1.0f, 90), 0.9f);
  expectNear("110%", resolve(1.0f, 110), 1.1f);
  expectNear("200%", resolve(1.0f, 200), 2.0f);
  expectNear("stepper max", resolve(1.0f, reader_line_spacing::PERCENT_MAX), 2.0f);

  // 5. The constants the Settings row and the picker share.
  expectEqual("PERCENT_OFF", reader_line_spacing::PERCENT_OFF, 0);
  expectEqual("PERCENT_MIN", reader_line_spacing::PERCENT_MIN, 0);
  expectEqual("PERCENT_MAX", reader_line_spacing::PERCENT_MAX, 200);
  expectEqual("PERCENT_STEP", reader_line_spacing::PERCENT_STEP, 10);
  check("PERCENT_MIN is PERCENT_OFF", reader_line_spacing::PERCENT_MIN == reader_line_spacing::PERCENT_OFF);
  check("max is on the step grid", reader_line_spacing::PERCENT_MAX % reader_line_spacing::PERCENT_STEP == 0);

  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("reader line spacing mapping: all %d checks passed\n", 8 + 8 + 3 + 2 + 4 + 2);
  return EXIT_SUCCESS;
}
