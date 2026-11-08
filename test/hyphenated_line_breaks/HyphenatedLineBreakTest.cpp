// Host regression for the hyphenated line breaker in ParsedText.
//
// Upstream cb2a4c78 (2.0.0) fixed the same class of bug in its own breaker: the
// greedy hyphenation path measured lines without the ruby overhang and without
// kinsoku hanging, so a paragraph could wrap differently depending on whether
// hyphenation was on, and ruby could be pushed past the page edge.
//
// CPYOS's DP breaker (computeLineBreaks) already applied both rules. The check
// here is therefore structural and path-independent: for text that contains no
// hyphenation points, the two breakers must produce identical lines. A second
// group pins the ruby cases where the greedy path used to differ, so a mutant
// that drops either reservation goes red.
//
// Fixture metrics come from test/chapter_html_slim_parser/stubs: every byte is
// 8 px wide, a space 4 px, kerning zero. No font, framebuffer or SD card.
#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using Words = std::vector<std::string>;
using Lines = std::vector<Words>;

struct Case {
  const char* name;
  std::vector<const char*> words;
  int rubyWord = -1;
  std::string ruby;
  uint16_t width = 100;
};

Lines layout(const Case& c, const bool hyphenation) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = false;
  ParsedText text(false, FirstLineIndent::NoIndent, hyphenation, false, style);
  for (const char* word : c.words) text.addWord(word, EpdFontFamily::REGULAR);
  if (c.rubyWord >= 0) text.setRubyForWordAt(static_cast<size_t>(c.rubyWord), c.ruby);
  Lines lines;
  text.layoutAndExtractLines(renderer, 0, c.width, [&](std::unique_ptr<TextBlock> block, uint32_t, bool) {
    Words line;
    for (uint16_t i = 0; i < block->wordCount(); ++i) line.emplace_back(block->wordText(i));
    lines.push_back(std::move(line));
    return true;
  });
  return lines;
}

std::string describe(const Lines& lines) {
  std::string out;
  for (const auto& line : lines) {
    out += " |";
    for (const auto& word : line) {
      out += ' ';
      out += word;
    }
  }
  return out;
}

int failures = 0;

void checkEqual(const char* name, const Lines& actual, const Lines& expected) {
  const std::string a = describe(actual);
  const std::string e = describe(expected);
  if (a == e) {
    printf("ok   %s:%s\n", name, a.c_str());
    return;
  }
  ++failures;
  printf("FAIL %s\n  expected:%s\n  actual:  %s\n", name, e.c_str(), a.c_str());
}

// The invariant this file exists for: no hyphenation points -> both breakers agree.
void checkPathsAgree(const Case& c) {
  const Lines hyphenated = layout(c, true);
  const Lines plain = layout(c, false);
  if (describe(hyphenated) == describe(plain)) {
    printf("ok   paths agree (%s):%s\n", c.name, describe(hyphenated).c_str());
    return;
  }
  ++failures;
  printf("FAIL paths disagree (%s)\n  hyphenated:%s\n  plain:     %s\n", c.name, describe(hyphenated).c_str(),
         describe(plain).c_str());
}

void expectLines(const char* name, const Case& c, const Lines& expected) {
  checkEqual(name, layout(c, true), expected);
}

}  // namespace

int main() {
  Hyphenator::setPreferredLanguage("en");

  // Ordinary hyphenated text: unchanged wrapping (regression pin).
  {
    Case c{"plain", {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot"}};
    expectLines("plain", c, {{"alpha", "bravo"}, {"charlie", "delta"}, {"echo", "foxtrot"}});
    checkPathsAgree(c);
  }

  // Ruby wider than its base on the line's FIRST word: the right half of the
  // overhang is reserved as an indent, so one fewer word fits (the greedy path
  // used to hyphenate "delta" to fill the line).
  {
    Case c{"ruby-at-line-start", {"bravo", "delta", "echo", "foxtrot"}, 0, "RRRRRRRR"};
    expectLines("ruby-at-line-start", c, {{"bravo"}, {"delta", "echo"}, {"foxtrot"}});
    checkPathsAgree(c);
  }

  // Ruby wider than its base on the word that would END the line: keeping it
  // there would push the ruby past the right margin, so the line ends earlier.
  {
    Case c{"ruby-at-line-end", {"bravo", "delta", "echo"}, 1, "RRRRRRRR"};
    expectLines("ruby-at-line-end", c, {{"bravo"}, {"delta", "echo"}});
    checkPathsAgree(c);
  }

  // CJK with a closing sentence mark and ruby: the DP path is the reference.
  {
    Case c{"cjk-marks", {"这是一个测试。", "另外一句，", "最后一段文字"}, -1, ""};
    checkPathsAgree(c);
  }
  {
    Case c{"cjk-ruby", {"这是一个测试。", "另外一句，", "最后一段文字"}, 0, "ZHESHIYIGECESHI"};
    checkPathsAgree(c);
  }

  if (failures != 0) {
    printf("\n%d failure(s)\n", failures);
    return 1;
  }
  printf("\nall checks passed\n");
  return 0;
}
