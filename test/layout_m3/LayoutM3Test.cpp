// Host test for layout milestone M3: CSS page breaks, orphan/widow control and
// heading keep-with-next.
//
// Build with run_layout_m3_test.sh (bare g++, no cmake/gtest). It compiles the real
// ChapterHtmlSlimParser/ParsedText/Page/TextBlock against the M2 renderer stub and
// checks two layers:
//   * lib/Epub/Epub/PageBreakPolicy.h on its own (the shipped decision logic);
//   * the parser end to end on a 480x80 viewport, i.e. exactly five 16 px lines per
//     page, so a page boundary is easy to reason about.
// run_layout_m3_test.sh also builds two mutants (a policy that never carries, and a
// parser that ignores the carry) and requires this binary to fail, so no check can
// pass vacuously.

#include <Epub/Page.h>
#include <Epub/PageBreakPolicy.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"

namespace {

int g_failures = 0;

void check(const bool condition, const char* what) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  ++g_failures;
}

using page_break_policy::Carry;
using page_break_policy::decide;
using page_break_policy::State;

const auto canAlways = [](const size_t) { return true; };

void testPolicy() {
  // Enough lines on the page and more to come: no break decision.
  check(decide(State{3, 4, 0}, canAlways).lines == 0, "policy: mid-paragraph never carries");
  // A lone first line at the page bottom is an orphan.
  {
    const Carry carry = decide(State{1, 4, 0}, canAlways);
    check(carry.lines == 1 && carry.paragraphLines == 1, "policy: orphan line is carried");
  }
  // The paragraph's last line would open the next page alone: a widow.
  {
    const Carry carry = decide(State{5, 0, 0}, canAlways);
    check(carry.lines == 1 && carry.paragraphLines == 1, "policy: widow carries one line");
  }
  // Only two lines here, so both move and no orphan is left behind.
  {
    const Carry carry = decide(State{2, 0, 0}, canAlways);
    check(carry.lines == 2 && carry.paragraphLines == 2, "policy: two-line widow moves both lines");
  }
  // An unknown remaining-line count never triggers the widow rule.
  check(decide(State{5, -1, 0}, canAlways).lines == 0, "policy: unknown tail is left alone");
  // A heading waits for the following block's first line.
  {
    const Carry carry = decide(State{0, 3, 2}, canAlways);
    check(carry.lines == 2 && carry.paragraphLines == 0, "policy: heading travels with the next block");
  }
  // ...and joins an orphan carry.
  {
    const Carry carry = decide(State{1, 2, 1}, canAlways);
    check(carry.lines == 2 && carry.paragraphLines == 1, "policy: heading joins an orphan carry");
  }
  // Heading + paragraph tail over the cap: only the paragraph lines move.
  {
    const Carry carry = decide(State{1, 3, 6}, canAlways);
    check(carry.lines == 1 && carry.paragraphLines == 1, "policy: oversized carry keeps the heading");
  }
  // Nothing may move when the page has too few trailing lines.
  const auto onlyBelowTwo = [](const size_t lines) { return lines < 2; };
  check(decide(State{2, 0, 0}, onlyBelowTwo).lines == 0, "policy: carry respects the page contents");
  // Arming the heading carry accumulates and clamps at the cap.
  check(page_break_policy::armKeepWithNext(true, 0, 2) == 2, "policy: heading arms its lines");
  check(page_break_policy::armKeepWithNext(true, 3, 5) == page_break_policy::kMaxCarriedLines,
        "policy: heading carry clamps");
  check(page_break_policy::armKeepWithNext(false, 3, 5) == 0, "policy: a body block disarms the carry");
}

struct Line {
  std::string text;
  uint16_t paragraphIndex = 0;
  bool paragraphFinal = false;
};

struct Fixture {
  std::string dir;
  GfxRenderer renderer;
  CssParser css{"/tmp"};
  std::vector<std::unique_ptr<Page>> pages;

