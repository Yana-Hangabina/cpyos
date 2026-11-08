// Host regression for the CSS cascade absorbed from upstream 0305aaa6 (layout milestone M1):
// compound (#id / multiple classes) selectors, descendant and child combinators, specificity
// ordering with source-order tiebreak, ::first-letter storage, the newly parsed properties
// (font-size / small-caps / page-break-* / white-space) and the preserved-whitespace helper.
//
// No cmake, no gtest, no firmware toolchain: compiles the real lib/Epub/Epub/css/CssParser.cpp
// against the CSS test's file-backed HAL stubs. `run_css_cascade_test.sh` also runs two mutation
// builds that must turn this suite red.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "CssParser.h"
#include "TextWhitespace.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

bool nearly(const float a, const float b) { return std::fabs(a - b) < 0.001f; }

const char* kCss =
    // Cascade distances: element < class < element.class < #id.
    "p { padding-top: 1em }\n"
    ".k { padding-top: 2em }\n"
    "p.k { padding-top: 3em }\n"
    "#i { padding-top: 4em }\n"
    // Equal-specificity source-order tiebreak; class attribute order must not matter.
    ".a { margin-top: 1em }\n"
    ".b { margin-top: 2em }\n"
    ".c { margin-bottom: 3em }\n"
    ".d { margin-bottom: 4em }\n"
    // Combinators.
    ".chapter p { text-align: justify }\n"
    "div.body > p { text-indent: 2em }\n"
    "div .x { margin-right: 3em }\n"
    // Context adds specificity: the earlier contextual rule must beat the later plain class.
    ".x { margin-right: 5em }\n"
    // Compound selectors.
    "p#lead { font-weight: bold }\n"
    "p.a.b { margin-left: 1em }\n"
    // Pseudo-element storage.
    "p::first-letter { font-style: italic }\n"
    ".x::first-letter { font-style: italic; font-weight: bold }\n"
    // New properties (parsed, not applied yet).
    "pre.shown { white-space: pre }\n"
    ".break { page-break-before: always }\n"
    ".small { font-variant: small-caps; font-size: 1.5em }\n"
    // Unsupported selectors must be dropped whole.
    "p + p { padding-left: 9em }\n"
    "p[lang] { padding-left: 9em }\n"
    "p:hover { padding-left: 9em }\n";

