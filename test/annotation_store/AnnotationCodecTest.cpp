// Host regression for the annotation codec. Annotations are stored as
// (spine, visible-offset range) and decoded on every reader entry; a codec that
// guesses at a damaged file underlines the wrong sentence, so every rejection
// path is pinned here.
#include <AnnotationStore.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

annotations::Annotation makeHighlight(const uint16_t spine, const uint32_t start, const uint32_t end,
                                      const char* quote) {
  annotations::Annotation a;
  a.spineIndex = spine;
  a.startOffset = start;
  a.endOffset = end;
  a.kind = static_cast<uint8_t>(annotations::Kind::Highlight);
  a.quote = quote;
  a.chapterTitle = "第一章";
  return a;
}

void testRoundTrip() {
  std::vector<annotations::Annotation> list;
  list.push_back(makeHighlight(0, 100, 112, "第一段文字"));
  annotations::Annotation note = makeHighlight(2, 7, 9, "ab");
  note.kind = static_cast<uint8_t>(annotations::Kind::Note);
  note.note = "这里需要再读一遍";
  list.push_back(note);

  const auto bytes = annotations::serialize(list);
  std::vector<annotations::Annotation> restored;
  expect(annotations::deserialize(bytes.data(), bytes.size(), restored), "round trip decodes");
  expect(restored.size() == 2, "round trip keeps both entries");
  expect(restored[0].spineIndex == 0 && restored[0].startOffset == 100 && restored[0].endOffset == 112,
         "round trip keeps the first range");
  expect(restored[0].quote == "第一段文字", "round trip keeps the quote");
  expect(restored[1].isNote() && restored[1].note == "这里需要再读一遍", "round trip keeps the note");
}

void testUtf8Clamp() {
  // 4 bytes would split the second character; the clamp must stop on a boundary.
  expect(annotations::clampUtf8("中文", 4) == "中", "clamp backs off a split codepoint");
  expect(annotations::clampUtf8("abc", 4) == "abc", "clamp leaves short text alone");
  expect(annotations::clampUtf8("a中", 1) == "a", "clamp keeps a complete single-byte prefix");
}

void testNormalize() {
  std::vector<annotations::Annotation> list;
  list.push_back(makeHighlight(2, 50, 60, "later"));
  list.push_back(makeHighlight(0, 90, 95, "second"));
  list.push_back(makeHighlight(0, 10, 20, "first"));
  expect(annotations::normalize(list), "normalize accepts a small list");
  expect(list.size() == 3 && list[0].quote == "first" && list[2].quote == "later", "normalize sorts by spine+offset");

  std::vector<annotations::Annotation> dupes;
  dupes.push_back(makeHighlight(0, 10, 20, "same"));
  dupes.push_back(makeHighlight(0, 10, 20, "same"));
  expect(annotations::normalize(dupes), "normalize accepts duplicates");
  expect(dupes.size() == 1, "normalize keeps one entry for the same range and quote");

  // Two different selections can share one reflowed PDF line's range; the quote
  // is part of the identity, so neither may be dropped.
  std::vector<annotations::Annotation> sameRange;
  sameRange.push_back(makeHighlight(0, 10, 20, "first quote"));
  sameRange.push_back(makeHighlight(0, 10, 20, "second quote"));
  expect(annotations::normalize(sameRange), "normalize accepts same-range quotes");
  expect(sameRange.size() == 2, "normalize keeps distinct quotes on one range");

  std::vector<annotations::Annotation> tooMany(annotations::kMaxCount + 1);
  expect(!annotations::normalize(tooMany), "normalize rejects more than the cap");
}

std::vector<uint8_t> goodBytes() {
  return annotations::serialize({makeHighlight(0, 100, 112, "quote")});
}

void expectRejected(std::vector<uint8_t> bytes, const char* what) {
  std::vector<annotations::Annotation> out;
  expect(!annotations::deserialize(bytes.data(), bytes.size(), out),
         what);
  expect(out.empty(), "a rejected file leaves the output untouched");
}

void testRejections() {
  auto bytes = goodBytes();
  expect(!bytes.empty(), "serialize produced bytes");

  {
    auto bad = bytes;
    bad[0] ^= 0xFF;  // magic
    expectRejected(bad, "rejects a wrong magic");
  }
  {
    auto bad = bytes;
    bad[4] = 2;  // version
    expectRejected(bad, "rejects a wrong version");
  }
  {
    auto bad = bytes;
    bad[6] = 0xFF;  // count low byte
    bad[7] = 0x7F;  // count high byte -> 32767 > cap
    expectRejected(bad, "rejects a count over the cap");
  }
  {
    auto bad = bytes;
    bad.resize(bad.size() - 1);  // truncated string
    expectRejected(bad, "rejects a truncated entry");
  }
  {
    auto bad = bytes;
    bad.push_back(0);
    expectRejected(bad, "rejects trailing bytes");
  }
  {
    // start == end is not a range.
    std::vector<annotations::Annotation> self = {makeHighlight(0, 40, 40, "x")};
    auto raw = annotations::serialize(self);
    expectRejected(raw, "rejects a zero-length range");
  }
  {
    // A string length that could never have been written.
    auto bad = goodBytes();
    // The quote length sits right after the 8-byte file header and the 11-byte
    // fixed entry header; overwrite it with a value above the cap.
    const size_t entryStart = 8;
    const size_t quoteLen = entryStart + 2 + 4 + 4 + 1;
    bad[quoteLen] = 0xFF;
    bad[quoteLen + 1] = 0x00;
    expectRejected(bad, "rejects an oversized string length");
  }
}

void testRangeTests() {
  const auto a = makeHighlight(0, 100, 112, "q");
  expect(annotations::contains(a, 100), "contains includes the start");
  expect(!annotations::contains(a, 112), "contains excludes the end");
  expect(annotations::overlaps(a, 0, 101), "overlaps a word crossing the start");
  expect(!annotations::overlaps(a, 112, 120), "does not overlap past the end");
}

void testExport() {
  std::vector<annotations::Annotation> list;
  list.push_back(makeHighlight(0, 10, 20, "高亮句子"));
  annotations::Annotation note = makeHighlight(0, 30, 40, "有笔记的句子");
  note.kind = static_cast<uint8_t>(annotations::Kind::Note);
  note.note = "备注正文";
  list.push_back(note);

  const std::string text = annotations::formatExport("测试书", "2026-11-09", list);
  expect(text.find("《测试书》") == 0, "export starts with the book title");
  expect(text.find("── 第一章") != std::string::npos, "export groups by chapter");
  expect(text.find("高亮：高亮句子") != std::string::npos, "export writes the quote");
  expect(text.find("批注：备注正文") != std::string::npos, "export writes the note");

  const std::string empty = annotations::formatExport("测试书", "2026-11-09", {});
  expect(empty.find("（暂无批注）") != std::string::npos, "export marks an empty list");
}

}  // namespace

int main() {
  testRoundTrip();
  testUtf8Clamp();
  testNormalize();
  testRejections();
  testRangeTests();
  testExport();
  if (failures == 0) {
    std::printf("PASS: annotation store (all checks)\n");
    return 0;
  }
  std::printf("annotation store: %d failure(s)\n", failures);
  return 1;
}