  bool build(const std::string& html, const std::string& stylesheet) {
    std::error_code ec;
    dir = std::filesystem::temp_directory_path(ec) / "layout_m3_test";
    std::filesystem::create_directories(dir, ec);
    const std::string htmlPath = dir + "/chapter.xhtml";
    const std::string cssPath = dir + "/style.css";
    {
      std::ofstream out(htmlPath, std::ios::binary);
      out << html;
    }
    {
      std::ofstream out(cssPath, std::ios::binary);
      out << stylesheet;
    }
    HalFile cssFile;
    if (!cssFile.open(cssPath.c_str(), "rb")) return false;
    css.loadFromStream(cssFile);
    cssFile.close();

    ChapterHtmlSlimParser parser(
        nullptr, htmlPath, renderer, /*fontId=*/7, /*lineCompression=*/1.0f, /*extraParagraphSpacing=*/0,
        /*firstLineIndent=*/0, /*firstLineIndentPx=*/0, /*paragraphAlignment=*/static_cast<uint8_t>(CssTextAlign::Justify),
        /*verticalBottomAlign=*/false, /*viewportWidth=*/renderer.getScreenWidth(),
        /*viewportHeight=*/kPageHeight, /*hyphenation=*/false, /*focusReading=*/false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pages.push_back(std::move(page)); },
        /*embeddedStyle=*/true, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/0, {}, nullptr, &css,
        /*collectTouchLinks=*/true, /*blockFonts=*/nullptr);
    return parser.parseAndBuildPages();
  }

  // The stub's section face is 16 px, so five lines fit and a page boundary is one
  // line decision.
  static constexpr int kPageHeight = 5 * 16;

  [[nodiscard]] std::vector<std::string> pageTexts() const {
    std::vector<std::string> out;
    for (const auto& page : pages) {
      std::string text;
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto* line = static_cast<const PageLine*>(element.get());
        if (!text.empty()) text += ' ';
        text += line->getBlock()->wordCount() > 0 ? line->getBlock()->wordText(0) : "";
      }
      out.push_back(text);
    }
    return out;
  }

  [[nodiscard]] std::vector<std::vector<Line>> pageLines() const {
    std::vector<std::vector<Line>> out;
    for (const auto& page : pages) {
      std::vector<Line> lines;
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto* line = static_cast<const PageLine*>(element.get());
        const TextBlock& block = *line->getBlock();
        Line info;
        info.text = block.wordCount() > 0 ? block.wordText(0) : "";
        info.paragraphIndex = block.getParagraphIndex();
        info.paragraphFinal = block.isParagraphFinal();
        lines.push_back(info);
      }
      out.push_back(lines);
    }
    return out;
  }
};

// One word per line: 50 glyphs at the stub's 8 px advance never share a row.
std::string word(const char* prefix, const int index) {
  std::string text = prefix;
  text += std::to_string(index);
  text.append(50 - text.size(), 'x');
  return text;
}

std::string words(const char* prefix, const int count) {
  std::string html;
  for (int i = 0; i < count; ++i) {
    if (i > 0) html += ' ';
    html += word(prefix, i);
  }
  return html;
}

bool startsWith(const std::string& text, const std::string& prefix) { return text.rfind(prefix, 0) == 0; }

const char* const kStyle =
    "p{margin:0}h1{margin:0}.brk{page-break-before:page}.aft{page-break-after:page}";

void testOrphan() {
  Fixture fixture;
  check(fixture.build("<html><body><p>" + words("w", 4) + "</p><p>" + words("P", 3) + "</p></body></html>", kStyle),
        "orphan fixture parses");
  const auto pages = fixture.pageTexts();
  check(pages.size() == 2, "orphan fixture has two pages");
  if (pages.size() != 2) return;
  check(pages[0] == words("w", 4), "orphan: the paragraph starts the next page instead of one line here");
  check(pages[1] == words("P", 3), "orphan: all three lines follow on the next page");

  // The carried line keeps its paragraph identity: same index as its siblings,
  // still not the paragraph's final line.
  const auto lines = fixture.pageLines();
  const auto& moved = lines[1][0];
  check(moved.paragraphIndex == lines[1][1].paragraphIndex, "orphan: carried line keeps its paragraph index");
  check(moved.paragraphIndex != lines[0][0].paragraphIndex, "orphan: carried line does not inherit the previous block");
  check(!moved.paragraphFinal, "orphan: carried line is not renumbered to the paragraph's last");
  check(lines[1][2].paragraphFinal, "orphan: the paragraph's last line is still its final one");
}

void testWidow() {
  Fixture fixture;
  check(fixture.build("<html><body><p>" + words("Q", 6) + "</p></body></html>", kStyle), "widow fixture parses");
  const auto pages = fixture.pageTexts();
  check(pages.size() == 2, "widow fixture has two pages");
  if (pages.size() != 2) return;
  check(pages[0] == words("Q", 4), "widow: one line is taken along so the next page gets two");
  check(pages[1] == word("Q", 4) + " " + word("Q", 5), "widow: the next page opens with two lines");
}

