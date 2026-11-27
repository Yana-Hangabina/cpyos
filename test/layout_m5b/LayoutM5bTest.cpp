// Host test for layout milestone M5b: an inline `font-size` that resolves to another face
// gets a per-word font run, and the words of that run are measured and positioned with the
// face they will be drawn with.
//
// Build with run_layout_m5b_test.sh (bare g++, no cmake/gtest). The parser, ParsedText, Page
// and TextBlock are the shipped sources compiled against the M2 font-aware renderer stub, so
// a glyph is 8 px on the section face and 12 px on any scaled face (ids 3000+). The script
// also builds two mutants (measurement ignoring the slot, and an arena without the slot
// region) and requires this binary to fail on both.

#include <Epub/Page.h>
#include <Epub/blocks/BlockFontProvider.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>

#include <cmath>
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

// One scaled face per CSS size, in the id range the renderer stub draws 12 px wide.
class ScaleFontProvider final : public BlockFontProvider {
 public:
  int resolveBlockFont(const float scale) override {
    if (scale >= 0.999f && scale <= 1.001f) return 0;  // the reader size: no extra face
    return 3000 + static_cast<int>(std::lround(scale * 100.0f));
  }
};

struct WordInfo {
  std::string text;
  int x = 0;
  uint8_t slot = 0;
  int32_t faceId = 0;

  bool operator==(const WordInfo& other) const {
    return text == other.text && x == other.x && slot == other.slot && faceId == other.faceId;
  }
};

struct Fixture {
  std::string dir;
  std::string htmlPath;
  GfxRenderer renderer;
  ScaleFontProvider provider;
  CssParser css{"/tmp"};
  std::vector<std::unique_ptr<Page>> pages;

  static constexpr int kLineHeight = 16;
  static constexpr int kPageHeight = 5 * kLineHeight;
  static constexpr int kPageWidth = 480;
  static constexpr int kSectionFontId = 7;

  bool build(const std::string& html, const std::string& stylesheet, const bool useProvider = true) {
    std::error_code ec;
    dir = std::filesystem::temp_directory_path(ec) / "layout_m5b_test";
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

    ChapterHtmlSlimParser parser(
        nullptr, htmlPath, renderer, kSectionFontId, /*lineCompression=*/1.0f, /*extraParagraphSpacing=*/0,
        /*firstLineIndent=*/0, /*firstLineIndentPx=*/0,
        /*paragraphAlignment=*/static_cast<uint8_t>(CssTextAlign::Justify),
        /*verticalBottomAlign=*/false,
        /*viewportWidth=*/static_cast<uint16_t>(kPageWidth), /*viewportHeight=*/static_cast<uint16_t>(kPageHeight),
        /*hyphenation=*/false, /*focusReading=*/false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pages.push_back(std::move(page)); },
        /*embeddedStyle=*/true, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/0, {}, nullptr, &css,
        /*collectTouchLinks=*/true, /*blockFontProvider=*/useProvider ? &provider : nullptr);
    return parser.parseAndBuildPages();
  }

  [[nodiscard]] std::vector<std::vector<std::string>> lineTexts() const {
    std::vector<std::vector<std::string>> out;
    for (const auto& page : pages) {
      std::vector<std::string> lines;
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto* line = static_cast<const PageLine*>(element.get());
        const auto& block = *line->getBlock();
        std::string text;
        for (uint16_t i = 0; i < block.wordCount(); ++i) {
          if (i > 0) text += ' ';
          text += block.wordText(i);
        }
        lines.push_back(text);
      }
      out.push_back(std::move(lines));
    }
    return out;
  }

  // First text line of the first page, as positioned words with their faces.
  [[nodiscard]] std::vector<WordInfo> firstLineWords() const {
    std::vector<WordInfo> out;
    if (pages.empty()) return out;
    for (const auto& element : pages[0]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto* line = static_cast<const PageLine*>(element.get());
      const auto& block = *line->getBlock();
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        WordInfo info;
        info.text = block.wordText(i);
        info.x = line->xPos + block.wordXpos(i);
        info.slot = block.wordFontSlot(i);
        info.faceId = block.wordFontId(info.slot);
        out.push_back(std::move(info));
      }
      break;
    }
    return out;
  }

  [[nodiscard]] const TextBlock* firstLineBlock() const {
    if (pages.empty()) return nullptr;
    for (const auto& element : pages[0]->elements) {
      if (element->getTag() == TAG_PageLine) return static_cast<const PageLine*>(element.get())->getBlock().get();
    }
    return nullptr;
  }

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
};

const char* const kStyle = "p{margin:0}.s2{font-size:2em}.s3{font-size:3em}.s15{font-size:1.5em}"
                            ".s25{font-size:2.5em}.sc{font-variant:small-caps}";

