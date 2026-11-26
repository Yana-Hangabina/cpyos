// Host regression for the CloudZao installed-resource record ("CZR1").
//
// The record decides whether a downloaded resource stays visible (and
// deletable) after the server acknowledged its delivery, so a parser bug hides
// files rather than failing loudly. Every rule here is pinned:
//  - exact wire format and round-trip,
//  - a wrong/absent magic rejects the whole file,
//  - malformed lines are skipped without dropping the valid ones,
//  - unsafe paths (relative, "..") never come back as entries,
//  - the entry cap and the byte cap hold.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "plus_resources_record.h"

namespace rec = plus_resources_record;

static int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static rec::Entry makeEntry(char type, const char* path, uint64_t size, const char* sha) {
  rec::Entry entry;
  entry.type = type;
  entry.path = path;
  entry.file_size = size;
  entry.sha256 = sha;
  return entry;
}

static void testExactFormat() {
  std::vector<rec::Entry> entries;
  entries.push_back(makeEntry('B', "/books/novel.epub", 1234, "abc123"));
  const std::string text = rec::encode(entries);
  CHECK(text == "CZR1\nB\t/books/novel.epub\t1234\tabc123\n");
}

static void testRoundTrip() {
  std::vector<rec::Entry> entries;
  entries.push_back(makeEntry('B', "/books/a.epub", 100, "aa"));
  entries.push_back(makeEntry('G', "/badges/wall.png", 200, "bb"));
  entries.push_back(makeEntry('F', "/fonts/Thing/thing.ttf", 300, "cc"));
  std::vector<rec::Entry> parsed;
  CHECK(rec::decode(rec::encode(entries), parsed));
  CHECK(parsed.size() == 3);
  CHECK(parsed[0].type == 'B' && parsed[0].path == "/books/a.epub" && parsed[0].file_size == 100 &&
        parsed[0].sha256 == "aa");
  CHECK(parsed[1].type == 'G' && parsed[1].path == "/badges/wall.png");
  CHECK(parsed[2].type == 'F' && parsed[2].file_size == 300);
}

static void testMissingMagicRejects() {
  std::vector<rec::Entry> parsed;
  CHECK(!rec::decode("B\t/books/a.epub\t1\taa\n", parsed));
  CHECK(parsed.empty());
  CHECK(!rec::decode("", parsed));
  CHECK(!rec::decode("CZR2\nB\t/books/a.epub\t1\taa\n", parsed));
}

static void testCrlfTolerated() {
  std::vector<rec::Entry> parsed;
  CHECK(rec::decode("CZR1\r\nB\t/books/a.epub\t10\taa\r\n", parsed));
  CHECK(parsed.size() == 1);
  CHECK(parsed[0].path == "/books/a.epub");
  CHECK(parsed[0].sha256 == "aa");
}

static void testBadLinesSkipped() {
  std::vector<rec::Entry> parsed;
  const std::string text =
      "CZR1\n"
      "no tabs here\n"
      "B\t/books/good.epub\t7\taa\n"
      "X\t/books/wrongtype.epub\t7\taa\n"
      "B\trelative/path.epub\t7\taa\n"
      "B\t/books/../escape.epub\t7\taa\n"
      "B\t/short\tno-size-sha\n"
      "\n"
      "G\t/badges/ok.png\t9\tbb\n";
  CHECK(rec::decode(text, parsed));
  CHECK(parsed.size() == 2);
  CHECK(parsed[0].path == "/books/good.epub");
  CHECK(parsed[1].path == "/badges/ok.png");
}

static void testEncodeSkipsUnsafe() {
  std::vector<rec::Entry> entries;
  entries.push_back(makeEntry('B', "relative.epub", 1, "aa"));
  entries.push_back(makeEntry('B', "/books/../x.epub", 1, "aa"));
  entries.push_back(makeEntry('?', "/books/unknown.epub", 1, "aa"));
  entries.push_back(makeEntry('B', "/books/ok.epub", 1, "aa"));
  const std::string text = rec::encode(entries);
  CHECK(text == "CZR1\nB\t/books/ok.epub\t1\taa\n");
}

static void testEntryCap() {
  std::vector<rec::Entry> entries;
  for (size_t i = 0; i < rec::kMaxEntries + 10; ++i) {
    char path[64];
    std::snprintf(path, sizeof(path), "/books/b%03u.epub", static_cast<unsigned>(i));
    entries.push_back(makeEntry('B', path, i, "aa"));
  }
  std::vector<rec::Entry> parsed;
  CHECK(rec::decode(rec::encode(entries), parsed));
  CHECK(parsed.size() == rec::kMaxEntries);
  CHECK(parsed.back().path == "/books/b063.epub");
}

static void testByteCap() {
  std::string oversized = "CZR1\n";
  oversized.append(rec::kMaxBytes, 'x');
  std::vector<rec::Entry> parsed;
  CHECK(!rec::decode(oversized, parsed));
}

static void testPathSafety() {
  CHECK(rec::pathIsSafe("/books/a.epub"));
  CHECK(!rec::pathIsSafe(""));
  CHECK(!rec::pathIsSafe("/"));
  CHECK(!rec::pathIsSafe("books/a.epub"));
  CHECK(!rec::pathIsSafe("/books/../a.epub"));
  CHECK(!rec::pathIsSafe(std::string("/books/") + std::string(250, 'a')));
}

int main() {
  testExactFormat();
  testRoundTrip();
  testMissingMagicRejects();
  testCrlfTolerated();
  testBadLinesSkipped();
  testEncodeSkipsUnsafe();
  testEntryCap();
  testByteCap();
  testPathSafety();
  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("plus resources record: all checks passed\n");
  return 0;
}
