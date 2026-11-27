// Host regression for annotations::normalize() -- the shared annotation identity
// rule that the EPUB and PDF readers both run their lists through.
//
// Why this file exists: the PDF reader anchors a highlight by LINE INDEX (a
// reflowed PDF page has no stable text offset), so two different selections on one
// line produce an identical (spineIndex, startOffset, endOffset).  normalize()
// deduped on that triple alone and silently deleted one of the user's highlights.
// The quote is in the key now; these tests pin both halves of that decision --
// different quotes survive, and an edit (same quote, new note) still replaces.
//
// Also pinned here, because it is the trap that let the bug hide: normalize() only
// clamps quote/note/chapterTitle when endOffset > startOffset, so a record written
// with a degenerate 0/0 range stores unbounded fields.
#include <cstdio>
#include <string>
#include <vector>

#include "AnnotationStore.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++checks;                                                              \
    if (!(cond)) {                                                         \
      ++failures;                                                          \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                      \
  } while (0)

using annotations::Annotation;
using annotations::Kind;

static Annotation make(const uint16_t spine, const uint32_t start, const uint32_t end,
                       const std::string& quote, const std::string& note = {}) {
  Annotation a{};
  a.spineIndex = spine;
  a.startOffset = start;
  a.endOffset = end;
  a.kind = note.empty() ? static_cast<uint8_t>(Kind::Highlight) : static_cast<uint8_t>(Kind::Note);
  a.quote = quote;
  a.note = note;
  a.chapterTitle = "title";
  return a;
}

// The bug being fixed: one PDF line, two different selections.
static void testSameLineDifferentQuoteBothSurvive() {
  std::vector<Annotation> list;
  list.push_back(make(3, 7, 8, "第一个词"));
  list.push_back(make(3, 7, 8, "第二个词"));
  CHECK(annotations::normalize(list));
  CHECK(list.size() == 2);
}

// Three selections that share one line still must not collapse pairwise.
static void testManyOnOneLine() {
  std::vector<Annotation> list;
  for (int i = 0; i < 6; ++i) {
    list.push_back(make(0, 2, 3, "quote-" + std::to_string(i)));
  }
  CHECK(annotations::normalize(list));
  CHECK(list.size() == 6);
}

// normalize()'s collapse must still fire for a genuinely identical selection.
// (EPUB edits by index rather than relying on this, but re-highlighting the same range
// produces the same key, and two entries for one selection would be a duplicate list.)
static void testEditReplacesRatherThanDuplicates() {
  std::vector<Annotation> list;
  list.push_back(make(1, 0, 9, "同一段", "旧批注"));
  list.push_back(make(1, 0, 9, "同一段", "新批注"));
  CHECK(annotations::normalize(list));
  CHECK(list.size() == 1);
  // "most recently written wins": normalize is a stable sort, so the later entry
  // is last within the group and is the one kept.
  CHECK(!list.empty() && list[0].note == "新批注");
}

static void testDifferentPagesBothSurvive() {
  std::vector<Annotation> list;
  list.push_back(make(1, 0, 1, "same quote"));
  list.push_back(make(2, 0, 1, "same quote"));
  CHECK(annotations::normalize(list));
  CHECK(list.size() == 2);
}

// Ordering is part of the contract (the merged list and export walk it in order).
static void testSortedBySpineThenStart() {
  std::vector<Annotation> list;
  list.push_back(make(2, 5, 6, "b"));
  list.push_back(make(1, 9, 10, "c"));
  list.push_back(make(1, 1, 2, "a"));
  CHECK(annotations::normalize(list));
  CHECK(list.size() == 3);
  CHECK(list[0].spineIndex == 1 && list[0].startOffset == 1);
  CHECK(list[1].spineIndex == 1 && list[1].startOffset == 9);
  CHECK(list[2].spineIndex == 2);
}

// This is the second half of the PDF bug: a degenerate range skips the clamp, so
// the old 0/0 records could persist an unbounded quote/note.
static void testClampOnlyAppliesToRealRanges() {
  const std::string longText(annotations::kMaxQuoteBytes * 3, 'x');
  std::vector<Annotation> degenerate;
  degenerate.push_back(make(0, 0, 0, longText));
  CHECK(annotations::normalize(degenerate));
  CHECK(degenerate[0].quote.size() == longText.size());  // documented, not desired

  std::vector<Annotation> real;
  real.push_back(make(0, 0, 1, longText));
  CHECK(annotations::normalize(real));
  CHECK(real[0].quote.size() == annotations::kMaxQuoteBytes);
}

// Two quote-distinct entries have to survive the file too, or the fix only lasts
// until the next reopen.
static void testRoundTripKeepsBoth() {
  std::vector<Annotation> list;
  list.push_back(make(3, 7, 8, "甲"));
  list.push_back(make(3, 7, 8, "乙"));
  CHECK(annotations::normalize(list));
  const std::vector<uint8_t> bytes = annotations::serialize(list);
  std::vector<Annotation> back;
  CHECK(annotations::deserialize(bytes.data(), bytes.size(), back));
  CHECK(back.size() == 2);
  CHECK(back[0].quote != back[1].quote);
}

static void testOverCapRejected() {
  std::vector<Annotation> list;
  for (size_t i = 0; i <= annotations::kMaxCount; ++i) {
    list.push_back(make(0, 0, 1, "q" + std::to_string(i)));
  }
  CHECK(!annotations::normalize(list));
}

int main() {
  testSameLineDifferentQuoteBothSurvive();
  testManyOnOneLine();
  testEditReplacesRatherThanDuplicates();
  testDifferentPagesBothSurvive();
  testSortedBySpineThenStart();
  testClampOnlyAppliesToRealRanges();
  testRoundTripKeepsBoth();
  testOverCapRejected();

  std::printf("annotation store: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
