// Host regression for `white-space` / <pre> handling in the real chapter parser (layout M1).
//
// The parser is driven over a small XHTML file with the repository's parser test stubs: the
// TextBlock stub appends every laid-out token to `laidOutWords`, so the token stream shows
// exactly whether a run was collapsed (HTML default) or preserved (white-space: pre), and
// whether a newline inside <pre> started a new line. No cmake, no gtest, no device.
#include <Arduino.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

extern std::vector<std::string> laidOutWords;

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

const char* kCss =
    ".poem { white-space: pre }\n"
    ".chapter pre { white-space: pre }\n"
    "p { text-align: justify }\n";

const char* kHtml =
    "<html><body>"
    "<p>a   b\n c</p>"
    "<pre>x   y\nz</pre>"
    "<div class=\"poem\">m  n</div>"
    "<div class=\"chapter\"><pre>q  r</pre></div>"
    "<div class=\"poem\"><p>g   h</p></div>"
    "<pre>u\tv</pre>"
    "<pre><span style=\"white-space: normal\">s   t</span>w   z</pre>"
    "<p>after   pre</p>"
    "</body></html>";

bool containsWord(const std::string& expected) {
  for (const auto& word : laidOutWords) {
    if (word == expected) return true;
  }
  std::printf("  (no token \"%s\"; got:", expected.c_str());
  for (const auto& word : laidOutWords) std::printf(" [%s]", word.c_str());
  std::printf(")\n");
  return false;
}

}  // namespace

int main() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "crosspoint_parser_ws";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string htmlPath = (dir / "chapter.xhtml").string();
  const std::string cssPath = (dir / "book.css").string();
  {
    std::ofstream html(htmlPath, std::ios::binary);
    html << kHtml;
    std::ofstream css(cssPath, std::ios::binary);
    css << kCss;
  }

  CssParser cssParser(dir.string());
  {
    HalFile source;
    if (!source.open(cssPath.c_str(), "rb") || cssParser.loadFromStream(source) != CssParser::ParseResult::Complete) {
      std::printf("FAIL: stylesheet did not parse\n");
      return 1;
    }
  }

  GfxRenderer renderer;
  size_t pages = 0;
  ChapterHtmlSlimParser parser{nullptr,
                               htmlPath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               false,
                               0,
                               static_cast<uint8_t>(CssTextAlign::Justify),
                               false,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               false,
                               [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) { ++pages; },
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser,
                               false};

  laidOutWords.clear();
  const bool parsed = parser.parseAndBuildPages();
  parser.abortParse();

  check(parsed, "chapter parses");
  check(pages > 0, "pages are emitted");

  // Default white-space: normal collapses runs into plain word boundaries.
  check(containsWord("a") && containsWord("b") && containsWord("c"), "normal text collapses whitespace");
  check(containsWord("after") && containsWord("pre"), "text after a preserved block is collapsed again");

  // <pre> keeps runs verbatim: one token carries the literal spaces.
  check(containsWord("x   y"), "<pre> keeps a three-space run");
  check(containsWord("z"), "<pre> newline starts a new line");
  check(containsWord("u    v"), "<pre> expands a tab to four spaces");

  // CSS white-space: pre, both directly and through a descendant selector.
  check(containsWord("m  n"), "white-space: pre keeps two spaces");
  check(containsWord("q  r"), "descendant .chapter pre applies white-space");
  check(containsWord("g   h"), "white-space: pre is inherited by a nested block");

  // A nested white-space: normal turns preservation back off inside the span (its run collapses
  // into word boundaries), and preservation resumes when the span closes: `t` joins `w` as one
  // word because inline boundaries do not insert a space, then the three spaces are kept.
  check(containsWord("s"), "white-space: normal inside <pre> collapses again");
  check(containsWord("tw   z"), "preservation resumes after the nested span");

  if (failures == 0) {
    std::printf("parser_whitespace: all checks passed (%zu pages, %zu tokens)\n", pages, laidOutWords.size());
    return 0;
  }
  std::printf("parser_whitespace: %d failure(s)\n", failures);
  return 1;
}
