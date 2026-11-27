#include "ReaderFontSizes.h"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;

void expectSizes(const char* name, const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
  if (got == want) return;
  ++failures;
  std::printf("FAIL %s\n  got :", name);
  for (const uint8_t pt : got) std::printf(" %u", pt);
  std::printf("\n  want:");
  for (const uint8_t pt : want) std::printf(" %u", pt);
  std::printf("\n");
}

void expectRange(const char* name, const uint16_t height, const uint8_t min, const uint8_t max) {
  const ReaderPointRange got = readerPointRangeForPortraitHeight(height);
  if (got.min == min && got.max == max) return;
  ++failures;
  std::printf("FAIL %s: %u..%u, want %u..%u\n", name, got.min, got.max, min, max);
}
}  // namespace

int main() {
  static_assert(READER_POINT_SIZE_COUNT == 15, "reader ladder size changed");
  static_assert(READER_MIN_POINT_SIZE == 10, "reader minimum changed");
  static_assert(READER_MAX_POINT_SIZE == 40, "large-print maximum changed");

  // Device-derived bounds: 4.7" Read Pico/Plus get the full range; a 3.97"
  // A4 (768 px) and the 800 px panels cap at 26; a 1100 px panel caps at 36.
  expectRange("4.7in", 1216, 10, 40);
  expectRange("A4 3.97in", 768, 10, 26);
  expectRange("800px", 800, 10, 26);
  expectRange("1100px", 1100, 10, 36);
  expectRange("small", 528, 10, 18);

  const std::vector<uint8_t> ladder = {10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 40};

  // A .cpfont shipping the old four sizes keeps them and gains only the tails.
  expectSizes("old cpfont 12-18", mergeReaderPointSizes({12, 14, 16, 18}, READER_POINT_SIZES,
                                                        READER_POINT_SIZE_COUNT, 10, 40),
              {10, 12, 14, 16, 18, 40});

  // A 3.97" A4 range keeps the fixed rows and adds only its 10/26 endpoints.
  expectSizes("A4 cpfont 12-18", mergeReaderPointSizes({12, 14, 16, 18}, READER_POINT_SIZES,
                                                       READER_POINT_SIZE_COUNT, 10, 26),
              {10, 12, 14, 16, 18, 26});

  // Gaps inside the native range stay native: a 12/16 font does not gain 14.
  expectSizes("gapped cpfont", mergeReaderPointSizes({12, 16}, READER_POINT_SIZES, READER_POINT_SIZE_COUNT, 10, 40),
              {10, 12, 16, 40});

  // A mid-range family extends both below and above.
  expectSizes("mid cpfont", mergeReaderPointSizes({20, 22, 24}, READER_POINT_SIZES, READER_POINT_SIZE_COUNT, 10, 40),
              {10, 20, 22, 24, 40});

  // No native sizes (or only out-of-range ones) means the full scalable ladder.
  expectSizes("empty cpfont", mergeReaderPointSizes({}, READER_POINT_SIZES, READER_POINT_SIZE_COUNT, 10, 40), ladder);
  expectSizes("out-of-range cpfont", mergeReaderPointSizes({8, 48}, READER_POINT_SIZES, READER_POINT_SIZE_COUNT, 10, 40),
              ladder);

  // A family that already covers the ladder adds nothing and has no duplicates.
  expectSizes("full ladder", mergeReaderPointSizes(ladder, READER_POINT_SIZES, READER_POINT_SIZE_COUNT, 10, 40),
              ladder);

  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("reader font sizes: OK (%zu ladder points)\n", ladder.size());
  return 0;
}