void testKeepWithNext() {
  Fixture fixture;
  check(fixture.build("<html><body><p>" + words("w", 4) + "</p><h1>Heading</h1><p>" + words("R", 2) +
                          "</p></body></html>",
                      kStyle),
        "keep-with-next fixture parses");
  const auto pages = fixture.pageTexts();
  check(pages.size() == 2, "keep-with-next fixture has two pages");
  if (pages.size() != 2) return;
  check(pages[0] == words("w", 4), "keep-with-next: the heading does not stay behind alone");
  check(startsWith(pages[1], "Heading"), "keep-with-next: the heading opens the next page");
  check(startsWith(pages[1], "Heading " + word("R", 0)), "keep-with-next: the heading sits with the next block's first line");
}

void testPageBreakBefore() {
  const std::string html = "<html><body><p>" + words("w", 2) + "</p><p class=\"brk\">" + words("B", 2) +
                           "</p><p>" + words("z", 2) + "</p></body></html>";
  Fixture fixture;
  check(fixture.build(html, kStyle), "page-break-before fixture parses");
  const auto pages = fixture.pageTexts();
  check(pages.size() == 2, "page-break-before: two pages");
  if (pages.size() != 2) return;
  check(pages[0] == words("w", 2), "page-break-before: the break block starts a page");
  check(startsWith(pages[1], word("B", 0)), "page-break-before: the break block is first on the new page");
  check(pages[1] == words("B", 2) + " " + words("z", 2), "page-break-before: following text stays on the new page");
}

void testPageBreakBeforeFromContainer() {
  // The break is declared on the container; its first child block must honour it.
  const std::string html = "<html><body><p>" + words("w", 2) + "</p><div class=\"brk\"><p>" + words("N", 2) +
                           "</p></div></body></html>";
  Fixture fixture;
  check(fixture.build(html, kStyle), "container page-break fixture parses");
  const auto pages = fixture.pageTexts();
  check(pages.size() == 2, "container break: two pages");
  if (pages.size() != 2) return;
  check(pages[0] == words("w", 2), "container break: the container's first block starts a page");
  check(startsWith(pages[1], word("N", 0)), "container break: the child block is first on the new page");
}

void testPageBreakAfter() {
  const std::string html = "<html><body><p class=\"aft\">" + words("A", 2) + "</p><p>" + words("w", 2) +
                           "</p></body></html>";
  Fixture fixture;
  check(fixture.build(html, kStyle), "page-break-after fixture parses");
  const auto pages = fixture.pageTexts();
  check(pages.size() == 2, "page-break-after: two pages");
  if (pages.size() != 2) return;
  check(pages[0] == words("A", 2), "page-break-after: the block ends its page");
  check(pages[1] == words("w", 2), "page-break-after: the next block starts a new page");
}

void testNoBlankPages() {
  // A break on the first block, and one right after a page that is already full.
  const std::string html = "<html><body><p class=\"brk\">" + words("F", 1) + "</p><p class=\"brk\">" + words("w", 5) +
                           "</p><p class=\"brk\">" + words("G", 1) + "</p></body></html>";
  Fixture fixture;
  check(fixture.build(html, kStyle), "blank-page fixture parses");
  const auto pages = fixture.pageTexts();
  check(pages.size() == 3, "no blank page: three pages");
  for (const auto& text : pages) check(!text.empty(), "no blank page: every page carries text");
  if (pages.size() == 3) {
    check(startsWith(pages[0], word("F", 0)), "no blank page: the first break block opens page 1");
    check(pages[1] == words("w", 5), "no blank page: the full page keeps its five lines");
    check(startsWith(pages[2], word("G", 0)), "no blank page: the second break block opens page 3");
  }
}

void testFootnoteFollowsItsMarker() {
  // The footnote marker sits on the paragraph's last line, which the widow rule
  // carries; its note must move with it instead of staying behind.
  Fixture fixture;
  const std::string html = "<html><body><p>" + words("Q", 5) + " <a href=\"chapter.xhtml#n1\">" + word("N", 0) +
                           "</a></p></body></html>";
  check(fixture.build(html, kStyle), "footnote fixture parses");
  const auto pages = fixture.pageTexts();
  check(pages.size() == 2, "footnote fixture has two pages");
  if (pages.size() != 2) return;
  check(fixture.pages[0]->footnotes.size() == 0, "footnote: the note does not stay on the page it left");
  check(fixture.pages[1]->footnotes.size() == 1, "footnote: the note follows its carried marker");
  check(fixture.pages[1]->links.size() >= 1, "footnote: the marker link moved with the line");
}

}  // namespace

int main() {
  testPolicy();
  testOrphan();
  testWidow();
  testKeepWithNext();
  testPageBreakBefore();
  testPageBreakBeforeFromContainer();
  testPageBreakAfter();
  testNoBlankPages();
  testFootnoteFollowsItsMarker();

  if (g_failures == 0) {
    std::printf("layout M3: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "layout M3: %d check(s) failed\n", g_failures);
  return 1;
}
