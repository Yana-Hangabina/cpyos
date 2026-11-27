// Host test for the PDF line-merge heuristic (src/pdf/PdfLineMerge.h). Pure:
// no renderer, no device. Pins every rule -- CJK rejoin, Latin rejoin,
// de-hyphenation, sentence-terminal stop, short-line stop, indent start,
// blank collapse -- and the run script mutates the header to prove they bite.
#include "pdf/PdfLineMerge.h"

#include <cstdio>
#include <string>
#include <vector>

static int checks = 0;
static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    ++checks;                                                    \
    if (!(cond)) {                                               \
      ++failures;                                                \
      printf("FAIL %d: %s\n", __LINE__, #cond);                 \
    }                                                            \
  } while (0)

using Strs = std::vector<std::string>;

static Strs m(const Strs& in) { return pdf_merge::merge(in, pdf_merge::Options{}); }

// De-hyphenation is OFF in the shipped default (see Options::deHyphenate), so the
// tests that exist to pin hyphen behaviour must ask for it explicitly rather than
// silently depending on what the struct happened to default to.
static Strs mHy(const Strs& in) {
  pdf_merge::Options on;
  on.deHyphenate = true;
  return pdf_merge::merge(in, on);
}

static void testCjkRejoin() {
  Strs in;
  in.push_back("本书如何帮助");
  in.push_back("你成为团队的英雄");
  Strs out = m(in);
  CHECK(out.size() == 1);
  CHECK(out.size() == 1 && out[0] == "本书如何帮助你成为团队的英雄");
}

static void testLatinRejoin() {
  Strs in;
  in.push_back("the quick brown");
  in.push_back("fox jumps");
  Strs out = m(in);
  CHECK(out.size() == 1);
  CHECK(out.size() == 1 && out[0] == "the quick brown fox jumps");
}

static void testDeHyphenate() {
  Strs in;
  in.push_back("counterpro-");
  in.push_back("ductive reasoning");
  Strs out = mHy(in);
  CHECK(out.size() == 1);
  CHECK(out.size() == 1 && out[0] == "counterproductive reasoning");
}

static void testTerminalStopsMerge() {
  Strs in;
  in.push_back("第一章结束。");
  in.push_back("新的段落开始");
  Strs out = m(in);
  CHECK(out.size() == 2);
}

static void testTerminalWithCloser() {
  Strs in;
  in.push_back("他说“你好。”");
  in.push_back("然后离开了");
  CHECK(m(in).size() == 2);
  Strs in2;
  in2.push_back("Stop!");
  in2.push_back("go");
  CHECK(m(in2).size() == 2);
}

static void testShortLineStopsMerge() {
  Strs in;
  in.push_back("前言");
  in.push_back("这是一段很长的正文内容用来验证不会被误判");
  CHECK(m(in).size() == 2);
}

static void testIndentedStartNewParagraph() {
  Strs in;
  in.push_back("上一段正文内容");
  in.push_back("  缩进的新段落");
  CHECK(m(in).size() == 2);
}

static void testBlankCollapse() {
  Strs in;
  in.push_back("第一段正文内容啊");
  in.push_back("");
  in.push_back("   ");
  in.push_back("第二段正文内容啊");
  Strs out = m(in);
  CHECK(out.size() == 2);
  for (const auto& s : out) CHECK(!s.empty());
}

static void testNoMergeAcrossTerminalChain() {
  Strs in;
  in.push_back("a.");
  in.push_back("b.");
  in.push_back("c.");
  CHECK(m(in).size() == 3);
}

static void testWellFormedUnchanged() {
  Strs in;
  in.push_back("这是完整的一句。");
  in.push_back("这是另一句完整的话。");
  CHECK(m(in) == in);
}


// --- a hard line break inside ONE block (MuPDF's stext shape) ---------------
// extractText() only splits on a blank line, so "line1\nline2" arrives as a
// single entry. merge() must still see it as two lines.

