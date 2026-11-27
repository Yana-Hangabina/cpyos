// Drives the SHIPPED pdf_join walk over page data measured from a real PDF, so that the
// paragraph counts quoted in docs/engineering/pdf-xip.md come from the same code the
// device runs rather than from a Python restatement of the rule.
//
// stdin, one block at a time:
//   BLOCK <n>
//   <printable 0|1> <x0> <y0> <x1> <y1>      (repeated n times)
// stdout, two lines per block:
//   STARTS <count> <lineIndex>...            the paragraph starts, for split context
//   STATS  <printableLines> <fullLines>      what the block was judged to be
#include <cstdio>

#include <vector>

#include "PdfLineJoin.h"

int main() {
  char label[8];
  int n = 0;
  while (std::scanf("%7s %d", label, &n) == 2) {
    std::vector<pdf_join::LineRecord> lines;
    if (n > 0) lines.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      int printable = 1;
      float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      if (std::scanf("%d %f %f %f %f", &printable, &x0, &y0, &x1, &y1) != 5) return 1;
      lines.push_back(pdf_join::LineRecord{{x0, y0, x1, y1}, printable != 0});
    }
    const auto starts = pdf_join::paragraphStarts(lines);
    const pdf_join::BlockStats stats = pdf_join::measureBlock(lines);
    std::printf("STARTS %zu", starts.size());
    for (const int index : starts) std::printf(" %d", index);
    std::printf("\nSTATS %d %d\n", stats.lines, stats.fullLines);
  }
  return 0;
}