// Fits one 480 px line at both sizes, so positions can be compared word by word.
const char* const kShortBody = "<html><body><p>alpha beta gamma delta</p></body></html>";

std::string mixedBody() {
  return "<html><body><p>alpha <span class=\"s2\">beta gamma</span> delta</p></body></html>";
}

void testPlainParagraphUnchanged() {
  Fixture withProvider;
  Fixture withoutProvider;
  check(withProvider.build(kShortBody, kStyle, /*useProvider=*/true), "plain: provider fixture parses");
  check(withoutProvider.build(kShortBody, kStyle, /*useProvider=*/false), "plain: control fixture parses");
  check(withProvider.lineTexts() == withoutProvider.lineTexts(), "plain: same lines as the control");
  check(withProvider.serialized("plain_p.pages") == withoutProvider.serialized("plain_c.pages"),
        "plain: serialised pages are byte-identical to the control");

  const TextBlock* block = withProvider.firstLineBlock();
  check(block != nullptr, "plain: the paragraph produced a line");
  if (block == nullptr) return;
  check(block->fontSlots() == 0, "plain: no slot table on a single-face line");
  for (uint16_t i = 0; i < block->wordCount(); ++i) {
    check(block->wordFontSlot(i) == 0, "plain: every word keeps the line face");
  }
}

void testInlineRunMeasuresWithItsFace() {
  Fixture control;
  Fixture mixed;
  check(control.build(kShortBody, kStyle), "run: control parses");
  check(mixed.build(mixedBody(), kStyle), "run: mixed fixture parses");
  check(control.lineTexts() != mixed.lineTexts() || control.firstLineWords() != mixed.firstLineWords(),
        "run: the enlarged words change the layout");

  const auto controlWords = control.firstLineWords();
  const auto mixedWords = mixed.firstLineWords();
  check(controlWords.size() == 4 && mixedWords.size() == 4, "run: both fixtures keep four words on one line");
  if (controlWords.size() != 4 || mixedWords.size() != 4) return;

  // Base glyph 8 px, scaled 12 px: "beta" (4) + "gamma" (5) add 4 px per character, and
  // the two inter-word spaces inside the run widen with them (4 -> 6 px).
  const int expectedShift = 4 * (4 + 5) + 2 * (6 - 4);
  check(mixedWords[0].x == controlWords[0].x, "run: the word before the run keeps its position");
  check(mixedWords[1].x == controlWords[1].x, "run: the run starts where the control puts it");
  check(mixedWords[1].slot == 1 && mixedWords[1].faceId == 3200, "run: the first run word carries the 2em face");
  check(mixedWords[2].slot == 1 && mixedWords[2].faceId == 3200, "run: the second run word carries the 2em face");
  check(mixedWords[3].slot == 0, "run: the word after the run returns to the line face");
  check(mixedWords[3].x == controlWords[3].x + expectedShift,
        "run: the enlarged words advance the following word by their extra width");

  const TextBlock* block = mixed.firstLineBlock();
  check(block != nullptr, "run: the mixed paragraph produced a line");
  if (block == nullptr) return;
  check(block->fontSlots() == 1, "run: the line keeps one extra face");
  const TextBlock* controlBlock = control.firstLineBlock();
  check(controlBlock != nullptr, "run: the control produced a line");
  if (controlBlock == nullptr) return;
  check(block->wordCount() == controlBlock->wordCount(), "run: both lines hold the same words");
  check(block->arenaBytes() == controlBlock->arenaBytes() + block->wordCount(),
        "run: the arena grows by exactly one slot byte per word");
}

void testUnrunParagraphsKeepTheArena() {
  Fixture fixture;
  check(fixture.build("<html><body><p>alpha beta gamma delta</p><p>one two three four</p></body></html>", kStyle),
        "arena: two plain paragraphs parse");
  size_t lines = 0;
  for (const auto& page : fixture.pages) {
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto* line = static_cast<const PageLine*>(element.get());
      check(line->getBlock()->fontSlots() == 0, "arena: an all-body line carries no slot region");
      ++lines;
    }
  }
  check(lines == 2, "arena: both paragraphs laid out as one line each");
}