static void testHardBreakInsideOneBlock() {
  Strs in;
  in.push_back("\xe6\x9c\xac\xe4\xb9\xa6\xe5\xa6\x82\xe4\xbd\x95\xe5\xb8\xae\xe5\x8a\xa9\n"
               "\xe4\xbd\xa0\xe6\x88\x90\xe4\xb8\xba\xe4\xb8\x80\xe5\x90\x8d\xe5\xb7\xa5\xe7\xa8\x8b\xe5\xb8\x88");
  const Strs out = m(in);
  CHECK(out.size() == 1);
  if (!out.empty()) {
    CHECK(out[0].find('\n') == std::string::npos);
    CHECK(out[0] ==
          "\xe6\x9c\xac\xe4\xb9\xa6\xe5\xa6\x82\xe4\xbd\x95\xe5\xb8\xae\xe5\x8a\xa9"
          "\xe4\xbd\xa0\xe6\x88\x90\xe4\xb8\xba\xe4\xb8\x80\xe5\x90\x8d\xe5\xb7\xa5\xe7\xa8\x8b\xe5\xb8\x88");
  }
}

static void testHardBreakDeHyphenate() {
  Strs in;
  in.push_back("counterpro-\nductive methods that everyone can read");
  const Strs out = mHy(in);
  CHECK(out.size() == 1);
  if (!out.empty()) CHECK(out[0] == "counterproductive methods that everyone can read");
}

static void testBlankLineInsideBlockStaysBreak() {
  Strs in;
  in.push_back("alpha paragraph one continues here\n\nbeta paragraph two follows");
  const Strs out = m(in);
  CHECK(out.size() == 2);
  for (const auto& s : out) CHECK(s.find('\n') == std::string::npos);
}

static void testTrailingNewlineAddsNoPhantomBreak() {
  Strs in;
  in.push_back("first paragraph text here\n");
  in.push_back("second line follows it along");
  const Strs out = m(in);
  CHECK(out.size() == 1);  // the trailing newline is not a paragraph break
}

static void testIdeographicIndentStartsNewParagraph() {
  Strs in;
  in.push_back("\xe8\xbf\x99\xe6\x98\xaf\xe7\xac\xac\xe4\xb8\x80\xe6\xae\xb5\xe6\x96\x87\xe5\xad\x97"
               "\n\xe3\x80\x80\xe3\x80\x80\xe8\xbf\x99\xe6\x98\xaf\xe7\xac\xac\xe4\xba\x8c\xe6\xae\xb5\xe6\x96\x87\xe5\xad\x97");
  const Strs out = m(in);
  CHECK(out.size() == 2);
  // The indent marker decides the break; reflow draws the indent itself, so it
  // must not survive into the paragraph text (it would double).
  CHECK(out.size() == 2 && out[1].rfind("\xe3\x80\x80", 0) != 0);
}

