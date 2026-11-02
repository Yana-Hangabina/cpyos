// Host check for the pagination LUT container in Section.h.
//
// The incremental build appends one PageLutEntry per laid-out page while the heap is
// fragmented by the parser. A std::vector needs a fresh contiguous block on every growth
// (a 1500-page chapter asks for ~24 KB at once); a std::deque uses fixed-size nodes and never
// moves existing entries. This test compiles the real Section.h, pins the container type and
// the 12-byte entry layout, and checks deque indexing matches a vector exactly at the sizes
// the reader actually reaches (0 / 1 / 1024 / 1500 pages), plus the no-invalidation property.
//
// Section.h is included with `private` redefined so the nested BuildContext can be named
// without instantiating Section (no Section.cpp link, no HAL objects).
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#define private public
#include "Epub/Section.h"
#undef private

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

Section::PageLutEntry entryFor(const size_t i) {
  Section::PageLutEntry e{};
  e.fileOffset = static_cast<uint32_t>(0x100 + i * 0x40);
  e.paragraphIndex = static_cast<uint16_t>(i % 4093);
  e.listItemIndex = static_cast<uint16_t>(i % 251);
  e.visibleTextOffset = static_cast<uint32_t>(i * 37);
  return e;
}

void compareAt(const size_t count) {
  std::deque<Section::PageLutEntry> dq;
  std::vector<Section::PageLutEntry> vec;
  for (size_t i = 0; i < count; ++i) {
    dq.push_back(entryFor(i));
    vec.push_back(entryFor(i));
  }
  check(dq.size() == vec.size(), "deque size must match vector size");
  check(dq.empty() == vec.empty(), "deque empty() must match vector");
  if (count > 0) {
    check(dq.back().visibleTextOffset == vec.back().visibleTextOffset, "back() must match");
    check(dq.front().fileOffset == vec.front().fileOffset, "front() must match");
  }
  for (size_t i = 0; i < count; ++i) {
    check(dq[i].fileOffset == vec[i].fileOffset, "indexed fileOffset must match");
    check(dq[i].paragraphIndex == vec[i].paragraphIndex, "indexed paragraphIndex must match");
    check(dq[i].listItemIndex == vec[i].listItemIndex, "indexed listItemIndex must match");
    check(dq[i].visibleTextOffset == vec[i].visibleTextOffset, "indexed visibleTextOffset must match");
  }
  // Range-for order (the LUT serialization walks the container this way).
  uint64_t dqDigest = 0;
  uint64_t vecDigest = 0;
  for (const auto& e : dq) dqDigest = dqDigest * 1099511628211ull ^ e.fileOffset;
  for (const auto& e : vec) vecDigest = vecDigest * 1099511628211ull ^ e.fileOffset;
  check(dqDigest == vecDigest, "range-for order must match");
}

}  // namespace

int main() {
  using Lut = decltype(Section::BuildContext::lut);
  static_assert(std::is_same_v<Lut, std::deque<Section::PageLutEntry>>,
                "BuildContext::lut must be a std::deque (see the comment in Section.h)");
  static_assert(sizeof(Section::PageLutEntry) == 12,
                "PageLutEntry must stay 4+2+2+4 bytes; the on-disk LUT is written field by field");

  compareAt(0);
  compareAt(1);
  compareAt(1024);
  compareAt(1500);

  // The reason for the change: growing the LUT must not move earlier entries.
  std::deque<Section::PageLutEntry> lut;
  lut.push_back(entryFor(0));
  const Section::PageLutEntry* first = &lut[0];
  for (size_t i = 1; i < 1500; ++i) lut.push_back(entryFor(i));
  check(first->fileOffset == 0x100, "a reference to an early entry survives 1500 appends");
  check(first->visibleTextOffset == 0, "the early entry's payload survives 1500 appends");
  check(lut.size() == 1500, "LUT holds every appended page");

  if (failures == 0) std::printf("PASS: LUT deque container (0 failures)\n");
  return failures == 0 ? 0 : 1;
}
