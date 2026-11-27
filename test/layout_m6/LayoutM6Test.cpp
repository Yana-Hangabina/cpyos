// Host test for layout milestone M6: a `::first-letter` rule with at least twice the
// block's font size turns the paragraph's first character into a drop cap, and the first
// lines of that block start to the right of the glyph.
//
// Build with run_layout_m6_test.sh (bare g++, no cmake/gtest). The parser, ParsedText,
// Page and TextBlock are the shipped sources compiled against the M2 font-aware renderer
// stub, so a page is exactly five 16 px lines on the 480x80 viewport used here and every
// glyph advances 8 px. The script also builds two mutants (a layout that ignores the
// reservation and a parser that never emits the element) and requires this binary to fail.

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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
  std::string htmlPath;
  GfxRenderer renderer;
  CssParser css{"/tmp"};
  std::vector<std::unique_ptr<Page>> pages;

  static constexpr int kLineHeight = 16;
  static constexpr int kPageHeight = 5 * kLineHeight;
  static constexpr int kPageWidth = 480;
  static constexpr int kSectionFontId = 7;

  bool build(const std::string& html, const std::string& stylesheet, const bool vertical = false) {
    std::error_code ec;
    dir = std::filesystem::temp_directory_path(ec) / "layout_m6_test";
    std::filesystem::create_directories(dir, ec);
    htmlPath = dir + "/chapter.xhtml";
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
        nullptr, htmlPath, renderer, kSectionFontId, /*lineCompression=*/1.0f, /*extraParagraphSpacing=*/0,
        /*firstLineIndent=*/0, /*firstLineIndentPx=*/0,
        /*paragraphAlignment=*/static_cast<uint8_t>(CssTextAlign::Justify),
        /*verticalBottomAlign=*/false, width, height, /*hyphenation=*/false, /*focusReading=*/false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pages.push_back(std::move(page)); },
        /*embeddedStyle=*/true, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/0, {}, nullptr, &css,
        /*collectTouchLinks=*/true, /*blockFonts=*/nullptr);
    parser.setVerticalText(vertical);
    return parser.parseAndBuildPages();
  }

  // Text of every line, page by page, so two builds can be compared line for line.
  [[nodiscard]] std::vector<std::vector<std::string>> lineTexts() const {
    std::vector<std::vector<std::string>> out;
    for (const auto& page : pages) {
      std::vector<std::string> lines;
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto* line = static_cast<const PageLine*>(element.get());
        const auto& words = *line->getBlock();
        std::string text;
        for (uint16_t i = 0; i < words.wordCount(); ++i) {
          if (i > 0) text += ' ';
          text += words.wordText(i);
        }
        lines.push_back(text);
      }
      out.push_back(std::move(lines));
    }
    return out;
  }

  // Serialised pages, so two builds can be compared byte for byte.
  [[nodiscard]] std::string serialized(const std::string& name) const {
    const std::string path = dir + "/" + name;
    HalFile file;
    if (!file.open(path.c_str(), "w+b")) return {};
    for (const auto& page : pages) {
      if (!page->serialize(file)) return {};
    }
    file.close();
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  // Left edge of every line's first word, page by page.
  [[nodiscard]] std::vector<std::vector<int>> lineLefts() const {
    std::vector<std::vector<int>> out;
    for (const auto& page : pages) {
      std::vector<int> lefts;
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto* line = static_cast<const PageLine*>(element.get());
        lefts.push_back(line->xPos + (line->getBlock()->wordCount() > 0 ? line->getBlock()->wordXpos(0) : 0));
      }
      out.push_back(std::move(lefts));
    }
    return out;
  }
};

std::vector<const PageDropCap*> capsOf(const Page* page) {
  std::vector<const PageDropCap*> caps;
  for (const auto& element : page->elements) {
    if (element->getTag() == TAG_PageDropCap) caps.push_back(static_cast<const PageDropCap*>(element.get()));
  }
  return caps;
}

size_t countCaps(const std::vector<std::unique_ptr<Page>>& pages) {
  size_t total = 0;
  for (const auto& page : pages) total += capsOf(page.get()).size();
  return total;
}

const char* const kCapStyle = "p{margin:0}.cap::first-letter{font-size:3em;float:left}";