// OCR'd Chinese fills the gaps between passages with characters that are INVISIBLE on
// paper but not empty as bytes: U+3000 ideographic space (the classic CJK indent), NBSP,
// U+200B. Byte-level "is this blank?" answers false, the paragraph gets laid out, and the
// reader sees a run of blank lines -- the exact symptom that motivated this option.
static void testInvisibleFillCountsAsBlank() {
  CHECK(utf8IsBlankText(std::string()));
  CHECK(utf8IsBlankText(" "));
  CHECK(utf8IsBlankText("\t \r\n"));
  CHECK(utf8IsBlankText("\xe3\x80\x80"));                    // U+3000
  CHECK(utf8IsBlankText("\xc2\xa0\xc2\xa0"));                    // NBSP x2
  CHECK(utf8IsBlankText("\xe2\x80\x8b\xe3\x80\x80 "));          // ZWSP + ideographic space
  CHECK(!utf8IsBlankText("\xe3\x80\x80字"));                     // one real glyph ends the blankness
  CHECK(!utf8IsBlankText("\xff"));                              // malformed bytes are content

  // A run of them collapses to ONE paragraph break, and no blank entry survives to be
  // drawn as a line.
  Strs in;
  in.push_back("\xe7\xac\xac\xe4\xb8\x80\xe6\xae\xb5\xe5\x86\x85\xe5\xae\xb9\xe8\xbe\x83\xe9\x95\xbf\xe4\xb8\x80\xe7\x82\xb9");
  in.push_back("\xe3\x80\x80");
  in.push_back("\xc2\xa0");
  in.push_back(" ");
  in.push_back("\xe7\ac\xac\xe4\xba\x8c\xe6\xb5\xb7\xe5\x86\x85\xe5\xae\xb9\xe4\xb9\x9f\xe8\xbe\x83\xe9\x95\xbf\xe4\xb8\x80\xe7\x82\xb9");
  const Strs out = m(in);
  CHECK(out.size() == 2);
  for (const auto& s : out) CHECK(!utf8IsBlankText(s));
}

static void testNeverEmitsNewlines() {
  Strs in;
  in.push_back("one\ntwo\nthree four five six seven eight");
  in.push_back("a\n\nb");
  in.push_back("trailing\n");
  const Strs out = m(in);
  for (const auto& s : out) CHECK(!s.empty() && s.find('\n') == std::string::npos);
}


// Measured on device (2026-11-19, /pdf_english.pdf with the option ON): a line
// that ends with a hyphenated COMPOUND is glued into a non-word, exactly as a
// line-final soft hyphen is repaired. The selection quote came back
// "identifiers, andextraordinarily complicated". This is the accepted cost of a
// dictionary-free heuristic -- calibre gates de-hyphenation on a word list, and
// a word list is Flash this build does not have. Pinned here so that changing
// it stays a decision, not an accident: if you add a gate, THIS check must be
// the one that flips.
static void testCompoundHyphenGluesByDesign() {
  Strs in;
  in.push_back("long identifiers, and-");
  in.push_back("extraordinarily complicated text");
  const Strs out = mHy(in);
  CHECK(out.size() == 1);
  if (!out.empty()) CHECK(out[0] == "long identifiers, andextraordinarily complicated text");

  // What ships: de-hyphenation off keeps the hyphen AND the words separate, so the
  // reader sees an odd `and- extraordinarily` rather than a fabricated word.
  const Strs kept = m(in);
  CHECK(kept.size() == 1);
  if (!kept.empty()) CHECK(kept[0] == "long identifiers, and- extraordinarily complicated text");
}

// The short-line stop must be relative to the page's median line length
// (calibre's --html-unwrap-factor).  A short line is the ragged right edge of
// the paragraph it ENDS, so it belongs to that paragraph and the line AFTER it
// opens a new one -- the break is only knowable after reading it.
static void testMedianRelativeBreak() {
  const std::string a = "这是一段很长的中文文字用来填满一整行的宽度甲";
  const std::string b = "这是另一段很长的中文文字用来填满整行宽度乙";
  const std::string tail = "本段收尾";   // short against the page median, not against three
  const std::string c = "这是下一段的开头也是很长的一行内容丙";
  const std::string d = "这是下一段的中间部分同样很长的一行丁";
  CHECK(pdf_merge::codepoints(tail) > pdf_merge::kAbsoluteShortCp);

  const Strs in{a, b, tail, c, d};
  const Strs out = m(in);
  CHECK(out.size() == 2);
  if (out.size() == 2) {
    CHECK(out[0] == a + b + tail);
    CHECK(out[1] == c + d);
  }

  // Without the median rule the tail line is no signal at all and the whole
  // page collapses into one paragraph: that is the old behaviour, pinned so
  // dropping the rule stays a decision rather than a silent regression.
  pdf_merge::Options absolute;
  absolute.unwrapPercent = 0;
  const Strs flat = pdf_merge::merge(in, absolute);
  CHECK(flat.size() == 1);
  if (!flat.empty()) CHECK(flat[0] == a + b + tail + c + d);
}