void testSerializationRoundTripsSlots() {
  Fixture fixture;
  check(fixture.build(mixedBody(), kStyle), "round-trip: fixture parses");
  const std::string bytes = fixture.serialized("mixed.pages");
  check(!bytes.empty(), "round-trip: the pages serialise");
  if (bytes.empty()) return;

  HalFile file;
  const std::string path = fixture.dir + "/mixed.pages";
  check(file.open(path.c_str(), "rb"), "round-trip: the serialised file opens");
  auto page = Page::deserialize(file, true);
  file.close();
  check(page != nullptr, "round-trip: the page deserialises");
  if (!page) return;

  const TextBlock* block = nullptr;
  for (const auto& element : page->elements) {
    if (element->getTag() == TAG_PageLine) block = static_cast<const PageLine*>(element.get())->getBlock().get();
  }
  check(block != nullptr, "round-trip: the page still carries its line");
  if (block == nullptr) return;
  check(block->fontSlots() == 1, "round-trip: the slot table survived");
  check(block->wordFontId(1) == 3200, "round-trip: the slot id survived");
  check(block->wordFontSlot(0) == 0 && block->wordFontSlot(1) == 1 && block->wordFontSlot(2) == 1 &&
            block->wordFontSlot(3) == 0,
        "round-trip: every word kept its slot");

  HalFile copy;
  const std::string copyPath = fixture.dir + "/mixed_copy.pages";
  check(copy.open(copyPath.c_str(), "w+b"), "round-trip: the copy opens");
  check(page->serialize(copy), "round-trip: the page re-serialises");
  copy.close();
  std::ifstream in(copyPath, std::ios::binary);
  const std::string again((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  check(again == bytes, "round-trip: re-serialising reproduces the same bytes");
}

void testFourthSizeFallsBackToTheBlockFace() {
  Fixture fixture;
  check(fixture.build("<html><body><p>lead <span class=\"s2\">aa</span> <span class=\"s3\">bb</span> "
                      "<span class=\"s15\">cc</span> <span class=\"s25\">dd</span> tail</p></body></html>",
                      kStyle),
        "cap: four inline sizes parse");
  const TextBlock* block = fixture.firstLineBlock();
  check(block != nullptr, "cap: the paragraph produced a line");
  if (block == nullptr) return;
  check(block->fontSlots() == 3, "cap: the line keeps at most three extra faces");

  const auto words = fixture.firstLineWords();
  check(words.size() == 6, "cap: all six words are on the line");
  if (words.size() != 6) return;
  check(words[0].slot == 0, "cap: the body word keeps the line face");
  check(words[1].slot != 0 && words[2].slot != 0 && words[3].slot != 0,
        "cap: the first three runs get a slot each");
  check(words[4].text == "dd" && words[4].slot == 0 && words[4].faceId == 0,
        "cap: the fourth run falls back to the line face");
  check(words[5].x == words[4].x + 8 * 2 + 4, "cap: the fallback run measures at the block size");
}

void testFocusAndDecorationsFollowTheRun() {
  // A run word that is also bold and underlined must keep both the face and the style.
  Fixture fixture;
  check(fixture.build("<html><body><p>alpha <span class=\"s2\"><b><u>beta</u></b></span> gamma</p></body></html>",
                      kStyle),
        "style: a styled run parses");
  const TextBlock* block = fixture.firstLineBlock();
  check(block != nullptr, "style: the paragraph produced a line");
  if (block == nullptr) return;
  check(block->wordFontSlot(1) == 1, "style: the bold underlined word keeps its face slot");
  const auto style = block->wordStyle(1);
  check((style & EpdFontFamily::BOLD) != 0, "style: the run word is still bold");
  check((style & EpdFontFamily::UNDERLINE) != 0, "style: the run word is still underlined");
}

void testInlineSpanSmallCaps() {
  // font-variant on a span must reach the words inside it: the renderer only shrinks a
  // run when the word style carries SMALL_CAPS. It used to be dropped here, so an
  // inline <span class="sc"> rendered as ordinary sentence case.
  Fixture fixture;
  check(fixture.build("<html><body><p>plain <span class=\"sc\">caps here</span> tail</p></body></html>", kStyle),
        "small caps: the span parses");
  const TextBlock* block = fixture.firstLineBlock();
  check(block != nullptr, "small caps: the paragraph produced a line");
  if (block == nullptr) return;
  check(block->wordCount() >= 4, "small caps: all four words are on the line");
  if (block->wordCount() < 4) return;
  check((block->wordStyle(0) & EpdFontFamily::SMALL_CAPS) == 0, "small caps: the word before the span is plain");
  check((block->wordStyle(1) & EpdFontFamily::SMALL_CAPS) != 0, "small caps: the first span word is small caps");
  check((block->wordStyle(2) & EpdFontFamily::SMALL_CAPS) != 0, "small caps: the last span word is small caps");
  check((block->wordStyle(3) & EpdFontFamily::SMALL_CAPS) == 0, "small caps: the word after the span is plain");
}

}  // namespace

int main() {
  testPlainParagraphUnchanged();
  testInlineRunMeasuresWithItsFace();
  testUnrunParagraphsKeepTheArena();
  testSerializationRoundTripsSlots();
  testFourthSizeFallsBackToTheBlockFace();
  testFocusAndDecorationsFollowTheRun();
  testInlineSpanSmallCaps();

  if (g_failures == 0) {
    std::printf("layout M5b: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "layout M5b: %d check(s) failed\n", g_failures);
  return 1;
}