std::string words(const char* prefix, const int count) {
  std::string html;
  for (int i = 0; i < count; ++i) {
    if (i > 0) html += ' ';
    html += prefix;
    html += std::to_string(i);
  }
  return html;
}

void testCapIsLaidOut() {
  Fixture fixture;
  check(fixture.build("<html><body><p class=\"cap\">Alpha " + words("word", 40) + "</p></body></html>", kCapStyle),
        "cap fixture parses");
  check(countCaps(fixture.pages) == 1, "cap: exactly one element for one capped block");
  if (countCaps(fixture.pages) != 1) return;

  const PageDropCap* cap = capsOf(fixture.pages[0].get()).front();
  check(cap->codepoint() == 'A', "cap: carries the paragraph's first letter");
  check(cap->scale() == 768, "cap: a 3em rule clamps to the 3x the primitive draws");
  check(cap->xPos == 0, "cap: sits at the block's left edge");
  check(cap->yPos == 0, "cap: ink top is the first reserved line's top");
  check(cap->capWidth() == 24, "cap: reserves the scaled glyph's ink width");
  check(cap->capHeight() == 36, "cap: reports the scaled glyph's ink height");
  check(!fixture.pages[0]->elements.empty() && fixture.pages[0]->elements[0]->getTag() == TAG_PageDropCap,
        "cap: the element precedes the line it belongs to");

  // "Alpha" loses its first letter: the rest of the word starts the paragraph.
  const auto texts = fixture.lineTexts();
  check(!texts.empty() && !texts[0].empty(), "cap: the first page carries lines");
  if (texts.empty() || texts[0].empty()) return;
  check(texts[0][0].rfind("lpha", 0) == 0, "cap: the remaining letters of the word continue the line");

  // 36 px of glyph over a 16 px line is three lines of reservation, then the full width.
  const auto lefts = fixture.lineLefts();
  check(lefts[0].size() >= 4, "cap: the block spans at least four lines");
  if (lefts[0].size() < 4) return;
  check(lefts[0][0] == 24, "cap: line 0 starts after the glyph");
  check(lefts[0][1] == 24, "cap: line 1 starts after the glyph");
  check(lefts[0][2] == 24, "cap: line 2 starts after the glyph");
  check(lefts[0][3] == 0, "cap: line 3 returns to the margin");
}

void testRuleNeedsDoubleSize() {
  // A ::first-letter rule that only sets a decorative property is not a drop cap.
  const char* const style = "p{margin:0}.cap::first-letter{font-weight:bold}";
  Fixture capped;
  Fixture plain;
  check(capped.build("<html><body><p class=\"cap\">Alpha " + words("word", 10) + "</p></body></html>", style),
        "small first-letter fixture parses");
  check(plain.build("<html><body><p>Alpha " + words("word", 10) + "</p></body></html>", style),
        "control fixture parses");
  check(!capped.pages.empty(), "size rule: the fixtures produced pages");
  check(countCaps(capped.pages) == 0, "size rule: a first-letter rule below 2x draws no cap");
  check(capped.lineTexts() == plain.lineTexts(), "size rule: lines match the unstyled paragraph");
  check(capped.lineLefts() == plain.lineLefts(), "size rule: word positions match the unstyled paragraph");

  // Anything past 3x would overflow the lines it spans, so the scale stops there.
  Fixture huge;
  check(huge.build("<html><body><p class=\"cap\">Alpha " + words("word", 10) + "</p></body></html>",
                   "p{margin:0}.cap::first-letter{font-size:6em}"),
        "clamped fixture parses");
  check(countCaps(huge.pages) == 1, "size rule: a 6em rule still draws one cap");
  if (countCaps(huge.pages) != 1) return;
  check(capsOf(huge.pages[0].get()).front()->scale() == 768, "size rule: the scale clamps to 3x");
}