// The median, not the mean: long outlier lines must not drag the threshold up
// so far that a genuine paragraph end stops being detected.
static void testMedianIgnoresOutliers() {
  const std::string wide(60, 'x');
  std::vector<std::string> in{wide, wide};
  for (int i = 0; i < 6; ++i) in.emplace_back(20, 'y');
  const std::string tail(7, 'z');
  in.push_back(tail);
  const std::string next(20, 'w');
  in.push_back(next);
  const pdf_merge::PageShape shape = pdf_merge::measurePage(in);
  CHECK(shape.median == 20);
  // Prose rows carry neither a leader nor a number tail, so the table guard below
  // must not fire on a page like this one.
  CHECK(shape.entryRows == 0);
  CHECK(!pdf_merge::pageIsAnEntryList(shape));
  const Strs out = m(in);
  // The tail belongs to the paragraph it ends; `next` must not join it.
  bool endsWithTail = false;
  bool nextAlone = false;
  for (const auto& para : out) {
    if (para.size() >= tail.size() && para.compare(para.size() - tail.size(), tail.size(), tail) == 0) endsWithTail = true;
    if (para == next) nextAlone = true;
  }
  CHECK(endsWithTail);
  CHECK(nextAlone);
}

// A hyphen-final line is a continuation however short it is. Under the median
// rule a wrapped "counterpro-" would read as the end of its paragraph and the
// de-hyphenation would never see its second half -- the two rules have to be
// aware of each other.
static void testHyphenFinalLineIsNeverABreak() {
  const std::string long1(40, 'x');
  const std::string long2(40, 'x');
  const Strs in{long1, std::string("counterpro-"), std::string("ductive methods that everyone can read")};
  const Strs out = mHy(in);
  CHECK(out.size() == 1);
  if (!out.empty()) {
    CHECK(out[0].find("counterproductive") != std::string::npos);
    CHECK(out[0].find("counterpro- ductive") == std::string::npos);
  }

  // Same signal at the shipped default (de-hyphenation off): still not a break.
  const Strs kept = m(in);
  CHECK(kept.size() == 1);
}

// The exact eight text objects scripts/make_pdf_hardwrap_test.py writes into
// the fixture PDF.  The device asserts `reflow merge: 8 blocks -> 4 paragraphs`
// for that page, so the same input has to produce the same split here --
// otherwise the on-device check is resting on a prose claim instead of on the
// bytes the extractor will actually deliver.
static void testFixturePageShape() {
  const Strs in{
      "The reflow engine must join these three",
      "counterpro-",
      "ductive lines into a single paragraph.",
      " ",
      "A short heading",
      "   An indented line starts its own paragraph.",
      "It continues here because the previous line",
      "did not end a sentence at all"};
  const Strs out = m(in);
  CHECK(out.size() == 4);
  // The device logs `reflow merge: 8 blocks -> 4 paragraphs`; that count must not
  // depend on the de-hyphenation flag, or the on-device assertion is a coincidence.
  CHECK(mHy(in).size() == 4);
  if (out.size() == 4) {
    CHECK(out[0] == "The reflow engine must join these three counterpro- ductive lines into a single paragraph.");
    CHECK(out[1] == "A short heading");
    CHECK(out[2] == "An indented line starts its own paragraph.");
    CHECK(out[3] == "It continues here because the previous line did not end a sentence at all");
  }
}

