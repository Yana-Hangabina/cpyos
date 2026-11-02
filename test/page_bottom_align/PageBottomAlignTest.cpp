// Host test for lib/Epub/Epub/PageBottomAlign.h (vertical page bottom align).
//
// The distribution math is the only part of the feature that can be checked
// without the e-ink HAL, so it is a pure function and this test pins it:
// flush-to-bottom pages, the half-line safety cap, the "never move line 0"
// invariant, and the degenerate inputs. Run with run_page_bottom_align_test.sh.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "PageBottomAlign.h"

namespace {

int gChecks = 0;
int gFailures = 0;

void expectEq(const int actual, const int expected, const std::string& what) {
  ++gChecks;
  if (actual != expected) {
    ++gFailures;
    std::printf("FAIL %s: got %d, want %d\n", what.c_str(), actual, expected);
  }
}

void expectTrue(const bool value, const std::string& what) {
  ++gChecks;
  if (!value) {
    ++gFailures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

// Bottom edge of the last line after the plan is applied.
int alignedBottom(const pageBottomAlign::Plan& p, const int contentBottom, const int lineCount) {
  return contentBottom + pageBottomAlign::shiftFor(p, lineCount - 1);
}

}  // namespace

int main() {
  // 1) Ordinary page: 13 lines of 60 px in an 800 px viewport, cursor at 740.
  //    60 px of leftover over 12 gaps = +5 px each, under the half-line cap, so
  //    the last line lands exactly on the content bottom.
  {
    const auto p = pageBottomAlign::plan(800, 740, 13, 60);
    expectTrue(p.distribute, "ordinary page distributes");
    expectEq(p.extraPerGap, 5, "ordinary extra per gap");
    expectEq(p.remainder, 0, "ordinary remainder");
    expectEq(alignedBottom(p, 740, 13), 800, "ordinary last line is flush");
    expectEq(pageBottomAlign::shiftFor(p, 0), 0, "first line never moves");
    // Strictly increasing: no line may overtake the next.
    for (int i = 1; i < 13; ++i) {
      expectTrue(pageBottomAlign::shiftFor(p, i) > pageBottomAlign::shiftFor(p, i - 1),
                 "shifts increase with line index");
    }
  }

  // 2) Remainder pixels: 13 lines of 60 in a 790 px viewport (cursor 740) leaves
  //    50 px over 12 gaps = 4 px each plus 2 spare pixels on the first two gaps.
  {
    const auto p = pageBottomAlign::plan(790, 740, 13, 60);
    expectTrue(p.distribute, "remainder page distributes");
    expectEq(p.extraPerGap, 4, "remainder extra per gap");
    expectEq(p.remainder, 2, "remainder count");
    expectEq(alignedBottom(p, 740, 13), 790, "remainder page is still flush");
    expectEq(pageBottomAlign::shiftFor(p, 1), 5, "first gap gets the spare pixel");
    expectEq(pageBottomAlign::shiftFor(p, 2), 10, "second gap gets the spare pixel");
    // Gap 3 is the first one that gets no spare pixel, so line 3 carries both.
    expectEq(pageBottomAlign::shiftFor(p, 3), 14, "third line carries the two spare pixels");
  }

  // 3) Sparse page (chapter with four lines in an 800 px viewport): the cap
  //    stops the stretch at half a line height per gap, so the excess stays at
  //    the bottom instead of one huge gap in the middle.
  {
    const auto p = pageBottomAlign::plan(800, 240, 4, 60);
    expectTrue(p.distribute, "sparse page distributes up to the cap");
    expectEq(p.extraPerGap, 30, "sparse page capped at half a line");
    expectEq(p.remainder, 0, "no remainder when capped");
    expectEq(alignedBottom(p, 240, 4), 330, "sparse page leaves the excess at the bottom");
  }

  // 4) Degenerate inputs: a single line cannot span top and bottom, a full page
  //    has nothing to spread, and a page that overflows is left alone.
  expectTrue(!pageBottomAlign::plan(800, 100, 1, 60).distribute, "one line does not distribute");
  expectTrue(!pageBottomAlign::plan(800, 0, 0, 60).distribute, "empty page does not distribute");
  expectTrue(!pageBottomAlign::plan(800, 800, 5, 60).distribute, "full page does not distribute");
  expectTrue(!pageBottomAlign::plan(800, 900, 5, 60).distribute, "overflowing page does not distribute");
  expectTrue(!pageBottomAlign::plan(800, 799, 5, 60).distribute, "one leftover pixel does not move a line");

  // 5) Tiny line heights: the cap floor is one pixel, so a 1 px line still gets
  //    a bounded stretch rather than zero.
  {
    const auto p = pageBottomAlign::plan(100, 90, 6, 1);
    expectTrue(p.distribute, "tiny line height still distributes");
    expectEq(p.extraPerGap, 1, "cap floor is 1 px");
  }

  // 6) The cap is exactly half a line height, inclusive: a 30 px leftover with
  //    one gap is applied in full, a 25 px one is exact, 31 px clamps back to 30.
  {
    expectEq(pageBottomAlign::plan(800, 770, 2, 60).extraPerGap, 30, "cap boundary uses half a line");
    expectEq(pageBottomAlign::plan(800, 775, 2, 60).extraPerGap, 25, "just under the cap is exact");
    expectEq(pageBottomAlign::plan(800, 769, 2, 60).extraPerGap, 30, "above the cap clamps to half a line");
  }

  // 7) A page the parser cannot redistribute (image or table) never reaches this
  //    code path; the pure function itself is geometry-only.

  std::printf("%s: %d checks, %d failures\n", gFailures == 0 ? "PASS" : "FAIL", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
