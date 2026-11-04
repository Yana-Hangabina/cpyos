// Host regression for the browser's reading pipeline: URL resolution, tolerant
// tokenizing, reader-mode extraction and pagination. Runs offline on fixtures.
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "HtmlScanner.h"
#include "ReaderExtractor.h"
#include "TextPager.h"
#include "WebCharset.h"
#include "WebPlaces.h"

#include <TxtEncoding.h>
#include "WebUrl.h"

using namespace webbrowser;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(expr)                                                       \
  do {                                                                    \
    ++checks;                                                             \
    if (!(expr)) {                                                        \
      ++failures;                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                              \
  do {                                                                              \
    ++checks;                                                                       \
    const auto& va = (a);                                                           \
    const auto& vb = (b);                                                           \
    if (!(va == vb)) {                                                              \
      ++failures;                                                                   \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);    \
    }                                                                               \
  } while (0)

std::string slice(const WebString& text, const uint32_t start, const uint32_t length) {
  return std::string(text.data() + start, length);
}

std::string blockText(const Document& doc, const size_t index) {
  const auto& b = doc.blocks[index];
  return slice(doc.text, b.textStart, b.textLength);
}

std::string linkHref(const Document& doc, const size_t index) {
  const auto& l = doc.links[index];
  return std::string(doc.hrefs.data() + l.hrefStart, l.hrefLength);
}

int fakeMeasure(void*, const char* text, const size_t length, bool) {
  if (length == 0) return 0;
  return static_cast<unsigned char>(text[0]) < 0x80 ? 10 : 20;
}

// A measurer whose whole-string result is wider than the sum of its glyph
// widths, the way a real line is (fallback face + space/kerning): the pager's
// per-glyph estimate under-counts and the fix-up pass must split the line.
int fakeMeasureWiderLines(void*, const char* text, const size_t length, bool) {
  if (length == 0) return 0;
  const int glyph = static_cast<unsigned char>(text[0]) < 0x80 ? 10 : 20;
  return glyph * static_cast<int>(length) + (length > 1 ? 20 : 0);
}

void testUrls() {
  std::string out;
  bool upgraded = false;

  CHECK(normalizeHttpUrl("example.com/a", out, upgraded));
  CHECK_EQ(out, std::string("https://example.com/a"));
  CHECK(!upgraded);

  CHECK(normalizeHttpUrl("http://example.com/x", out, upgraded));
  CHECK_EQ(out, std::string("http://example.com/x"));
  CHECK(!upgraded);

  // Resolving against a plain-HTTP base keeps the scheme.
  bool sameDoc = false;
  CHECK(resolveHref("http://example.com/dir/", "b.html", out, sameDoc) && !sameDoc);
  CHECK_EQ(out, std::string("http://example.com/dir/b.html"));

  CHECK(!normalizeHttpUrl("javascript:alert(1)", out, upgraded));
  CHECK(!normalizeHttpUrl("https://例子.com/", out, upgraded));
  CHECK(normalizeHttpUrl("https://a.com/路径", out, upgraded));
  CHECK_EQ(out, std::string("https://a.com/%E8%B7%AF%E5%BE%84"));

  const std::string base = "https://a.com/dir/page.html";
  bool same = false;
  CHECK(resolveHref(base, "b.html", out, same) && !same);
  CHECK_EQ(out, std::string("https://a.com/dir/b.html"));
  CHECK(resolveHref(base, "/x", out, same));
  CHECK_EQ(out, std::string("https://a.com/x"));
  CHECK(resolveHref(base, "../z", out, same));
  CHECK_EQ(out, std::string("https://a.com/z"));
  CHECK(resolveHref(base, "//cdn.a.com/p", out, same));
  CHECK_EQ(out, std::string("https://cdn.a.com/p"));
  CHECK(resolveHref(base, "?p=2", out, same));
  CHECK_EQ(out, std::string("https://a.com/dir/page.html?p=2"));
  CHECK(resolveHref(base, "#top", out, same) && same);
  CHECK(!resolveHref(base, "mailto:a@b.c", out, same));
  CHECK(!resolveHref(base, "javascript:void(0)", out, same));

  CHECK_EQ(hostOf("https://u:p@example.com:8443/x"), std::string("example.com"));
  CHECK_EQ(originOf("https://example.com:8443/x"), std::string("https://example.com:8443"));
}

void testScanner() {
  {
    HtmlScanner scanner("<p class=x data-y=\"1\">hi &amp; bye</p>");
    HtmlToken token;
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::StartTag);
    CHECK_EQ(std::string(token.name), std::string("p"));
    CHECK_EQ(std::string(token.attribute("class")), std::string("x"));
    CHECK_EQ(std::string(token.attribute("data-y")), std::string("1"));
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::Text);
    CHECK_EQ(std::string(token.text), std::string("hi & bye"));
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::EndTag);
    CHECK(!scanner.next(token));
  }
  {
    // Script content is raw: '<' inside it must not open a tag, and the
    // closing tag must end the raw run.
    HtmlScanner scanner("<script>if (a<b) x();</script><p>ok</p>");
    HtmlToken token;
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::StartTag && token.name == "script");
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::Text);
    CHECK_EQ(std::string(token.text), std::string("if (a<b) x();"));
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::EndTag && token.name == "script");
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::StartTag && token.name == "p");
  }
  {
    HtmlScanner scanner("<!-- c --><!doctype html>a < b &#20013;&#x6587;");
    HtmlToken token;
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::Text);
    CHECK_EQ(std::string(token.text), std::string("a < b 中文"));
    CHECK(!scanner.next(token));
  }
  {
    HtmlScanner scanner("<img src=a.png /><br/>");
    HtmlToken token;
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::StartTag && token.selfClosing);
    CHECK_EQ(std::string(token.attribute("src")), std::string("a.png"));
    CHECK(scanner.next(token));
    CHECK(token.type == HtmlTokenType::StartTag && token.name == "br" && token.selfClosing);
  }
}