// --- real extracted text, not hand-written strings -------------------------------
// Everything below came out of a shipped PDF (thirdparty/lcms2/doc/LittleCMS2.16
// API.pdf, 177 pages), extracted with the host's PyMuPDF 1.28.0 (MuPDF 1.29.0) via
// get_text("text") for the per-line shape and get_text("blocks") for the block shape.
// Two honest caveats, because the temptation to overstate this is exactly how the
// 1.26.3 claim got written here first:
//   * the device module builds MuPDF **1.26.3**, so this is not the engine's own
//     output -- it is a proxy for the *shape* of real text (leader length, page-number
//     tails, hyphen-free wraps). `make_real_fixture.py` prints the version it used, and
//     regenerating here reproduces these arrays byte-for-byte;
//   * the device's `extractText()` joins lines inside one block module-side, so the
//     per-line array is the OCR'd worst case rather than a byte-exact device dump. The
//     device-side measurement is `exp/pdf_verify_matrix.py --sweep N`.
// A hand-written fixture can only contain the shapes I thought of; this is a real table
// of contents, with real long dot leaders and real page-number tails. Only section
// titles and numbers, no body prose.
static const char* kRealTocRows[] = {
      "3",
      "Colorspace conversions ................................................................................................... 60",
      "Encoding /Decoding on PCS ............................................................................................ 62",
      "Accessing tags...................................................................................................................... 64",
      "Tag types........................................................................................................................... 64",
      "Tags .................................................................................................................................. 65",
      "Accessing tags as raw data .............................................................................................. 71",
      "Profile structures ................................................................................................................... 73",
      "Platforms ........................................................................................................................... 74",
      "Reference gamut .............................................................................................................. 74",
      "Image State ....................................................................................................................... 74",
      "Pipeline Stages (Multi processing elements) ................................................................... 75",
      "Formatters............................................................................................................................. 77",
      "Macros to build formatters............................................................................................. 77",
      "Macros to extract information from formatters .............................................................. 77",
      "Color spaces in Little CMS notation ................................................................................. 78",
      "Translate color space from/to Little CMS notation to ICC................................................ 79",
      "Predefined formatters ....................................................................................................... 80",
      "Alpha channel ................................................................................................................... 84",
      "Other tables....................................................................................................................... 86",
      "Intents ................................................................................................................................... 88",
      "Flags ..................................................................................................................................... 91",
      "Color transforms ................................................................................................................... 92",
      "Proofing transforms........................................................................................................... 97",
      "Multiprofile transforms..................................................................................................... 101",
      "Dynamically changing the input/output formats ................................................................. 103",
      "PostScript generation ......................................................................................................... 105",
      "Δ E metrics.......................................................................................................................... 107",
      "Temperature <-> Chromaticity (Black body) ...................................................................... 110",
      "CIE CAM02 ......................................................................................................................... 111",
      "Gamut boundary description .............................................................................................. 113",
      "Gamut mapping .................................................................................................................. 115",
      "MD5 message digest .......................................................................................................... 116",
      "CGATS.17-200x handling................................................................................................... 118",
};

// A real page of body text, one entry per visual line: the shape an OCR'd book
// produces, and the shape this option exists to repair.
static const char* kRealProseLines[] = {
      "21",
      "Error logging",
      "2.0",
      "void cmsSetLogErrorHandler(cmsLogErrorHandlerFunction Fn);",
      "Allows user to set any specific logger on global context. Each time this function is called, the",
      "previous logger is replaced. Calling this functin with NULL as parameter, does reset the",
      "logger to the default Little CMS logger. The default Little CMS logger does nothing.",
      "Parameters:",
      "Fn: Callback to the logger (user defined function), or NULL to reset Little CMS to its",
      "default logger.",
      "Returns:",
      "*None*",
      "2.6",
      "void cmsSetLogErrorHandlerTHR(cmsContext ContextID,",
      "cmsLogErrorHandlerFunction Fn);",
      "Allows user to set any specific logger for the given context. Each time this function is called,",
      "the previous logger is replaced. Calling this functin with NULL as parameter, does reset the",
      "logger to the default Little CMS logger. The default Little CMS logger does nothing.",
      "Parameters:",
      "ContextID:   Handle to user-defined context, or NULL for the global context",
      "Fn: Callback to the logger (user defined function), or NULL to reset Little CMS to its",
      "default logger.",
      "Returns:",
      "*None*",
};

