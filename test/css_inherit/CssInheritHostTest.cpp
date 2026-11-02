// Host regression for CSS `font-weight: inherit` / `font-style: inherit` (upstream 1bd323dd).
//
// Exercises the parts a host test can reach without the chapter parser: value interpretation,
// the cascade (`resolveStyle`), inline styles, and the CSS cache wire format (a cached style
// with value 2 must decode, which is what the version/validation bump is for). The
// parser-side resolution against the parent's emphasis is reasoned through in the report.
#include <cstdio>
#include <filesystem>
#include <string>

#include "CssParser.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

const char* kCss =
    "p { font-weight: inherit; }\n"
    "em { font-style: inherit; }\n"
    ".reset { font-weight: inherit; font-style: inherit; }\n"
    ".bold { font-weight: bold; }\n"
    ".italic { font-style: italic; }\n";

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "crosspoint_css_inherit_host";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string cachePath = dir.string();
  const std::string cssPath = (dir / "book.css").string();
  {
    std::FILE* f = std::fopen(cssPath.c_str(), "wb");
    if (!f) {
      std::printf("FAIL: cannot write %s\n", cssPath.c_str());
      return 1;
    }
    std::fwrite(kCss, 1, std::char_traits<char>::length(kCss), f);
    std::fclose(f);
  }

  CssParser parser(cachePath);
  {
    HalFile css;
    if (!css.open(cssPath.c_str(), "rb")) {
      std::printf("FAIL: cannot open stylesheet\n");
      return 1;
    }
    check(parser.loadFromStream(css) == CssParser::ParseResult::Complete, "stylesheet parses");
  }

  // Cascade: an explicit inherit is preserved, not folded to normal.
  {
    const CssStyle p = parser.resolveStyle("p", "");
    check(p.hasFontWeight() && p.fontWeight == CssFontWeight::Inherit, "p keeps font-weight: inherit");
    const CssStyle em = parser.resolveStyle("em", "");
    check(em.hasFontStyle() && em.fontStyle == CssFontStyle::Inherit, "em keeps font-style: inherit");
    const CssStyle reset = parser.resolveStyle("span", "reset");
    check(reset.fontWeight == CssFontWeight::Inherit, "class keeps font-weight: inherit");
    check(reset.fontStyle == CssFontStyle::Inherit, "class keeps font-style: inherit");
  }

  // Regression: ordinary values still resolve as before.
  {
    const CssStyle bold = parser.resolveStyle("span", "bold");
    check(bold.fontWeight == CssFontWeight::Bold, "font-weight: bold unchanged");
    const CssStyle italic = parser.resolveStyle("span", "italic");
    check(italic.fontStyle == CssFontStyle::Italic, "font-style: italic unchanged");
    const CssStyle plain = parser.resolveStyle("dd", "");
    check(!plain.hasFontWeight() && !plain.hasFontStyle(), "undefined properties stay undefined");
  }

  // Inline styles go through the same interpreters.
  {
    const CssStyle w = CssParser::parseInlineStyle("font-weight: inherit");
    check(w.hasFontWeight() && w.fontWeight == CssFontWeight::Inherit, "inline font-weight: inherit");
    const CssStyle s = CssParser::parseInlineStyle("font-style: inherit");
    check(s.hasFontStyle() && s.fontStyle == CssFontStyle::Inherit, "inline font-style: inherit");
    const CssStyle b = CssParser::parseInlineStyle("font-weight: bold");
    check(b.fontWeight == CssFontWeight::Bold, "inline bold unchanged");
    const CssStyle n = CssParser::parseInlineStyle("font-weight: 500");
    check(n.fontWeight == CssFontWeight::Normal, "inline numeric 500 unchanged");
  }

  // Cache wire format: value 2 must survive encode/decode, and the version bump must make a
  // fresh parser accept the file. A rejected style wire would fail the whole load.
  {
    check(parser.saveToCache(true), "CSS cache writes");
    CssParser cached(cachePath);
    check(cached.loadFromCache() == CssParser::CacheLoadResult::Complete, "CSS cache reloads completely");
    const CssStyle p = cached.resolveStyle("p", "");
    check(p.fontWeight == CssFontWeight::Inherit, "cached p keeps font-weight: inherit");
    const CssStyle reset = cached.resolveStyle("span", "reset");
    check(reset.fontStyle == CssFontStyle::Inherit, "cached class keeps font-style: inherit");
    const CssStyle bold = cached.resolveStyle("span", "bold");
    check(bold.fontWeight == CssFontWeight::Bold, "cached bold unchanged");
  }

  fs::remove_all(dir);
  if (failures == 0) std::printf("PASS: CSS inherit (0 failures)\n");
  return failures == 0 ? 0 : 1;
}