void testExtraction() {
  const std::string html =
      "<html><head><title> 第一章 &amp; 测试 </title>"
      "<meta charset=\"utf-8\"></head><body>"
      "<div class=\"nav\">导航菜单</div>"
      "<h1>第一章</h1>"
      "<p>正文一&amp;二<a href=\"/next\">下一章</a>尾巴</p>"
      "<script>var x = '<p>no</p>';</script>"
      "<div id=\"comments\">读者评论</div>"
      "<p>第二段<br>换行</p>"
      "<ul><li>条目一</li><li>条目二</li></ul>"
      "</body></html>";
  Document doc;
  CHECK(extractDocument(html, "https://a.com/book/1.html", doc));
  CHECK_EQ(std::string(doc.title.c_str()), std::string("第一章 & 测试"));
  CHECK_EQ(std::string(doc.url.c_str()), std::string("https://a.com/book/1.html"));

  CHECK(doc.blocks.size() >= 5);
  CHECK(doc.blocks[0].kind == BlockKind::Heading);
  CHECK_EQ(static_cast<int>(doc.blocks[0].level), 1);
  CHECK_EQ(blockText(doc, 0), std::string("第一章"));
  CHECK_EQ(blockText(doc, 1), std::string("正文一&二下一章尾巴"));
  CHECK_EQ(blockText(doc, 2), std::string("第二段"));
  CHECK_EQ(blockText(doc, 3), std::string("换行"));
  CHECK(doc.blocks[4].kind == BlockKind::ListItem);
  CHECK_EQ(blockText(doc, 4), std::string("条目一"));
  CHECK_EQ(blockText(doc, 5), std::string("条目二"));

  bool sawNav = false;
  bool sawComment = false;
  bool sawScript = false;
  for (const auto& b : doc.blocks) {
    const std::string text = slice(doc.text, b.textStart, b.textLength);
    if (text.find("导航") != std::string::npos) sawNav = true;
    if (text.find("读者评论") != std::string::npos) sawComment = true;
    if (text.find("var x") != std::string::npos) sawScript = true;
  }
  CHECK(!sawNav);
  CHECK(!sawComment);
  CHECK(!sawScript);

  CHECK_EQ(doc.links.size(), 1u);
  if (doc.links.size() == 1) {
    CHECK_EQ(slice(doc.text, doc.links[0].textStart, doc.links[0].textLength), std::string("下一章"));
    CHECK_EQ(linkHref(doc, 0), std::string("https://a.com/next"));
  }

  // <base href> rewrites link resolution.
  Document based;
  CHECK(extractDocument("<head><base href=\"https://cdn.a.com/x/\"></head><body><a href=\"p.html\">p</a></body>",
                        "https://a.com/", based));
  CHECK_EQ(based.links.size(), 1u);
  if (based.links.size() == 1) CHECK_EQ(linkHref(based, 0), std::string("https://cdn.a.com/x/p.html"));

  // Unbalanced markup inside a skipped chrome container must not swallow the
  // article: the close tag pops to the nearest matching open tag.
  Document broken;
  CHECK(extractDocument("<body><div class=\"nav\"><p>菜单</div><p>正文</p></body>", "https://a.com/", broken));
  CHECK_EQ(broken.blocks.size(), 1u);
  CHECK_EQ(blockText(broken, 0), std::string("正文"));

  // javascript: links are not links, and their text still belongs to the body.
  Document js;
  CHECK(extractDocument("<body><p>前<a href=\"javascript:void(0)\">点我</a>后</p></body>", "https://a.com/", js));
  CHECK_EQ(js.links.size(), 0u);
  CHECK_EQ(blockText(js, 0), std::string("前点我后"));

  // Caps truncate rather than grow without bound.
  Document capped;
  ExtractOptions options;
  options.maxTextBytes = 8;
  extractDocument("<body><p>0123456789ABCDEF</p></body>", "https://a.com/", capped, options);
  CHECK(capped.truncated);
  CHECK(capped.text.size() <= 8);
  CHECK(!capped.text.empty());
}

