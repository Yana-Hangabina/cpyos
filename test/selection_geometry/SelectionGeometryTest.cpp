// Host test for the pure selection geometry shared by the EPUB and PDF readers.
// No Arduino/ESP headers: it compiles SelectionGeometry.h directly and pins the
// hit-test, range, drag-clamp, handle-geometry and handle-hit rules.  The run
// script also runs mutation steps (break one rule, the suite must go red).
#include "activities/reader/SelectionGeometry.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using selection_geom::WordRect;
using selection_geom::Range;

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    ++checks;                                                           \
    if (!(cond)) {                                                      \
      ++failures;                                                       \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                   \
  } while (0)

// A simple two-line page: line 0 has words 0..2, line 1 has words 0..1.
// Horizontal layout, each word 20 px wide, 30 px tall, 40 px line pitch.
static std::vector<WordRect> makeWords() {
  std::vector<WordRect> v;
  int idx = 0;
  for (int line = 0; line < 2; ++line) {
    const int count = line == 0 ? 3 : 2;
    for (int w = 0; w < count; ++w) {
      WordRect r;
      r.x = static_cast<int16_t>(10 + w * 20);
      r.y = static_cast<int16_t>(20 + line * 40);
      r.w = 20;
      r.h = 30;
      r.line = static_cast<uint16_t>(line);
      r.word = static_cast<uint16_t>(w);
      r.start = static_cast<uint32_t>(idx * 5);
      r.end = static_cast<uint32_t>(idx * 5 + 5);
      v.push_back(r);
      ++idx;
    }
  }
  return v;
}

static void testBefore() {
  using selection_geom::before;
  CHECK(before(0, 0, 0, 1));
  CHECK(!before(0, 1, 0, 0));
  CHECK(before(0, 5, 1, 0));
  CHECK(!before(1, 0, 0, 5));
  CHECK(!before(1, 1, 1, 1));
}

static void testCovers() {
  const auto words = makeWords();
  // Range line0 word1 .. line1 word0 covers the middle of line 0, all of it,
  // and the first word of line 1.
  Range r{0, 1, 1, 0};
  CHECK(!selection_geom::covers(r, 0, 0));
  CHECK(selection_geom::covers(r, 0, 1));
  CHECK(selection_geom::covers(r, 0, 2));
  CHECK(selection_geom::covers(r, 1, 0));
  CHECK(!selection_geom::covers(r, 1, 1));
  // Reversed range (start after end) still covers the same span.
  Range rev{1, 0, 0, 1};
  CHECK(!selection_geom::covers(rev, 0, 0));
  CHECK(selection_geom::covers(rev, 0, 1));
  CHECK(selection_geom::covers(rev, 0, 2));
  CHECK(selection_geom::covers(rev, 1, 0));
  // Single word.
  Range one{0, 2, 0, 2};
  CHECK(selection_geom::covers(one, 0, 2));
  CHECK(!selection_geom::covers(one, 0, 1));
  CHECK(!selection_geom::covers(one, 1, 0));
}

static void testNearest() {
  const auto words = makeWords();
  // Exact hit inside word (0,0): x in [10,30), y in [20,50).
  CHECK(selection_geom::nearest(words, 15, 25) == 0);
  // Exact hit inside word (1,1): index 4.
  CHECK(selection_geom::nearest(words, 35, 65) == 4);
  // A press in the gap above the page snaps to the nearest word, never -1.
  CHECK(selection_geom::nearest(words, 15, 5) == 0);
  // Empty list returns -1.
  std::vector<WordRect> empty;
  CHECK(selection_geom::nearest(empty, 10, 10) == -1);
}

