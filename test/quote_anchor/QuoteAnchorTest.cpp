// Host regression for quote_anchor::findLineRange -- how a PDF highlight finds its
// text again after the page has been re-laid out.
//
// The case that motivated this is the two objectives colliding: 智能合并断行 joins
// hard-wrapped CJK lines WITHOUT a space, so a quote captured before the toggle holds
// a space that the new page cannot produce.  Before the second pass existed, the match
// failed and the caller fell back to the line numbers stored at creation -- numbers
// that describe a layout with one MORE line than exists now, so the highlight drifted
// onto the wrong lines or vanished.
#include <cstdio>
#include <string>
#include <vector>

#include "activities/reader/QuoteAnchor.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                            \
  do {                                                         \
    ++checks;                                                  \
    if (!(cond)) {                                             \
      ++failures;                                              \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                          \
  } while (0)

using Lines = std::vector<std::string>;

static const char* kHelp = "\xe6\x9c\xac\xe4\xb9\xa6\xe5\xa6\x82\xe4\xbd\x95\xe5\xb8\xae\xe5\x8a\xa9";  // 本书如何帮助
static const char* kYou = "\xe4\xbd\xa0\xe6\x88\x90\xe4\xb8\xba";                                        // 你成为

static bool find(const Lines& lines, const std::string& quote, int& first, int& last,
                 bool* relaxed = nullptr) {
  return quote_anchor::findLineRange(lines, quote, first, last, relaxed);
}

// A quote spanning two lines still resolves, on the exact pass.
static void testExactMultiLine() {
  const Lines lines{std::string(kHelp), std::string(kYou) + "\xe4\xb8\x80\xe5\x90\x8d", "tail"};
  int first = -1;
  int last = -1;
  bool relaxed = true;
  CHECK(find(lines, std::string(kHelp) + " " + kYou, first, last, &relaxed));
  CHECK(!relaxed);
  CHECK(first == 0 && last == 2);
}

// Toggle merge ON: the two lines became one, with no space between them.
static void testRelaxedAfterMergeOn() {
  const Lines lines{std::string(kHelp) + kYou + "\xe4\xb8\x80\xe5\x90\x8d", "tail"};
  int first = -1;
  int last = -1;
  bool relaxed = false;
  CHECK(find(lines, std::string(kHelp) + " " + kYou, first, last, &relaxed));
  CHECK(relaxed);
  CHECK(first == 0 && last == 1);
}

// Toggle merge OFF: the quote was taken from a joined paragraph, the page is split
// again.  Same asymmetry in the other direction.
static void testRelaxedAfterMergeOff() {
  const Lines lines{std::string(kHelp), std::string(kYou), "rest of the page"};
  int first = -1;
  int last = -1;
  bool relaxed = false;
  CHECK(find(lines, std::string(kHelp) + kYou, first, last, &relaxed));
  CHECK(relaxed);
  CHECK(first == 0 && last == 2);
}

// Interior spaces inside one line must not break the exact pass either.
static void testExactWithInteriorSpacing() {
  const Lines lines{"alpha  beta   gamma", "delta"};
  int first = -1;
  int last = -1;
  bool relaxed = true;
  CHECK(find(lines, "alpha beta gamma", first, last, &relaxed));
  CHECK(!relaxed);
  CHECK(first == 0 && last == 1);
}

// Ordering matters: when an exact match exists it wins, so Latin word boundaries keep
// their precision and the loosened pass cannot quietly relocate a highlight that is
// already placed correctly.
static void testExactIsPreferredOverDense() {
  // "the cat" appears exactly on line 1, and the de-spaced string would also match
  // across "them cat"-style noise on line 0.
  const Lines lines{"xthecaty", "the cat sat"};
  int first = -1;
  int last = -1;
  bool relaxed = true;
  CHECK(find(lines, "the cat", first, last, &relaxed));
  CHECK(!relaxed);
  CHECK(first == 1 && last == 2);
}

static void testNotFoundAndDegenerateInput() {
  const Lines lines{std::string(kHelp), "second line here"};
  int first = 0;
  int last = 0;
  CHECK(!find(lines, "not present at all", first, last));
  CHECK(!find(lines, "", first, last));
  CHECK(!find(lines, "   \t ", first, last));
  CHECK(!find(Lines{}, std::string(kHelp), first, last));
  CHECK(!find(Lines{"", "  "}, std::string(kHelp), first, last));
}

// A quote ending exactly at a line boundary must not extend into the next line.
static void testRangeEndsOnTheQuoteNotThePage() {
  const Lines lines{"alpha beta", "gamma delta"};
  int first = -1;
  int last = -1;
  bool relaxed = true;
  CHECK(find(lines, "alpha beta", first, last, &relaxed));
  CHECK(!relaxed);
  CHECK(first == 0 && last == 1);
}

// A bare substring search anchors a Latin quote onto a DIFFERENT word's line, and a
// matched quote underlines that whole line -- a highlight on text nobody selected.
static void testLatinSubstringInsideAWordIsNotAMatch() {
  const Lines exact{"band extraordinary results"};
  int first = -1;
  int last = -1;
  bool relaxed = false;
  CHECK(!find(exact, "and extra", first, last, &relaxed));

  // Same trap on the whitespace-insensitive pass, where nothing separates the letters
  // at all: dense page "bandextraordinary", dense needle "andextra".
  const Lines dense{"band", "extraordinary results"};
  CHECK(!find(dense, "and extra", first, last, &relaxed));
}

static void testLatinWordBoundaryStillMatches() {
  const Lines lines{"we discussed and extra terms here"};
  int first = -1;
  int last = -1;
  bool relaxed = true;
  CHECK(find(lines, "and extra", first, last, &relaxed));
  CHECK(!relaxed);
  CHECK(first == 0 && last == 1);
}

// CJK has no separators, so a quote may legitimately begin and end inside an
// unbroken run.  Applying the Latin guard there would kill every merged-page re-anchor.
static void testCjkMidRunIsNotWordBoundaryGuarded() {
  const Lines lines{std::string(kHelp) + kYou + "\xe4\xb8\x80\xe5\x90\x8d"};
  int first = -1;
  int last = -1;
  bool relaxed = false;
  CHECK(find(lines, std::string(kYou) + "\xe4\xb8\x80\xe5\x90\x8d", first, last, &relaxed));
}

// When both a mid-word and a real occurrence exist, pick the real one -- scanning must
// continue past a rejected hit rather than giving up on the page.
static void testPicksTheBoundaryValidOccurrence() {
  const Lines lines{"band extra ordinary", "then and extra terms followed"};
  int first = -1;
  int last = -1;
  bool relaxed = true;
  CHECK(find(lines, "and extra", first, last, &relaxed));
  CHECK(!relaxed);
  CHECK(first == 1 && last == 2);
}

// The mirror of the left-side trap: the quote is a PREFIX of a longer word on the
// page. "and extra" is contained in "and extraordinary", so without the right-hand
// boundary check the reader underlines a line about something else entirely.
static void testLatinPrefixOfALongerWordIsNotAMatch() {
  const Lines lines{"and extraordinary results followed here"};
  int first = -1;
  int last = -1;
  bool relaxed = false;
  CHECK(!find(lines, "and extra", first, last, &relaxed));
}

// --- per-line span resolution (EPUB-parity underline precision) -----------------

static int spanOn(const Lines& lines, const std::string& quote, const int line, int& s, int& e) {
  return quote_anchor::spanForLine(lines, quote, line, s, e);
}

// A one-line quote underlines exactly its own bytes, not the sentence around it.
static void testSingleLineSpanIsExact() {
  const Lines lines{"the quick brown fox jumps"};
  int s = -1, e = -1;
  CHECK(spanOn(lines, "quick brown", 0, s, e) == quote_anchor::kHere);
  CHECK(s == 4 && e == 15);
}

// A quote crossing lines must not paint the boundary lines end to end: the first line
// starts where the quote starts, the last ends where the quote ends.
static void testMultiLineBoundarySpans() {
  const Lines lines{"alpha beta gamma", "delta epsilon zeta", "eta theta"};
  int s = -1, e = -1;
  CHECK(spanOn(lines, "beta gamma delta epsilon", 0, s, e) == quote_anchor::kHere);
  CHECK(s == 6);                       // starts at "beta", not at column 0
  CHECK(e == 16);  // "alpha beta gamma".size() -- the quote runs to the end of line 0
  CHECK(spanOn(lines, "beta gamma delta epsilon", 1, s, e) == quote_anchor::kHere);
  CHECK(s == 0 && e == 13);            // stops after "epsilon", before " zeta"
}

// Interior lines of a three-line quote are covered in full -- that is what the quote says.
static void testInteriorLineIsFullWidth() {
  const Lines lines{"one two", "three four", "five six"};
  int s = -1, e = -1;
  CHECK(spanOn(lines, "two three four five", 1, s, e) == quote_anchor::kHere);
  CHECK(s == 0 && e == 10);
}

// The distinction the caller depends on: absent may fall back to stored offsets,
// elsewhere must not, or a stale offset range lands on a re-laid-out page.
static void testAbsentAndElsewhereAreDistinguished() {
  const Lines lines{"first line", "second line"};
  int s = -1, e = -1;
  CHECK(spanOn(lines, "nowhere at all", 0, s, e) == quote_anchor::kAbsent);
  CHECK(spanOn(lines, "first line", 1, s, e) == quote_anchor::kElsewhere);
  CHECK(spanOn(lines, "second line", 7, s, e) == quote_anchor::kAbsent);  // bad line index
}

// CJK: columns are byte offsets, so a mid-run quote on a merged line resolves to the
// right bytes even though there is no word boundary to line up with.
static void testCjkSpanUsesByteOffsets() {
  const std::string merged = std::string(kHelp) + kYou + "\xe4\xb8\x80\xe5\x90\x8d";
  const Lines lines{merged};
  int s = -1, e = -1;
  CHECK(spanOn(lines, std::string(kYou) + "\xe4\xb8\x80\xe5\x90\x8d", 0, s, e) ==
        quote_anchor::kHere);
  CHECK(s == static_cast<int>(std::string(kHelp).size()));
  CHECK(e == static_cast<int>(merged.size()));
}

// ---------------------------------------------------------------------------
// A repeated sentence must anchor to the copy the reader marked.
//
// `startOffset` for a PDF annotation is the LINE the selection began on, and it is the only
// information that distinguishes the second "他笑了" on a page from the first. Taking the
// first whole-word match silently underlines a different passage than the one that was
// selected -- the failure the quote was stored to prevent, reintroduced in a subtler form.
// ---------------------------------------------------------------------------
// Copies of 他笑了 sit on lines 1, 4 and 6, so a hint can be tested as strictly nearer
// (3 -> line 4), as a tie (5 -> lines 4 and 6 are both one apart) and as absent (-1).
static const std::vector<std::string> repeatedPage() {
  return {"前言部分在这一页的最上面写着。", "他笑了，然后转身离开这个地方。", "下面接着写了很长的一段其他内容文字。",
          "又过了几行之后事情已经有了变化。", "他笑了，好像什么都没有发生过一样。", "中间还有一段与上下文无关的文字内容。",
          "他笑了，这一页第三次写下同样的一句话。"};
}

static void testHintSelectsTheNearestOccurrence() {
  const std::vector<std::string> lines = repeatedPage();
  const std::string quote = "他笑了";

  // No hint: the first copy, exactly as before this rule existed.
  int first = -1;
  int last = -1;
  CHECK(quote_anchor::findLineRange(lines, quote, first, last));
  CHECK(first == 1 && last == 2);

  // Hint at the second copy: that is the one that must resolve.
  CHECK(quote_anchor::findLineRange(lines, quote, first, last, nullptr, 4));
  CHECK(first == 4 && last == 5);

  // Hint strictly nearer the third copy than the second: line 6 wins over line 4.
  CHECK(quote_anchor::findLineRange(lines, quote, first, last, nullptr, 6));
  CHECK(first == 6 && last == 7);

  // Hint exactly between two copies (5 is one line from both 4 and 6): the earlier one wins,
  // so which line a reader sees underlined does not depend on iteration order.
  CHECK(quote_anchor::findLineRange(lines, quote, first, last, nullptr, 5));
  CHECK(first == 4 && last == 5);

  // spanForLine draws and hit-tests from the same resolution, so it must agree: with the
  // hint on the second copy, the FIRST copy's line is kElsewhere (do not underline it).
  int startByte = -1;
  int endByte = -1;
  CHECK(quote_anchor::spanForLine(lines, quote, 4, startByte, endByte, 4) == quote_anchor::kHere);
  // Read the span BEFORE the next call: spanForLine clears its outputs on entry, which is
  // deliberate -- a caller that underlined from stale columns would paint a line the quote
  // never covered.
  CHECK(startByte == 0 && endByte == static_cast<int>(std::string("他笑了").size()));
  CHECK(quote_anchor::spanForLine(lines, quote, 6, startByte, endByte, 4) == quote_anchor::kElsewhere);
  CHECK(startByte == -1 && endByte == -1);
  CHECK(quote_anchor::spanForLine(lines, quote, 1, startByte, endByte, 4) == quote_anchor::kElsewhere);
  CHECK(quote_anchor::spanForLine(lines, quote, 1, startByte, endByte, 1) == quote_anchor::kHere);
  CHECK(startByte == 0 && endByte == static_cast<int>(std::string("他笑了").size()));
}

static void testExactPassStillBeatsTheHintedRelaxedOne() {
  // The line merge changed the spacing under an existing annotation, and the exact pass can
  // still find ONE copy exactly. A whitespace-free match further from the hint must not beat
  // it -- "relaxed never beats exact" is pinned independently of the hint.
  const std::vector<std::string> lines = {"开头的一行文字内容足够长以避开短行规则。",
                                          "第二段 文字 里 有 空格的 版本 在这里",
                                          "第三段文字里有空格的版本在这里出现了两次",
                                          "结尾的一行文字内容也足够长以避免误判。"};
  const std::string quote = "第三段文字里有空格的版本在这里出现了两次";
  int first = -1;
  int last = -1;
  bool relaxed = true;
  CHECK(quote_anchor::findLineRange(lines, quote, first, last, &relaxed, 1));
  CHECK(first == 2 && last == 3);
  CHECK(!relaxed);
}


int main() {
  testExactMultiLine();
  testRelaxedAfterMergeOn();
  testRelaxedAfterMergeOff();
  testExactWithInteriorSpacing();
  testExactIsPreferredOverDense();
  testNotFoundAndDegenerateInput();
  testRangeEndsOnTheQuoteNotThePage();
  testLatinSubstringInsideAWordIsNotAMatch();
  testLatinWordBoundaryStillMatches();
  testCjkMidRunIsNotWordBoundaryGuarded();
  testPicksTheBoundaryValidOccurrence();
  testLatinPrefixOfALongerWordIsNotAMatch();
  testSingleLineSpanIsExact();
  testMultiLineBoundarySpans();
  testInteriorLineIsFullWidth();
  testAbsentAndElsewhereAreDistinguished();
  testCjkSpanUsesByteOffsets();
  testHintSelectsTheNearestOccurrence();
  testExactPassStillBeatsTheHintedRelaxedOne();

  std::printf("quote anchor: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