void testPager() {
  // ASCII: 26 chars, 10 px each, 100 px wide -> 3 lines of 10.
  {
    Document doc;
    doc.text = "abcdefghijklmnopqrstuvwxyz";
    Block b;
    b.textStart = 0;
    b.textLength = 26;
    doc.blocks.push_back(b);

    PagerSpec spec;
    spec.contentWidth = 100;
    spec.lineHeight = 10;
    spec.pageHeight = 100;
    spec.measure = fakeMeasure;
    spec.measureContext = nullptr;
    Pagination pager;
    CHECK(paginate(doc, spec, pager));
    CHECK_EQ(pager.lines.size(), 3u);
    std::string rebuilt;
    for (const auto& line : pager.lines) {
      rebuilt += slice(doc.text, line.textStart, line.textLength);
      CHECK(line.textLength <= 10);
    }
    CHECK_EQ(rebuilt, std::string("abcdefghijklmnopqrstuvwxyz"));
    CHECK_EQ(pager.pages.size(), 1u);
  }

  // CJK: 20 px each, 100 px wide -> 5 per line, and no line may start with '，'.
  {
    Document doc;
    doc.text = "中文中文，中文中文中文";
    Block b;
    b.textStart = 0;
    b.textLength = static_cast<uint32_t>(doc.text.size());
    doc.blocks.push_back(b);

    PagerSpec spec;
    // 90 px: the closing '，' itself no longer fits after four ideographs, so a
    // pager that allowed a break before it would start the next line with it.
    spec.contentWidth = 90;
    spec.lineHeight = 10;
    spec.pageHeight = 60;
    spec.measure = fakeMeasure;
    Pagination pager;
    CHECK(paginate(doc, spec, pager));
    CHECK(pager.lines.size() >= 3);
    for (const auto& line : pager.lines) {
      const std::string text = slice(doc.text, line.textStart, line.textLength);
      CHECK(!text.empty());
      CHECK(text[0] != "，"[0]);  // first byte of '，' is 0xEF
      CHECK(static_cast<int>(line.textLength / 3) <= 5);
    }
    std::string rebuilt;
    for (const auto& line : pager.lines) rebuilt += slice(doc.text, line.textStart, line.textLength);
    CHECK_EQ(rebuilt, std::string("中文中文，中文中文中文"));
    for (const auto& page : pager.pages) {
      CHECK(page.lineCount > 0);
      for (uint32_t i = 0; i < page.lineCount; ++i) {
        const auto& line = pager.lines[page.firstLine + i];
        CHECK(line.y - page.yOffset + spec.lineHeight <= spec.pageHeight);
      }
    }
  }

  // A single unbreakable word is force-broken instead of overflowing.
  {
    Document doc;
    doc.text = "aaaaaaaaaaaaaaaaaaaa";
    Block b;
    b.textStart = 0;
    b.textLength = static_cast<uint32_t>(doc.text.size());
    doc.blocks.push_back(b);
    PagerSpec spec;
    spec.contentWidth = 50;
    spec.lineHeight = 10;
    spec.pageHeight = 100;
    spec.measure = fakeMeasure;
    Pagination pager;
    CHECK(paginate(doc, spec, pager));
    CHECK_EQ(pager.lines.size(), 4u);
    for (const auto& line : pager.lines) CHECK(line.textLength <= 5);
  }

  // Whole-line verification: when the per-glyph sum under-counts, the fix-up
  // pass must split ranges until the renderer's own measurement fits.
  {
    Document doc;
    doc.text = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    Block b;
    b.textStart = 0;
    b.textLength = static_cast<uint32_t>(doc.text.size());
    doc.blocks.push_back(b);
    PagerSpec spec;
    spec.contentWidth = 100;
    spec.lineHeight = 10;
    spec.pageHeight = 100;
    spec.measure = fakeMeasureWiderLines;
    spec.verifyWholeLines = true;
    Pagination pager;
    CHECK(paginate(doc, spec, pager));
    CHECK(pager.lines.size() > 3u);
    for (const auto& line : pager.lines) {
      const std::string text = slice(doc.text, line.textStart, line.textLength);
      CHECK(fakeMeasureWiderLines(nullptr, text.data(), text.size(), false) <= spec.contentWidth);
    }
    std::string rebuilt;
    for (const auto& line : pager.lines) rebuilt += slice(doc.text, line.textStart, line.textLength);
    CHECK_EQ(rebuilt, doc.text);
  }

  // Pre keeps blank lines.
  {
    Document doc;
    doc.text = "a\n\nb";
    Block b;
    b.textStart = 0;
    b.textLength = static_cast<uint32_t>(doc.text.size());
    b.kind = BlockKind::Pre;
    doc.blocks.push_back(b);
    PagerSpec spec;
    spec.contentWidth = 100;
    spec.lineHeight = 10;
    spec.pageHeight = 100;
    spec.measure = fakeMeasure;
    Pagination pager;
    CHECK(paginate(doc, spec, pager));
    CHECK_EQ(pager.lines.size(), 3u);
    CHECK_EQ(pager.lines[1].textLength, 0u);
  }
}

