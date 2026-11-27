// Host test for layout milestone M4b: the chapter parser emits a PageBorderBox for a
// block that carries CSS border or background-shade decoration.
//
// Build with run_layout_m4_test.sh (bare g++, no cmake/gtest). The parser, ParsedText,
// Page and TextBlock are the shipped sources compiled against the M2 font-aware renderer
// stub, so a page is exactly five 16 px lines on the 480x80 viewport used here. The
// script also builds two mutants (a parser that never opens a box, and one that keeps the
// top edge on a continuation) and requires this binary to fail.

#include <Epub/Page.h>
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

struct Fixture {
  std::string dir;
  GfxRenderer renderer;
  CssParser css{"/tmp"};
  std::vector<std::unique_ptr<Page>> pages;

  // The stub's section face is 16 px, so five lines fill a page and a page boundary is
  // one line decision.
  static constexpr int kLineHeight = 16;
  static constexpr int kPageHeight = 5 * kLineHeight;
  static constexpr int kPageWidth = 480;

  bool build(const std::string& html, const std::string& stylesheet, const bool vertical = false) {
    std::error_code ec;
    dir = std::filesystem::temp_directory_path(ec) / "layout_m4_test";
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

    // Vertical writing swaps the viewport axes, exactly as Section.cpp does.
    const uint16_t width = static_cast<uint16_t>(vertical ? kPageHeight : kPageWidth);
    const uint16_t height = static_cast<uint16_t>(vertical ? kPageWidth : kPageHeight);
    ChapterHtmlSlimParser parser(
        nullptr, htmlPath, renderer, /*fontId=*/7, /*lineCompression=*/1.0f, /*extraParagraphSpacing=*/0,
        /*firstLineIndent=*/0, /*firstLineIndentPx=*/0,
        /*paragraphAlignment=*/static_cast<uint8_t>(CssTextAlign::Justify),
        /*verticalBottomAlign=*/false, width, height, /*hyphenation=*/false, /*focusReading=*/false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pages.push_back(std::move(page)); },
        /*embeddedStyle=*/true, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/0, {}, nullptr, &css,
        /*collectTouchLinks=*/true, /*blockFonts=*/nullptr);
    parser.setVerticalText(vertical);
    return parser.parseAndBuildPages();
  }

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
};

size_t countBoxes(const Page* page) {
  size_t count = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() == TAG_PageBorderBox) ++count;
  }
  return count;
}

// Box i of the page, or nullptr when the page carries fewer than i+1.
const PageBorderBox* boxAt(const Page* page, const size_t index) {
  size_t seen = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageBorderBox) continue;
    if (seen++ == index) return static_cast<const PageBorderBox*>(element.get());
  }
  return nullptr;
}

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

const char* const kStyle =
    "p{margin:0}.box{border:2px solid #000;background-color:gray}"
    ".inset{border-left:1px solid #000;margin-left:16px;margin-right:8px}"
    ".brk{page-break-before:page}";

void testSingleBox() {
  Fixture fixture;
  check(fixture.build("<html><body><p class=\"box\">" + words("b", 3) + "</p></body></html>", kStyle),
        "single box fixture parses");
  check(fixture.pages.size() == 1, "single box: one page");
  if (fixture.pages.size() != 1) return;
  const Page* page = fixture.pages[0].get();
  check(countBoxes(page) == 1, "single box: exactly one box for one decorated block");
  check(!page->elements.empty() && page->elements[0]->getTag() == TAG_PageBorderBox,
        "single box: the box is inserted before the block's first line");
  check(page->elements.size() > 1 && page->elements[1]->getTag() == TAG_PageLine,
        "single box: the block's first line follows the box");
  const PageBorderBox* box = boxAt(page, 0);
  if (!box) return;
  check(box->xPos == 0, "single box: starts at the content lane's left edge");
  check(box->yPos == 0, "single box: starts at the block's first line");
  check(box->boxWidth() == Fixture::kPageWidth, "single box: spans the full content lane");
  check(box->boxHeight() == 3 * Fixture::kLineHeight, "single box: covers the block's three lines");
  check(box->borderWidthTop() == 2 && box->borderWidthRight() == 2 && box->borderWidthBottom() == 2 &&
            box->borderWidthLeft() == 2,
        "single box: keeps all four declared edges");
  check(box->shade() == 8, "single box: keeps the background ink level");
}