static void testRealTocPageIsNotMerged() {
  const Strs toc(kRealTocRows, kRealTocRows + sizeof(kRealTocRows) / sizeof(*kRealTocRows));
  const auto out = m(toc);
  // Measured on this fixture: 34 -> 34 with the guards, 34 -> 2 with them removed (the
  // whole contents page as two paragraphs).  That is the reported symptom inverted, and
  // it is what the default-on option was doing.
  CHECK(out.size() == toc.size());
  for (size_t i = 0; i < out.size() && i < toc.size(); ++i) CHECK(out[i] == toc[i]);
  const pdf_merge::PageShape shape = pdf_merge::measurePage(toc);
  CHECK(shape.lines == toc.size());
  CHECK(shape.entryRows * 2 >= shape.lines);
  CHECK(pdf_merge::pageIsAnEntryList(shape));
}

static void testRealProsePageStillMerges() {
  const Strs lines(kRealProseLines, kRealProseLines + sizeof(kRealProseLines) / sizeof(*kRealProseLines));
  const auto out = m(lines);
  CHECK(out.size() < lines.size());   // 24 -> 15 measured: a real wrapped page still merges
}

static void testRealBlockShapeMergesExceptNumberedHeads() {
  const Strs blocks = {
      "21  Error logging",
      "2.0    void cmsSetLogErrorHandler(cmsLogErrorHandlerFunction Fn);      Allows user to set any specific logger on global context. Each time this function is called, the  previous logger is replaced. Calling this functin with NULL as parameter, does reset the  logger to the default Little CMS logger. The default Little CMS logger does nothing.",
      "Parameters:",
      "Fn: Callback to the logger (user defined function), or NULL to reset Little CMS to its  default logger.",
      "Returns:",
      "*None*",
      "2.6    void cmsSetLogErrorHandlerTHR(cmsContext ContextID,                                                                     cmsLogErrorHandlerFunction Fn);      Allows user to set any specific logger for the given context. Each time this function is called,  the previous logger is replaced. Calling this functin with NULL as parameter, does reset the  logger to the default Little CMS logger. The default Little CMS logger does nothing.",
      "Parameters:",
      "ContextID:   Handle to user-defined context, or NULL for the global context  Fn: Callback to the logger (user defined function), or NULL to reset Little CMS to its  default logger.    Returns:",
      "*None*",
  };
  const auto out = m(blocks);
  // 10 -> 6 measured.  The one merge the guards refuse here is the numbered heading
  // `2.6  void cmsSetLogErrorHandlerTHR(...`, which startsEnumerated() keeps off the
  // paragraph beneath it -- the rule doing its job, not paying for it.
  CHECK(out.size() == 6);
  CHECK(out[0] == blocks[0]);
}

// A table of contents is the case the median-relative rule gets backwards: every
// row is the same length, so nothing looks short and the whole page reads as one
// wrapped paragraph.  Measured before the guards existed: 6 rows -> 1 paragraph with
// "… 1 第二章" glued mid-sentence.  The option is ON by default, so this was the
// feature inventing the defect it was meant to repair.
static void testTableOfContentsSurvives() {
  const Strs toc = {
      "\347\254\254\344\270\200\347\253\240  \345\272\237\345\234\237\345\234\260\351\223\201\347\253\231 .................... 1",
      "\347\254\254\344\272\214\347\253\240  \345\215\210\345\244\234\344\276\277\345\210\251\345\272\227 .................... 7",
      "\347\254\254\344\270\211\347\253\240  \351\225\277\345\244\234\345\260\206\345\260\275 ........................ 15",
      "\347\254\254\345\233\233\347\253\240  \345\275\222\351\200\224 ............................ 23",
  };
  const Strs joined = {"\344\270\200", "\344\272\214", "\344\270\211", "\345\233\233"};
  (void)joined;
  const Strs ascii = {"Chapter One  .................... 1",
                      "Chapter Two  .................... 7",
                      "Chapter Three  ................ 15",
                      "Chapter Four  .................. 23"};
  const auto out = m(ascii);
  CHECK(out.size() == ascii.size());
  for (size_t k = 0; k < out.size() && k < ascii.size(); ++k) CHECK(out[k] == ascii[k]);
  // Same page, dot leaders removed but page numbers left: still a table.
  const Strs numbers = {"Chapter One  1", "Chapter Two  7", "Chapter Three  15", "Chapter Four  23"};
  CHECK(m(numbers).size() == numbers.size());
}