void testCharset() {
  CHECK_EQ(sniffCharset("", "text/html; charset=GBK"), std::string("gbk"));
  CHECK_EQ(sniffCharset("<meta charset=\"gb2312\">", ""), std::string("gb2312"));
  CHECK_EQ(sniffCharset("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=gb18030\">", ""),
           std::string("gb18030"));
  CHECK_EQ(sniffCharset("<html><head></head>", ""), std::string());
  CHECK(isGbkCharset("gbk"));
  CHECK(isGbkCharset("gb18030"));
  CHECK(!isGbkCharset("utf-8"));
}

}  // namespace

// The device path for a Chinese novel site: GBK bytes -> detect ->
// transcodeInPlace -> tokenize -> paginate. encodeGbkFromUtf8() supplies real
// GBK bytes, so no binary fixture is checked in.
void testGbkPipeline() {
  const std::string html =
      "<html><head><meta charset=\"gbk\"><title>第一章</title></head>"
      "<body><p>韩立站在山巅，风很大。</p></body></html>";
  uint8_t gbk[2048];
  size_t gbkLength = 0;
  CHECK(txt_encoding::encodeGbkFromUtf8(html.data(), html.size(), gbk, sizeof(gbk), gbkLength));
  CHECK(gbkLength > 0);
  CHECK_EQ(sniffCharset(std::string_view(reinterpret_cast<const char*>(gbk), 200), "text/html"),
           std::string("gbk"));
  CHECK(txt_encoding::detect(gbk, gbkLength, true) == txt_encoding::Encoding::Gbk);

  uint8_t buffer[4096];
  memcpy(buffer, gbk, gbkLength);
  const auto transcoded =
      txt_encoding::transcodeInPlace(buffer, gbkLength, sizeof(buffer), true, txt_encoding::Encoding::Gbk);
  CHECK(transcoded.utf8Length > 0);

  Document doc;
  CHECK(extractDocument(std::string_view(reinterpret_cast<const char*>(buffer), transcoded.utf8Length),
                        "https://a.com/book/1.html", doc));
  CHECK_EQ(std::string(doc.title.c_str()), std::string("第一章"));
  CHECK_EQ(doc.blocks.size(), 1u);
  CHECK_EQ(blockText(doc, 0), std::string("韩立站在山巅，风很大。"));

  PagerSpec spec;
  spec.contentWidth = 120;
  spec.pageHeight = 30;
  spec.lineHeight = 10;
  spec.measure = fakeMeasure;
  Pagination pager;
  CHECK(paginate(doc, spec, pager));
  CHECK(!pager.lines.empty());
}

std::string viewOf(const WebString& text) { return std::string(text.c_str()); }