static void testDragClamp() {
  // Start handle cannot cross to the right of the end.
  Range r{0, 0, 0, 2};
  CHECK(!selection_geom::moveStartTo(r, 0, 3));  // past end -> rejected
  CHECK(r.startWord == 0);                       // unchanged
  CHECK(selection_geom::moveStartTo(r, 0, 1));   // valid move
  CHECK(r.startLine == 0 && r.startWord == 1);
  CHECK(!selection_geom::moveStartTo(r, 0, 1));  // no-op returns false

  // End handle cannot cross to the left of the start.
  Range e{0, 1, 0, 2};
  CHECK(!selection_geom::moveEndTo(e, 0, 0));  // before start -> rejected
  CHECK(e.endWord == 2);
  CHECK(selection_geom::moveEndTo(e, 1, 1));  // valid move
  CHECK(e.endLine == 1 && e.endWord == 1);
}

static void testExtend() {
  // Anchor at (0,1); tapping left of the anchor grows the start, not the end.
  Range r{0, 1, 0, 1};
  selection_geom::extendTo(r, 0, 1, 0, 0);
  CHECK(r.startLine == 0 && r.startWord == 0);
  CHECK(r.endLine == 0 && r.endWord == 1);
  // Tapping right of the anchor grows the end.
  Range r2{0, 1, 0, 1};
  selection_geom::extendTo(r2, 0, 1, 1, 0);
  CHECK(r2.startLine == 0 && r2.startWord == 1);
  CHECK(r2.endLine == 1 && r2.endWord == 0);
}

static void testCoveredEnds() {
  const auto words = makeWords();
  Range r{0, 1, 1, 0};
  auto e = selection_geom::coveredEnds(words, r);
  CHECK(e.found);
  CHECK(e.firstX == 30 && e.firstY == 20);  // word (0,1)
  CHECK(e.lastX == 10 && e.lastY == 60);    // word (1,0)
  // A range that covers nothing reports found=false.
  Range none{1, 1, 1, 1};
  auto e2 = selection_geom::coveredEnds(words, none);
  CHECK(e2.found);  // (1,1) exists
  std::vector<WordRect> w2 = makeWords();
  Range missing{5, 0, 5, 0};
  CHECK(!selection_geom::coveredEnds(w2, missing).found);
}

static void testHandleGeometry() {
  selection_geom::Ends e;
  e.found = true;
  e.firstX = 100;
  e.firstY = 20;
  e.firstW = 20;
  e.firstH = 30;
  e.lastX = 200;
  e.lastY = 60;
  e.lastW = 20;
  e.lastH = 30;
  const int lineH = 30;
  const int screenW = 684;
  auto h = selection_geom::handleGeometry(e, /*vertical=*/false, screenW, lineH);
  // Start handle sits left of the first word; end handle right of the last.
  CHECK(h.x[0] == 100 - 24 - 4);
  CHECK(h.x[1] == 200 + 20 + 4);
  CHECK(h.w[0] == 24 && h.h[1] == 24);
  // Vertical handles are centred on the column and above/below.
  auto hv = selection_geom::handleGeometry(e, /*vertical=*/true, screenW, lineH);
  CHECK(hv.y[0] == 20 - 24 - 4);
  CHECK(hv.y[1] == 60 + 30 + 4);
  // A range at the very left edge clamps the start handle on-screen.
  selection_geom::Ends edge;
  edge.found = true;
  edge.firstX = 3;
  edge.firstY = 20;
  edge.firstW = 20;
  edge.firstH = 30;
  edge.lastX = 3;
  edge.lastY = 20;
  edge.lastW = 20;
  edge.lastH = 30;
  auto he = selection_geom::handleGeometry(edge, false, screenW, lineH);
  CHECK(he.x[0] >= 2);
  CHECK(he.x[0] + 24 <= screenW - 2);
  // Not found -> no handles.
  selection_geom::Ends none;
  auto hn = selection_geom::handleGeometry(none, false, screenW, lineH);
  CHECK(hn.w[0] == 0 && hn.w[1] == 0);
}

