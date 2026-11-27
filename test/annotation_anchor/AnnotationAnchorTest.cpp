// Host test for AnnotationAnchor.h: which words of a screen belong to an
// existing PDF annotation.  The PDF reader stores highlights as captured quotes
// (no stable text offsets), so re-selecting a highlight on press is an interval
// question.  These are the cases the device would otherwise have to teach us.
#include "activities/reader/AnnotationAnchor.h"

#include <cstdio>
#include <string>
#include <vector>

static int checks = 0;
static int failures = 0;
#define CHECK(cond)                                        \
  do {                                                     \
    ++checks;                                              \
    if (!(cond)) {                                         \
      ++failures;                                          \
      printf("FAIL %d: %s\n", __LINE__, #cond);            \
    }                                                      \
  } while (0)

using anno_anchor::isWholePageMarker;
using anno_anchor::Word;

// A screen of 3 lines x 4 words, 5 bytes each.
static std::vector<Word> screen() {
  std::vector<Word> words;
  for (int line = 0; line < 3; ++line) {
    for (int i = 0; i < 4; ++i) {
      words.push_back(Word{static_cast<uint16_t>(line), static_cast<uint32_t>(i * 5),
                           static_cast<uint32_t>(i * 5 + 5)});
    }
  }
  return words;
}

template <typename F>
int run(const std::vector<Word>& words, const size_t tapped, F coverOf, int& first, int& last) {
  return anno_anchor::rangeForWord(words, tapped, coverOf, first, last);
}

static void testHalfOpenEdges() {
  CHECK(anno_anchor::overlaps(0, 5, 5, 10) == false);   // word ends exactly where span starts
  CHECK(anno_anchor::overlaps(5, 10, 0, 5) == false);   // word starts exactly where span ends
  CHECK(anno_anchor::overlaps(0, 6, 5, 10) == true);    // one byte of real overlap
  CHECK(anno_anchor::overlaps(0, 5, -1, -1) == false);  // no span on this line
  CHECK(anno_anchor::overlaps(0, 5, 3, 3) == false);    // empty span
}

static void testSingleLineRange() {
  const auto words = screen();
  int first = -1;
  int last = -1;
  // Annotation covers bytes 0..15 of line 1 => words 0,1,2 of that line.
  const int found = run(words, 5, [&](const size_t i) {
    const Word& w = words[i];
    return (w.line == 1 && anno_anchor::overlaps(w.start, w.end, 0, 15)) ? 0 : -1;
  }, first, last);
  CHECK(found == 0);
  CHECK(first == 4);   // line 1 word 0 is global index 4
  CHECK(last == 6);    // line 1 word 2
}

static void testMultiLineAnnotation() {
  const auto words = screen();
  int first = -1;
  int last = -1;
  // A quote that spans lines 0 (from byte 10) to 2 (to byte 10).
  const int found = run(words, 2, [&](const size_t i) {
    const Word& w = words[i];
    const int s = w.line == 0 ? 10 : 0;
    const int e = w.line == 2 ? 10 : 20;
    return anno_anchor::overlaps(w.start, w.end, s, e) ? 0 : -1;
  }, first, last);
  CHECK(found == 0);
  CHECK(first == 2);
  CHECK(last == 9);

  // One word earlier on the same line is outside the span: byte 5..10 against a
  // span starting at 10.  This is the exact mistake the first draft of this test
  // made, so it is pinned rather than trusted.
  int nearFirst = -1;
  int nearLast = -1;
  CHECK(run(words, 1, [&](const size_t i) {
            const Word& w = words[i];
            const int s = w.line == 0 ? 10 : 0;
            const int e = w.line == 2 ? 10 : 20;
            return anno_anchor::overlaps(w.start, w.end, s, e) ? 0 : -1;
          }, nearFirst, nearLast) == -1);
}

static void testPressOutsideFindsNothing() {
  const auto words = screen();
  int first = -1;
  int last = -1;
  const int found = run(words, 3, [&](const size_t) { return -1; }, first, last);
  CHECK(found == -1);
  CHECK(first == -1);
  CHECK(last == -1);
}

static void testNearestAnnotationWins() {
  const auto words = screen();
  int first = -1;
  int last = -1;
  // Two annotations overlap word 0; the resolver reports one, and the widening
  // must not mix words from the other.
  const int found = run(words, 0, [&](const size_t i) {
    const Word& w = words[i];
    if (w.line != 0) return -1;
    if (w.start < 10) return 0;   // annotation 0: words 0,1
    if (w.start < 20) return 1;   // annotation 1: word 2,3
    return -1;
  }, first, last);
  CHECK(found == 0);
  CHECK(first == 0);
  CHECK(last == 1);
}

static void testTappedOutOfRange() {
  const auto words = screen();
  int first = -1;
  int last = -1;
  CHECK(run(words, words.size() + 4, [&](const size_t) { return 0; }, first, last) == -1);
  const std::vector<Word> empty;
  CHECK(run(empty, 0, [&](const size_t) { return 0; }, first, last) == -1);
}


// The page-note path and the selection paths share one record type, so telling
// "a note about this page" from "a note about these words" is a real rule. It used
// to be "any annotation on this page", which made the page-note editor prefill
// from a selection note and then delete every annotation on the page.
static void testWholePageMarkerIsExact() {
  CHECK(isWholePageMarker(0, 1));
  CHECK(!isWholePageMarker(0, 0));   // degenerate: not a marker
  CHECK(!isWholePageMarker(0, 2));   // two lines selected
  CHECK(!isWholePageMarker(7, 8));   // a selection on line 7
  CHECK(!isWholePageMarker(1, 1));
}

int main() {

  testHalfOpenEdges();
  testSingleLineRange();
  testMultiLineAnnotation();
  testPressOutsideFindsNothing();
  testNearestAnnotationWins();
  testTappedOutOfRange();
  testWholePageMarkerIsExact();
  printf("\nannotation anchor: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