void testPlaces() {
  // Bookmarks: same URL refreshes and moves to the front.
  WebVector<Place> bookmarks;
  Place first;
  first.url = "https://a.com/1";
  first.title = "One\tTabbed\nTitle";
  first.visitedAt = 100;
  first.page = 3;
  addBookmark(bookmarks, first);
  addBookmark(bookmarks, first);
  CHECK_EQ(bookmarks.size(), 1u);
  Place second;
  second.url = "https://a.com/2";
  second.title = "Two";
  second.visitedAt = 200;
  addBookmark(bookmarks, second);
  CHECK_EQ(bookmarks.size(), 2u);
  CHECK_EQ(viewOf(bookmarks[0].url), std::string("https://a.com/2"));
  CHECK_EQ(findBookmark(bookmarks, "https://a.com/1"), 1);
  toggleBookmark(bookmarks, first);
  CHECK_EQ(findBookmark(bookmarks, "https://a.com/1"), -1);
  toggleBookmark(bookmarks, first);
  CHECK(findBookmark(bookmarks, "https://a.com/1") >= 0);

  // Cap.
  for (int i = 0; i < 80; ++i) {
    Place p;
    char url[64];
    snprintf(url, sizeof(url), "https://cap/%d", i);
    p.url = url;
    p.title = "t";
    addBookmark(bookmarks, p);
  }
  CHECK_EQ(bookmarks.size(), kMaxBookmarks);

  // History is MRU and unique by URL.
  WebVector<Place> history;
  recordHistory(history, second);
  recordHistory(history, first);
  recordHistory(history, second);
  CHECK_EQ(history.size(), 2u);
  CHECK_EQ(viewOf(history[0].url), std::string("https://a.com/2"));

  // Round trip (fields sanitised; unicode title preserved).
  Place unicode;
  unicode.url = "https://a.com/中文";
  unicode.title = "第一章\t测试";
  unicode.visitedAt = 123456;
  unicode.page = 7;
  WebVector<Place> saved;
  saved.push_back(unicode);
  const WebString serialized = serializePlaces(saved);
  WebVector<Place> loaded;
  CHECK(deserializePlaces(std::string(serialized.c_str(), serialized.size()), loaded));
  CHECK_EQ(loaded.size(), 1u);
  CHECK_EQ(viewOf(loaded[0].url), std::string("https://a.com/中文"));
  CHECK_EQ(viewOf(loaded[0].title), std::string("第一章 测试"));
  CHECK_EQ(loaded[0].visitedAt, 123456u);
  CHECK_EQ(static_cast<int>(loaded[0].page), 7);

  // Malformed input is rejected or skipped, never trusted.
  WebVector<Place> bad;
  CHECK(!deserializePlaces("NOPE\n", bad));
  CHECK(deserializePlaces("WPL1\t1\nhttps://ok\ttitle\t1\t0\nbroken-line\n", bad));
  CHECK_EQ(bad.size(), 1u);
  CHECK(deserializePlaces("", bad));
  CHECK_EQ(bad.size(), 0u);
}

void testAdjacentLinks() {
  // Two anchors written back to back must not render as one glued run; the
  // extractor inserts one thin gap so each keeps its own underline.
  Document doc;
  CHECK(extractDocument("<body><p><a href=\"/next\">下一章</a><a href=\"/prev\">上一章</a></p></body>",
                        "https://a.com/b/1", doc));
  if (!doc.blocks.empty()) CHECK_EQ(blockText(doc, 0), std::string("下一章 上一章"));
  CHECK_EQ(doc.links.size(), 2u);
  if (doc.links.size() == 2) {
    CHECK_EQ(slice(doc.text, doc.links[0].textStart, doc.links[0].textLength), std::string("下一章"));
    CHECK_EQ(slice(doc.text, doc.links[1].textStart, doc.links[1].textLength), std::string("上一章"));
  }

  // Text between two anchors glues the run: no gaps on either side.
  Document glued;
  CHECK(extractDocument("<body><p>前<a href=\"/a\">点</a>后</p></body>", "https://a.com/", glued));
  if (!glued.blocks.empty()) CHECK_EQ(blockText(glued, 0), std::string("前点后"));

  // A line break between anchors belongs to the block structure; the next
  // block starts with its own link and has no leading space.
  Document blocks;
  CHECK(extractDocument("<body><p><a href=\"/a\">甲</a></p><p><a href=\"/b\">乙</a></p></body>",
                        "https://a.com/", blocks));
  CHECK_EQ(blockText(blocks, 0), std::string("甲"));
  CHECK_EQ(blockText(blocks, 1), std::string("乙"));
}

int main() {
  testUrls();
  testScanner();
  testExtraction();
  testAdjacentLinks();
  testPager();
  testCharset();
  testGbkPipeline();
  testPlaces();
  if (failures != 0) {
    std::fprintf(stderr, "%d/%d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("webbrowser host tests passed: %d checks\n", checks);
  return 0;
}