// A wrapped-prose page must not be mistaken for a table just because one line ends
// on a number.  The rule needs half the page to look like rows.
static void testProsePageWithOneNumberLineStillMerges() {
  const Strs prose = {"\346\234\254\344\271\246\345\246\202\344\275\225\345\270\256\345\212\251",
                      "\344\275\240\346\210\220\344\270\272\346\233\264\345\245\275\347\232\204",
                      "\350\257\273\350\200\205\343\200\202\345\205\250\344\271\246\345\205\261 12",
                      "\344\270\252\347\253\240\350\212\202\357\274\214\346\257\217\347\253\240\345\271\263",
                      "\345\235\207\344\270\211\347\231\276\345\255\227\345\267\246\345\217\263"};
  const auto out = m(prose);
  CHECK(out.size() < prose.size());
}

// Leaders, not numbers, are the weaker evidence: a single row of dots mid-page still
// stops THAT merge without disabling the rest of the page.
static void testLeaderRowStopsOnlyItsOwnMerge() {
  const Strs in = {"\346\231\250\345\211\215\346\230\216\346\234\210\345\205\211 ...",
                   "\347\226\221\346\230\257\345\234\260\344\270\212\351\234\234",
                   "\344\270\276\345\244\264\346\234\233\346\230\216\346\234\210",
                   "\344\275\216\345\244\264\346\200\235\346\225\205\344\271\241"};
  const auto out = m(in);
  CHECK(out.size() == 2);            // the dotted row stands alone
  CHECK(out[0] == in[0]);            // ... and nothing was appended to it
  // The remaining three are a poem: equal-length unpunctuated lines carry no text-only
  // signal that they are not a hard-wrapped column, so they merge. That is the
  // documented limit of the rule (a column of verse needs line geometry to see), and
  // this assertion exists so the limit stays a KNOWN behaviour rather than a surprise.
  CHECK(out[1] == "\347\226\221\346\230\257\345\234\260\344\270\212\351\234\234"
                  "\344\270\276\345\244\264\346\234\233\346\230\216\346\234\210"
                  "\344\275\216\345\244\264\346\200\235\346\225\205\344\271\241");
}

static void testEnumerationStartsANewParagraph() {
  const Strs in = {"sentinel", "x"};
  (void)in;
  const Strs rows = {"\345\272\237\345\234\260\351\223\201\347\253\231\347\232\204\344\272\213",
                     "\347\254\254\344\272\214\347\253\240 \345\215\210\345\244\234\344\276\277\345\210\251\345\272\227",
                     "\345\220\216\346\235\245\347\232\204\346\227\245\345\255\220\345\220\214\346\240\267"};
  const auto out = m(rows);
  CHECK(out.size() == 3);   // the enumerated row neither joins nor is joined
  const Strs numbered = {"\344\270\200\346\256\265\345\274\200\345\244\264\347\232\204\350\257\235",
                         "2. \347\254\254\344\272\214\346\235\241", "3. \347\254\254\344\270\211\346\235\241"};
  CHECK(m(numbered).size() == 3);
}