void testBoxFollowsPrecedingLines() {
  // Two plain lines, then a two-line decorated block: the box starts below the text that
  // came before it and never covers it.
  Fixture fixture;
  check(fixture.build("<html><body><p>" + words("w", 2) + "</p><p class=\"box\">" + words("b", 2) +
                          "</p></body></html>",
                      kStyle),
        "offset box fixture parses");
  check(fixture.pages.size() == 1, "offset box: one page");
  if (fixture.pages.size() != 1) return;
  const Page* page = fixture.pages[0].get();
  check(countBoxes(page) == 1, "offset box: only the decorated block emits one");
  const PageBorderBox* box = boxAt(page, 0);
  if (!box) return;
  check(box->yPos == 2 * Fixture::kLineHeight, "offset box: starts at the decorated block's first line");
  check(box->boxHeight() == 2 * Fixture::kLineHeight, "offset box: covers only the decorated block");
}

void testInsetLane() {
  // A one-sided border plus margins: the box follows the text lane, not the viewport.
  Fixture fixture;
  check(fixture.build("<html><body><p class=\"inset\">" + words("i", 2) + "</p></body></html>", kStyle),
        "inset box fixture parses");
  check(fixture.pages.size() == 1, "inset box: one page");
  if (fixture.pages.size() != 1) return;
  const PageBorderBox* box = boxAt(fixture.pages[0].get(), 0);
  if (!box) {
    check(false, "inset box: box is emitted");
    return;
  }
  check(box->xPos == 16, "inset box: starts at the block's left inset");
  check(box->boxWidth() == Fixture::kPageWidth - 16 - 8, "inset box: width is the content lane");
  check(box->borderWidthLeft() == 1, "inset box: keeps the declared edge");
  check(box->borderWidthTop() == 0 && box->borderWidthRight() == 0 && box->borderWidthBottom() == 0,
        "inset box: undeclared edges stay empty");
  check(box->shade() == 0, "inset box: no background without a shade rule");
}

void testSplitAcrossPages() {
  // Seven lines on a five-line page: five stay, two continue. The cut edges are dropped
  // on both halves so the outline reads as one interrupted box, not two.
  Fixture fixture;
  check(fixture.build("<html><body><p class=\"box\">" + words("c", 7) + "</p></body></html>", kStyle),
        "split box fixture parses");
  check(fixture.pages.size() == 2, "split box: two pages");
  if (fixture.pages.size() != 2) return;
  const auto texts = fixture.pageTexts();
  check(texts[0] == words("c", 5), "split box: five lines stay on the first page");
  check(texts[1] == word("c", 5) + " " + word("c", 6), "split box: two lines continue");

  check(countBoxes(fixture.pages[0].get()) == 1, "split box: first page carries one box");
  const PageBorderBox* first = boxAt(fixture.pages[0].get(), 0);
  if (first) {
    check(first->yPos == 0, "split box: first part starts at the top");
    check(first->boxHeight() == 5 * Fixture::kLineHeight, "split box: first part covers its five lines");
    check(first->borderWidthTop() == 2, "split box: first part keeps the top edge");
    check(first->borderWidthBottom() == 0, "split box: first part drops the cut bottom edge");
    check(first->borderWidthLeft() == 2 && first->borderWidthRight() == 2,
          "split box: first part keeps the side edges");
  } else {
    check(false, "split box: first page box is present");
  }

  check(countBoxes(fixture.pages[1].get()) == 1, "split box: continuation page carries one box");
  const PageBorderBox* second = boxAt(fixture.pages[1].get(), 0);
  if (second) {
    check(second->yPos == 0, "split box: continuation starts at the page's first line");
    check(second->boxHeight() == 2 * Fixture::kLineHeight, "split box: continuation covers the rest");
    check(second->borderWidthTop() == 0, "split box: continuation drops the cut top edge");
    check(second->borderWidthBottom() == 2, "split box: continuation keeps the real bottom edge");
    check(second->shade() == 8, "split box: continuation keeps the background");
  } else {
    check(false, "split box: continuation box is present");
  }
}

void testForcedBreakKeepsFullOutline() {
  // A break before a decorated block must not leave a box on the page it left, and the
  // box that opens the next page is a fresh one with its top edge.
  Fixture fixture;
  check(fixture.build("<html><body><p>" + words("w", 2) + "</p><p class=\"box brk\">" + words("d", 2) +
                          "</p></body></html>",
                      kStyle),
        "break box fixture parses");
  check(fixture.pages.size() == 2, "break box: two pages");
  if (fixture.pages.size() != 2) return;
  check(countBoxes(fixture.pages[0].get()) == 0, "break box: the page before the break has no box");
  const PageBorderBox* box = boxAt(fixture.pages[1].get(), 0);
  if (!box) {
    check(false, "break box: the new page carries a box");
    return;
  }
  check(box->yPos == 0 && box->boxHeight() == 2 * Fixture::kLineHeight, "break box: covers the new block");
  check(box->borderWidthTop() == 2 && box->borderWidthBottom() == 2,
        "break box: a forced break is not a cut, both horizontal edges stay");
}