static void testHitHandle() {
  selection_geom::Ends e;
  e.found = true;
  e.firstX = 100;
  e.firstY = 20;
  e.firstW = 20;
  e.firstH = 30;
  e.lastX = 200;
  e.lastY = 20;
  e.lastW = 20;
  e.lastH = 30;
  auto h = selection_geom::handleGeometry(e, false, 684, 30);
  // Dead centre of each handle hits that handle.
  CHECK(selection_geom::hitHandle(h, h.x[0] + 12, h.y[0] + 12) == 0);
  CHECK(selection_geom::hitHandle(h, h.x[1] + 12, h.y[1] + 12) == 1);
  // Just inside the slop margin still hits.
  CHECK(selection_geom::hitHandle(h, h.x[0] - 11, h.y[0] + 12) == 0);
  // Far from both handles misses.
  CHECK(selection_geom::hitHandle(h, 5, 5) == -1);
  // Degenerate (no) handles never hit.
  selection_geom::Handles empty;
  CHECK(selection_geom::hitHandle(empty, 10, 10) == -1);
}

// Key widening: Left/Right walk reading order, Up/Down keep the column. The two
// "no move" edges and the missing-word guard are the parts a reader gets wrong
// silently -- defaulting a missing focus to index 0 yanks the selection to the
// first word of the page after any renumbering.
static void testKeyStep() {
  using selection_geom::indexOf;
  using selection_geom::KeyStep;
  using selection_geom::stepWord;
  const auto words = makeWords();

  CHECK(indexOf(words, 0, 0) == 0);
  CHECK(indexOf(words, 0, 2) == 2);
  CHECK(indexOf(words, 1, 1) == 4);
  CHECK(indexOf(words, 1, 2) == -1);  // that word is not on this page
  CHECK(indexOf(words, 2, 0) == -1);

  CHECK(stepWord(words, 0, KeyStep::Left, 20) == -1);   // first word: no move
  CHECK(stepWord(words, 4, KeyStep::Right, 40) == -1);  // last word: no move
  CHECK(stepWord(words, 3, KeyStep::Left, 20) == 2);    // back into the previous line
  CHECK(stepWord(words, 2, KeyStep::Right, 60) == 3);   // into the next line
  CHECK(stepWord(words, -1, KeyStep::Right, 20) == -1);
  CHECK(stepWord(words, 99, KeyStep::Left, 20) == -1);
  const std::vector<WordRect> none;
  CHECK(stepWord(none, 0, KeyStep::Right, 20) == -1);

  // Vertical: nearest column on the neighbouring line, never the line start.
  CHECK(stepWord(words, 0, KeyStep::Up, 20) == -1);     // no line above the first
  CHECK(stepWord(words, 4, KeyStep::Up, 40) == 1);      // centre 40 -> word 1
  CHECK(stepWord(words, 3, KeyStep::Up, 20) == 0);
  CHECK(stepWord(words, 0, KeyStep::Down, 60) == 4);    // 60 is nearer 40 than 20
  CHECK(stepWord(words, 4, KeyStep::Down, 1000) == -1); // no line below the last

  // Both readers feed their own rectangle: the EPUB one carries extra fields,
  // and the rule must not fork because of it.
  struct Wide {
    int16_t x, y, w, h;
    uint16_t line, word;
    const char* text;
  };
  const std::vector<Wide> wide = {{10, 20, 20, 30, 0, 0, "a"}, {30, 20, 20, 30, 0, 1, "b"},
                                  {10, 60, 20, 30, 1, 0, "c"}};
  CHECK(indexOf(wide, 0, 1) == 1);
  CHECK(indexOf(wide, 1, 1) == -1);
  CHECK(stepWord(wide, 1, KeyStep::Down, 40) == 2);
  CHECK(stepWord(wide, 2, KeyStep::Up, 40) == 1);

  // A single-word line still resolves by column, not by index.
  std::vector<WordRect> one = words;
  one.erase(one.begin() + 4);
  CHECK(stepWord(one, 3, KeyStep::Up, 40) == 1);
  CHECK(stepWord(one, 1, KeyStep::Down, 40) == 3);
}

int main() {
  testBefore();
  testCovers();
  testNearest();
  testDragClamp();
  testExtend();
  testCoveredEnds();
  testHandleGeometry();
  testHitHandle();
  testKeyStep();
  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