void testWithoutRuleIsUnchanged() {
  // No ::first-letter rule at all: the block must paginate exactly like the control.
  Fixture cappedStyle;
  Fixture plainStyle;
  const std::string body = "Alpha " + words("word", 40);
  check(cappedStyle.build("<html><body><p class=\"plain\">" + body + "</p></body></html>", kCapStyle),
        "rule-free fixture parses");
  check(plainStyle.build("<html><body><p>" + body + "</p></body></html>", "p{margin:0}"),
        "rule-free control parses");
  check(countCaps(cappedStyle.pages) == 0, "no rule: nothing is emitted");
  check(!cappedStyle.pages.empty() && cappedStyle.pages.size() == plainStyle.pages.size(),
        "no rule: both fixtures produced the same pages");
  check(cappedStyle.pages.size() == plainStyle.pages.size(), "no rule: same page count as the control");
  check(cappedStyle.lineTexts() == plainStyle.lineTexts(), "no rule: same lines as the control");
  check(cappedStyle.lineLefts() == plainStyle.lineLefts(), "no rule: same word positions as the control");
  const std::string cappedBytes = cappedStyle.serialized("capped.pages");
  check(!cappedBytes.empty(), "no rule: the pages serialise");
  check(cappedBytes == plainStyle.serialized("control.pages"),
        "no rule: the serialised pages are byte-identical to the control");
}

void testSingleCodepointFirstWord() {
  // The paragraph opens with a one-letter word: only that letter is consumed, and the
  // following word starts the indented line.
  Fixture fixture;
  check(fixture.build("<html><body><p class=\"cap\">X " + words("word", 20) + "</p></body></html>", kCapStyle),
        "single letter fixture parses");
  check(countCaps(fixture.pages) == 1, "single letter: one cap");
  if (countCaps(fixture.pages) != 1) return;
  const PageDropCap* cap = capsOf(fixture.pages[0].get()).front();
  check(cap->codepoint() == 'X', "single letter: the cap is that letter");
  const auto texts = fixture.lineTexts();
  check(!texts.empty() && !texts[0].empty(), "single letter: the page carries lines");
  if (texts.empty() || texts[0].empty()) return;
  check(texts[0][0].rfind("word0", 0) == 0, "single letter: only one character was consumed");
  const auto lefts = fixture.lineLefts();
  check(lefts[0][0] == 24, "single letter: the cap still reserves its column");
}

void testCapFollowsCarriedFirstLine() {
  // Four plain lines fill most of the page, so the capped paragraph starts at the bottom
  // and its first line is carried to the next page by the orphan rule. The glyph must
  // travel with the line it enlarges, not stay behind on a page without it.
  Fixture fixture;
  check(fixture.build("<html><body><p>one</p><p>two</p><p>three</p><p>four</p>"
                      "<p class=\"cap\">Alpha " + words("word", 20) + "</p></body></html>",
                      kCapStyle),
        "carry fixture parses");
  check(fixture.pages.size() >= 2, "carry: the paragraph is split over two pages");
  if (fixture.pages.size() < 2) return;
  check(capsOf(fixture.pages[0].get()).empty(), "carry: the page that lost the first line has no cap");
  check(capsOf(fixture.pages[1].get()).size() == 1, "carry: the cap moved to the page with its first line");
  const auto lefts = fixture.lineLefts();
  check(!lefts[1].empty() && lefts[1][0] == 24, "carry: the carried line keeps its placeholder column");
}

void testVerticalAndTableEmitNothing() {
  Fixture vertical;
  check(vertical.build("<html><body><p class=\"cap\">Alpha " + words("word", 6) + "</p></body></html>", kCapStyle,
                       /*vertical=*/true),
        "vertical fixture parses");
  check(countCaps(vertical.pages) == 0, "vertical: no cap element");

  Fixture table;
  check(table.build("<html><body><table><tr><td class=\"cap\">Alpha " + words("word", 6) + "</td></tr></table></body></html>",
                    kCapStyle),
        "table fixture parses");
  check(countCaps(table.pages) == 0, "table: no cap element inside a cell");
}

}  // namespace

int main() {
  testCapIsLaidOut();
  testRuleNeedsDoubleSize();
  testWithoutRuleIsUnchanged();
  testSingleCodepointFirstWord();
  testCapFollowsCarriedFirstLine();
  testVerticalAndTableEmitNothing();

  if (g_failures == 0) {
    std::printf("layout M6: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "layout M6: %d check(s) failed\n", g_failures);
  return 1;
}