void testWidowCarryKeepsBoxesMeasurable() {
  // Six lines on a five-line page: the widow rule carries the last line, so the block is
  // cut at four lines. Both parts must keep a real height -- a box whose lines all moved
  // would be zero-height -- and the continuation starts at the line it actually covers.
  Fixture fixture;
  check(fixture.build("<html><body><p class=\"box\">" + words("e", 6) + "</p></body></html>", kStyle),
        "widow box fixture parses");
  check(fixture.pages.size() == 2, "widow box: two pages");
  if (fixture.pages.size() != 2) return;
  const auto texts = fixture.pageTexts();
  check(texts[0] == words("e", 4), "widow box: one line is carried, four stay");
  const PageBorderBox* first = boxAt(fixture.pages[0].get(), 0);
  if (!first) {
    check(false, "widow box: the first page keeps its box");
  } else {
    check(first->boxHeight() == 4 * Fixture::kLineHeight, "widow box: the box ends at the last line that stayed");
    check(first->borderWidthBottom() == 0, "widow box: the cut bottom edge is dropped");
  }
  const PageBorderBox* second = boxAt(fixture.pages[1].get(), 0);
  if (!second) {
    check(false, "widow box: the continuation carries a box");
  } else {
    check(second->boxHeight() > 0, "widow box: the continuation box is never zero-height");
    check(second->yPos == Fixture::kLineHeight, "widow box: the continuation covers the line this block places");
    check(second->borderWidthTop() == 0, "widow box: the continuation has no top edge");
  }
}

void testTableCellsEmitNoBoxes() {
  // Table cell lines are pushed outside a block, so a bordered table must not sprout one
  // box per cell line; its outline needs a table-level box, which M4b does not model.
  Fixture fixture;
  check(fixture.build("<html><body><table class=\"box\"><tr><td>alpha beta</td><td>gamma</td></tr>"
                      "<tr><td>delta</td><td>epsilon zeta</td></tr></table></body></html>",
                      kStyle),
        "table fixture parses");
  for (const auto& page : fixture.pages) check(countBoxes(page.get()) == 0, "table: cell lines emit no box");
}

void testSiblingAfterContainerKeepsNoBox() {
  // A shaded <div> hands its box to the blocks inside it, but not to the next sibling.
  // The empty block left behind by </p> still carried the div's style; merging it into
  // the following paragraph used to resurrect the box and shade the next <p> too.
  Fixture fixture;
  check(fixture.build("<html><body><div class=\"box\"><p>" + words("b", 2) + "</p></div><p>" + words("w", 2) +
                          "</p></body></html>",
                      kStyle),
        "sibling-after-container fixture parses");
  check(fixture.pages.size() == 1, "sibling box: one page");
  if (fixture.pages.size() != 1) return;
  const Page* page = fixture.pages[0].get();
  check(countBoxes(page) == 1, "sibling box: the sibling paragraph emits no second box");
  const PageBorderBox* box = boxAt(page, 0);
  if (!box) return;
  check(box->yPos == 0, "sibling box: starts at the decorated block's first line");
  check(box->boxHeight() == 2 * Fixture::kLineHeight, "sibling box: covers only the decorated block");
}

void testPlainBlockEmitsNothing() {
  Fixture fixture;
  check(fixture.build("<html><body><p>" + words("p", 7) + "</p></body></html>", kStyle),
        "plain fixture parses");
  check(fixture.pages.size() == 2, "plain: two pages");
  for (const auto& page : fixture.pages) check(countBoxes(page.get()) == 0, "plain: no box without box CSS");
}

void testVerticalEmitsNothing() {
  Fixture fixture;
  check(fixture.build("<html><body><p class=\"box\">" + words("v", 3) + "</p></body></html>", kStyle,
                      /*vertical=*/true),
        "vertical fixture parses");
  check(!fixture.pages.empty(), "vertical: pages are built");
  for (const auto& page : fixture.pages) check(countBoxes(page.get()) == 0, "vertical: no box in vertical writing");
}

}  // namespace

int main() {
  testSingleBox();
  testBoxFollowsPrecedingLines();
  testInsetLane();
  testSplitAcrossPages();
  testForcedBreakKeepsFullOutline();
  testWidowCarryKeepsBoxesMeasurable();
  testTableCellsEmitNoBoxes();
  testSiblingAfterContainerKeepsNoBox();
  testPlainBlockEmitsNothing();
  testVerticalEmitsNothing();

  if (g_failures == 0) {
    std::printf("layout M4b: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "layout M4b: %d check(s) failed\n", g_failures);
  return 1;
}
