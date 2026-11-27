// Host test for layout milestone M2: block-level CSS font-size.
//
// Build with run_layout_m2_test.sh (bare g++, no cmake/gtest). It compiles the real
// ChapterHtmlSlimParser/ParsedText/Page/TextBlock against a font-aware renderer stub,
// feeds a small XHTML + stylesheet, and checks:
//   * headings and `font-size` blocks resolve to a scaled face id;
//   * the `<p><span class="big">Chapter One</span></p>` whole-block heuristic sizes
//     the block, and body text after the span cancels it;
//   * <small> gets the browser's 0.83em default and 10em clamps to 3x;
//   * line height follows the block face (24 px vs 16 px in the stub);
//   * BlockStyle::fontId survives a TextBlock serialize/deserialize round trip.
// run_layout_m2_test.sh also builds a mutated parser that ignores CSS font-size and
// requires this binary to fail, so the checks cannot pass vacuously.

#include <Epub/Page.h>
#include <Epub/blocks/BlockFontProvider.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

constexpr int kSectionFontId = 7;

class TestBlockFonts final : public BlockFontProvider {
 public:
  int resolveBlockFont(const float scale) override {
    requested.push_back(scale);
    // The parser already clamps; snap a 1.0 block back to "section font".
    if (std::fabs(scale - 1.0f) < 0.01f) return 0;
    const int id = layout_m2_stub::kBlockFontBase + static_cast<int>(std::lround(scale * 100.0f));
    for (const int seen : ids) {
      if (seen == id) return id;
    }
    ids.push_back(id);
    return id;
  }

  std::vector<float> requested;
  std::vector<int> ids;
};

struct LineInfo {
  std::string firstWord;
  int fontId = 0;
  int y = 0;
};

struct Fixture {
  std::string dir;
  GfxRenderer renderer;
  CssParser css{"/tmp"};
  TestBlockFonts fonts;
  std::vector<std::unique_ptr<Page>> pages;

  bool build(const std::string& html) {
    std::error_code ec;
    dir = std::filesystem::temp_directory_path(ec) / "layout_m2_test";
    std::filesystem::create_directories(dir, ec);
    const std::string htmlPath = dir + "/chapter.xhtml";
    const std::string cssPath = dir + "/style.css";
    {
      std::ofstream out(htmlPath, std::ios::binary);
      out << html;
    }
    {
      std::ofstream out(cssPath, std::ios::binary);
      out << ".big{font-size:2em}.title{font-size:1.5em}.huge{font-size:10em}"
             ".pct{font-size:120%}";
    }
    HalFile cssFile;
    if (!cssFile.open(cssPath.c_str(), "rb")) return false;
    css.loadFromStream(cssFile);
    cssFile.close();

    ChapterHtmlSlimParser parser(
        nullptr, htmlPath, renderer, kSectionFontId, 1.0f, /*extraParagraphSpacing=*/0, /*firstLineIndent=*/0,
        /*firstLineIndentPx=*/0, /*paragraphAlignment=*/static_cast<uint8_t>(CssTextAlign::Justify),
        /*verticalBottomAlign=*/false, renderer.getScreenWidth(), renderer.getScreenHeight(), /*hyphenation=*/false,
        /*focusReading=*/false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pages.push_back(std::move(page)); },
        /*embeddedStyle=*/true, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/0, {}, nullptr, &css,
        /*collectTouchLinks=*/false, &fonts);
    return parser.parseAndBuildPages();
  }

  std::vector<LineInfo> lines() const {
    std::vector<LineInfo> out;
    for (const auto& page : pages) {
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto* line = static_cast<const PageLine*>(element.get());
        const TextBlock& block = *line->getBlock();
        LineInfo info;
        info.firstWord = block.wordCount() > 0 ? block.wordText(0) : "";
        info.fontId = block.getBlockStyle().fontId;
        info.y = element->yPos;
        out.push_back(info);
      }
    }
    return out;
  }
};

std::string longParagraph(const std::string& cls, const std::string& prefix, int words) {
  std::string html = "<p class=\"" + cls + "\">";
  for (int i = 0; i < words; ++i) {
    html += prefix + std::to_string(i);
    html += " ";
  }
  html += "</p>";
  return html;
}

void testSerializationRoundTrip() {
  BlockStyle style;
  style.fontId = 0x12345678;
  style.textIndent = 12;
  TextBlock block({"font", "round", "trip"}, {0, 10, 20},
                  {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, {}, {}, style);
  check(block.valid(), "serialization fixture block is valid");

  const std::string path = "/tmp/layout_m2_block.bin";
  {
    HalFile file;
    check(file.open(path.c_str(), "w+b"), "open serialization target");
    check(block.serialize(file), "serialize block");
    file.close();
  }
  {
    HalFile file;
    check(file.open(path.c_str(), "rb"), "reopen serialization target");
    auto restored = TextBlock::deserialize(file);
    file.close();
    check(restored != nullptr, "deserialize block");
    if (restored) {
      check(restored->getBlockStyle().fontId == 0x12345678, "font id round-trips");
      check(restored->getBlockStyle().textIndent == 12, "other block style fields still round-trip");
      check(restored->wordCount() == 3, "word count round-trips");
    }
  }
  std::remove(path.c_str());
}

}  // namespace