static void testMeasurePageCountsRowsAndBlanks() {
  const Strs in = {"Chapter One  1", "  ", "Chapter Two  7", "\u3000\u3000", "Chapter Three  15"};
  const pdf_merge::PageShape shape = pdf_merge::measurePage(in);
  CHECK(shape.lines == 3);            // the two invisible-fill lines are not lines
  CHECK(shape.entryRows == 3);
  CHECK(pdf_merge::pageIsAnEntryList(shape));
  // Two of five is not yet a table: a wrapped page may well end one line on a number.
  const Strs mixed = {"Chapter One  1", "body text here", "more body text", "still prose", "and prose"};
  const pdf_merge::PageShape two = pdf_merge::measurePage(mixed);
  CHECK(two.lines == 5);
  CHECK(two.entryRows == 1);
  CHECK(!pdf_merge::pageIsAnEntryList(two));
  // Exactly half is the boundary, and it takes three rows to say anything at all.
  const Strs half = {"Item A  1", "Item B  2", "prose line", "prose line"};
  CHECK(pdf_merge::pageIsAnEntryList(pdf_merge::measurePage(half)));
  const Strs twoRows = {"Item A  1", "Item B  2"};
  CHECK(!pdf_merge::pageIsAnEntryList(pdf_merge::measurePage(twoRows)));
}

static void testEntryRowPredicates() {
  using pdf_merge::hasLeaderRun;
  using pdf_merge::endsWithDigit;
  using pdf_merge::isEntryRow;
  using pdf_merge::startsEnumerated;
  CHECK(hasLeaderRun("Title .... 3"));
  CHECK(!hasLeaderRun("Title . 3"));
  CHECK(!hasLeaderRun("end of sentence."));
  CHECK(endsWithDigit("Chapter 12"));
  CHECK(endsWithDigit("Chapter 12  "));    // trailing spaces are not the end of the row
  CHECK(!endsWithDigit("12 chapters"));
  CHECK(isEntryRow("\347\254\254\344\270\200\347\253\240 1"));
  CHECK(startsEnumerated("\347\254\254\344\270\211\347\253\240 \345\275\222\351\200\224"));
  CHECK(startsEnumerated("12. \345\275\222\351\200\224"));
  CHECK(!startsEnumerated("\347\254\254\344\270\211\345\244\251\346\211\215\351\206\222\345\256\214"));  // 第 + 三天, no section word
  CHECK(!startsEnumerated("\344\270\213\351\235\242\346\210\221\344\273\254\345\274\200\345\247\213"));
}

int main() {
  testCjkRejoin();
  testLatinRejoin();
  testDeHyphenate();
  testTerminalStopsMerge();
  testTerminalWithCloser();
  testShortLineStopsMerge();
  testIndentedStartNewParagraph();
  testBlankCollapse();
  testNoMergeAcrossTerminalChain();
  testWellFormedUnchanged();
  testHardBreakInsideOneBlock();
  testHardBreakDeHyphenate();
  testBlankLineInsideBlockStaysBreak();
  testTrailingNewlineAddsNoPhantomBreak();
  testIdeographicIndentStartsNewParagraph();
  testMedianRelativeBreak();
  testMedianIgnoresOutliers();
  testHyphenFinalLineIsNeverABreak();
  testFixturePageShape();
  testInvisibleFillCountsAsBlank();
  testNeverEmitsNewlines();
  testCompoundHyphenGluesByDesign();
  testRealTocPageIsNotMerged();
  testRealProsePageStillMerges();
  testRealBlockShapeMergesExceptNumberedHeads();
  testTableOfContentsSurvives();
  testProsePageWithOneNumberLineStillMerges();
  testLeaderRowStopsOnlyItsOwnMerge();
  testEnumerationStartsANewParagraph();
  testMeasurePageCountsRowsAndBlanks();
  testEntryRowPredicates();
  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