CssAncestor ancestor(const char* tag, const char* classes = "", const char* id = "") {
  return CssParser::makeAncestor(tag, classes, id);
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "crosspoint_css_cascade_host";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string cssPath = (dir / "book.css").string();
  {
    std::ofstream out(cssPath, std::ios::binary);
    out << kCss;
  }

  CssParser parser(dir.string());
  {
    HalFile source;
    if (!source.open(cssPath.c_str(), "rb")) {
      std::printf("FAIL: cannot open fixture\n");
      return 1;
    }
    check(parser.loadFromStream(source) == CssParser::ParseResult::Complete, "stylesheet parses completely");
  }
  // 18 supported rules; the three unsupported selectors must not be stored.
  check(parser.ruleCount() == 19, "unsupported selectors are dropped");

  // --- specificity ---------------------------------------------------------
  {
    const CssStyle plain = parser.resolveStyle("p", "");
    check(plain.hasPaddingTop() && nearly(plain.paddingTop.value, 1.0f), "element selector");
  }
  {
    const CssStyle klass = parser.resolveStyle("p", "k");
    check(klass.hasPaddingTop() && nearly(klass.paddingTop.value, 3.0f), "element.class beats class and element");
  }
  {
    const CssStyle withId = parser.resolveStyle("p", "k", "i");
    check(withId.hasPaddingTop() && nearly(withId.paddingTop.value, 4.0f), "id beats element.class");
  }

  // --- source order and class-attribute order -----------------------------
  {
    const CssStyle style = parser.resolveStyle("span", "b a");
    check(style.hasMarginTop() && nearly(style.marginTop.value, 2.0f), "later equal-specificity rule wins");
  }
  {
    const CssStyle style = parser.resolveStyle("span", "d c");
    check(style.hasMarginBottom() && nearly(style.marginBottom.value, 4.0f),
          "class attribute order does not decide the cascade");
  }

  // --- combinators --------------------------------------------------------
  {
    const CssAncestor ancestors[] = {ancestor("html"), ancestor("body"), ancestor("div", "chapter")};
    const CssStyle style = parser.resolveStyle("p", "", "", ancestors, 3);
    check(style.hasTextAlign() && style.textAlign == CssTextAlign::Justify, "descendant .chapter p matches");
  }
  {
    const CssAncestor ancestors[] = {ancestor("html"), ancestor("body"), ancestor("div", "body")};
    const CssStyle viaChild = parser.resolveStyle("p", "", "", ancestors, 3);
    check(viaChild.hasTextIndent(), "child div.body > p matches its direct parent");
    const CssStyle wrongClass = parser.resolveStyle("p", "", "", ancestors, 2);
    check(!wrongClass.hasTextIndent(), "child combinator does not match a grandparent");
  }
  {
    const CssAncestor ancestors[] = {ancestor("html"), ancestor("body"), ancestor("div")};
    const CssStyle style = parser.resolveStyle("span", "x", "", ancestors, 3);
    check(style.hasMarginRight() && nearly(style.marginRight.value, 3.0f),
          "descendant div .x outranks the later plain .x");
    check(!parser.resolveStyle("span", "x", "", ancestors, 2).hasMarginRight() ||
              nearly(parser.resolveStyle("span", "x", "", ancestors, 2).marginRight.value, 5.0f),
          "descendant requires an actual ancestor");
  }

  // --- compound selectors -------------------------------------------------
  {
    check(parser.resolveStyle("p", "", "lead").fontWeight == CssFontWeight::Bold, "p#lead matches the id");
    check(!parser.resolveStyle("p", "", "other").hasFontWeight(), "id selector needs the exact id");
    check(parser.resolveStyle("p", "a b").hasMarginLeft(), "p.a.b matches both classes in any order");
    check(!parser.resolveStyle("p", "a").hasMarginLeft(), "p.a.b needs both classes");
  }

  // --- ::first-letter -----------------------------------------------------
  {
    check(parser.hasFirstLetterRules(), "first-letter rules are detected");
    const CssStyle letter = parser.resolveStyle("p", "", "", nullptr, 0, true);
    check(letter.hasFontStyle() && letter.fontStyle == CssFontStyle::Italic, "p::first-letter resolves");
    const CssStyle plain = parser.resolveStyle("p", "");
    check(!plain.hasFontStyle(), "first-letter style does not leak into the element style");
    const CssStyle compound = parser.resolveStyle("span", "x", "", nullptr, 0, true);
    check(compound.hasFontWeight() && compound.fontWeight == CssFontWeight::Bold,
          ".x::first-letter outranks p::first-letter");
  }

  // --- newly parsed properties -------------------------------------------
  {
    const CssStyle pre = parser.resolveStyle("pre", "shown");
    check(pre.hasPreserveWhitespace() && pre.preserveWhitespace, "white-space: pre is stored");
    const CssStyle brk = parser.resolveStyle("div", "break");
    check(brk.hasPageBreakBefore() && brk.pageBreakBefore, "page-break-before: always is stored");
    const CssStyle small = parser.resolveStyle("span", "small");
    check(small.hasSmallCaps() && small.smallCaps, "font-variant: small-caps is stored");
    check(small.hasFontSize() && small.fontSize.unit == CssUnit::Em && nearly(small.fontSize.value, 1.5f),
          "font-size keeps its unit");
    // Parsed-but-not-applied: the values must not touch layout fields.
    check(!small.hasDisplay() && !small.hasMarginTop(), "font-size does not alter layout properties");
  }

  // --- CSS cache round-trip ----------------------------------------------
  {
    check(parser.saveToCache(true), "cache writes");
    CssParser reloaded(dir.string());
    check(reloaded.loadFromCache() == CssParser::CacheLoadResult::Complete, "cache loads");
    check(reloaded.ruleCount() == parser.ruleCount(), "cache restores every rule");
    const CssAncestor ancestors[] = {ancestor("html"), ancestor("body"), ancestor("div", "chapter")};
    const CssStyle context = reloaded.resolveStyle("p", "", "", ancestors, 3);
    check(context.hasTextAlign() && context.textAlign == CssTextAlign::Justify,
          "contextual rule survives the cache");
    const CssStyle small = reloaded.resolveStyle("span", "small");
    check(small.hasSmallCaps() && small.smallCaps, "new flags survive the cache");
    check(small.hasFontSize() && nearly(small.fontSize.value, 1.5f), "font-size survives the cache");
    check(reloaded.resolveStyle("pre", "shown").preserveWhitespace, "white-space survives the cache");
  }

  // --- whitespace helper --------------------------------------------------
  {
    check(textws::normalizeWhitespace("  a \t b\n c  ", false) == "a b c", "normal mode trims and collapses");
    check(textws::normalizeWhitespace("   ", false).empty(), "whitespace-only run collapses to empty");
    check(textws::normalizeWhitespace("  a \t b\n c  ", true) == "  a \t b\n c  ",
          "preserve mode keeps interior spacing");
    check(textws::normalizeWhitespace("a\r\nb", true) == "a\nb", "preserve mode drops carriage returns");
    check(textws::normalizeWhitespace("", true).empty(), "empty input stays empty");
  }

  if (failures == 0) {
    std::printf("css_cascade: all checks passed\n");
    return 0;
  }
  std::printf("css_cascade: %d failure(s)\n", failures);
  return 1;
}