int main() {
  Fixture fixture;
  const std::string html =
      "<html><body>"
      "<h1>Heading</h1>"
      "<div class=\"title\">Title text</div>"
      "<div class=\"title\"><p>Inherited title</p></div>"
      "<p class=\"pct\">Percent sized</p>"
      "<p class=\"huge\">Clamped huge</p>"
      "<p><span class=\"big\">Chapter One</span></p>"
      "<p><span class=\"big\">Chapter Two</span> and ordinary body text follows the span.</p>"
      "<p><small>small text</small></p>" +
      longParagraph("big", "large", 60) + longParagraph("", "plain", 120) + "</body></html>";
  check(fixture.build(html), "XHTML parses and lays out");

  const auto lines = fixture.lines();
  check(!lines.empty(), "pages carry text lines");

  // Scale -> id as the test provider computes it.
  const auto idFor = [](const float scale) {
    return layout_m2_stub::kBlockFontBase + static_cast<int>(std::lround(scale * 100.0f));
  };
  const int bigId = idFor(2.0f);
  const int titleId = idFor(1.5f);
  const int smallId = idFor(0.83f);
  const int pctId = idFor(1.2f);
  const int maxId = idFor(3.0f);

  check(bigId != titleId, "distinct sizes get distinct ids");

  bool sawHeading = false, sawTitle = false, sawInherited = false, sawSpanOnly = false;
  bool sawSpanPlusText = false, sawSmall = false, sawPct = false, sawClamped = false;
  for (const auto& line : lines) {
    if (line.firstWord == "Heading") {
      sawHeading = true;
      check(line.fontId == bigId, "h1 uses 2x face");
    } else if (line.firstWord == "Title") {
      sawTitle = true;
      check(line.fontId == titleId, ".title 1.5em uses 1.5x face");
    } else if (line.firstWord == "Inherited") {
      sawInherited = true;
      check(line.fontId == titleId, "child block inherits the parent font-size");
    } else if (line.firstWord == "Percent") {
      sawPct = true;
      check(line.fontId == pctId, "percentage font-size is relative to the parent");
    } else if (line.firstWord == "Clamped") {
      sawClamped = true;
      check(line.fontId == maxId, "10em clamps to 3x");
    } else if (line.firstWord == "Chapter") {
      if (line.fontId == bigId) {
        sawSpanOnly = true;
      } else if (line.fontId == 0) {
        sawSpanPlusText = true;
      }
    } else if (line.firstWord == "small") {
      sawSmall = true;
      check(line.fontId == smallId, "<small> uses the 0.83x default");
    }
  }
  check(sawHeading, "h1 block was laid out");
  check(sawTitle, ".title block was laid out");
  check(sawInherited, "nested block was laid out");
  check(sawPct, "percentage block was laid out");
  check(sawClamped, "clamped block was laid out");
  check(sawSpanOnly, "span-only paragraph is sized as a block");
  check(sawSpanPlusText, "span followed by body text keeps the section font");
  check(sawSmall, "<small> block was laid out");

  // Line height must follow the block face: consecutive lines of the large
  // paragraph advance 24 px, the plain one 16 px (stub metrics).
  int largeDelta = 0, plainDelta = 0;
  int lastLargeY = -1, lastPlainY = -1;
  for (const auto& line : lines) {
    if (line.firstWord.rfind("large", 0) == 0) {
      if (lastLargeY >= 0 && largeDelta == 0) largeDelta = line.y - lastLargeY;
      lastLargeY = line.y;
    } else if (line.firstWord.rfind("plain", 0) == 0) {
      if (lastPlainY >= 0 && plainDelta == 0) plainDelta = line.y - lastPlainY;
      lastPlainY = line.y;
    }
  }
  check(largeDelta == 24, "scaled block lines are 24 px apart");
  check(plainDelta == 16, "section-font lines stay 16 px apart");

  // 60 large words must wrap into more lines than 60 plain words would: the larger
  // advance is what makes that true. Count lines instead of re-measuring here.
  int largeLines = 0, plainLines = 0;
  for (const auto& line : lines) {
    if (line.firstWord.rfind("large", 0) == 0) ++largeLines;
    if (line.firstWord.rfind("plain", 0) == 0) ++plainLines;
  }
  check(largeLines >= 2 && plainLines >= 2, "both long paragraphs wrap");

  // The provider must have been asked for the clamped scale, never the raw 10em.
  bool sawClampRequest = false;
  for (const float scale : fixture.fonts.requested) {
    check(scale <= 3.0f && scale >= 0.5f, "provider only sees clamped scales");
    if (std::fabs(scale - 3.0f) < 0.001f) sawClampRequest = true;
  }
  check(sawClampRequest, "clamp reached the provider");
  // One id per distinct scale in the fixture (2.0, 1.5, 1.2, 3.0, 0.83); the
  // production provider enforces the 3-extra-face cap, which this host build does not link.
  check(fixture.fonts.ids.size() == 5, "one face per distinct block size");

  testSerializationRoundTrip();

  if (g_failures == 0) {
    std::printf("layout M2: all checks passed (%zu lines, %zu faces)\n", lines.size(), fixture.fonts.ids.size());
    return 0;
  }
  std::fprintf(stderr, "layout M2: %d check(s) failed\n", g_failures);
  return 1;
}
