#include "ReaderExtractor.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "HtmlScanner.h"
#include "WebUrl.h"

namespace webbrowser {
namespace {

bool isAsciiSpace(const char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool isVoidTag(const std::string_view name) {
  return name == "area" || name == "base" || name == "br" || name == "col" || name == "embed" ||
         name == "hr" || name == "img" || name == "input" || name == "link" || name == "meta" ||
         name == "param" || name == "source" || name == "track" || name == "wbr";
}

int headingLevel(const std::string_view name) {
  if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') return name[1] - '0';
  return 0;
}

bool isBlockTag(const std::string_view name) {
  return name == "p" || name == "div" || name == "section" || name == "article" || name == "main" ||
         name == "header" || name == "footer" || name == "nav" || name == "aside" || name == "li" ||
         name == "ul" || name == "ol" || name == "dl" || name == "dt" || name == "dd" ||
         name == "blockquote" || name == "pre" || name == "table" || name == "thead" ||
         name == "tbody" || name == "tr" || name == "td" || name == "th" || name == "figure" ||
         name == "figcaption" || name == "details" || name == "summary" || headingLevel(name) != 0;
}

bool isJunkToken(const std::string_view token) {
  static constexpr const char* kJunk[] = {
      "ad",     "ads",        "advert",  "advertisement", "banner", "breadcrumb",
      "share",  "social",     "recommend", "recommended", "sidebar", "side",
      "footer", "header",     "nav",     "navigation",    "menu",    "toolbar",
      "comment", "comments",  "copyright", "pagination",  "pager",   "related",
      "promo",  "popup",      "login",   "register",
  };
  for (const char* junk : kJunk) {
    size_t i = 0;
    while (junk[i] != '\0') ++i;
    if (token.size() != i) continue;
    bool same = true;
    for (size_t k = 0; k < i; ++k) {
      if (std::tolower(static_cast<unsigned char>(token[k])) != junk[k]) {
        same = false;
        break;
      }
    }
    if (same) return true;
  }
  return false;
}

// A class/id attribute is a list of tokens separated by whitespace, '-' or '_'.
// Whole-token matching is what keeps `content-ad` out while `advert` in, and
// avoids `ad` matching inside `shadow`.
bool matchesJunkToken(const std::string_view value) {
  size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && (isAsciiSpace(value[i]) || value[i] == '-' || value[i] == '_')) ++i;
    const size_t start = i;
    while (i < value.size() && !(isAsciiSpace(value[i]) || value[i] == '-' || value[i] == '_')) ++i;
    if (i > start && isJunkToken(value.substr(start, i - start))) return true;
  }
  return false;
}

bool isJunkContainer(const HtmlToken& token) {
  return matchesJunkToken(token.attribute("class")) || matchesJunkToken(token.attribute("id"));
}

struct PendingLink {
  uint32_t relStart = 0;
  uint32_t relLength = 0;
  std::string href;
};

class Extractor {
 public:
  Extractor(Document& doc, const ExtractOptions& options, const std::string_view documentUrl)
      : doc_(doc), options_(options), documentUrl_(documentUrl), base_(documentUrl) {}

  bool run(const std::string_view html) {
    HtmlScanner scanner(html);
    HtmlToken token;
    while (!done_ && scanner.next(token)) {
      switch (token.type) {
        case HtmlTokenType::Text:
          onText(token.text);
          break;
        case HtmlTokenType::StartTag:
          onStart(token);
          break;
        case HtmlTokenType::EndTag:
          onEnd(token);
          break;
        default:
          break;
      }
    }
    flush();
    trimInto(title_, titleBuffer_);
    doc_.title = title_;
    doc_.url = WebString(documentUrl_.data(), documentUrl_.size());
    return !doc_.text.empty();
  }

 private:
  void onStart(const HtmlToken& token) {
    const std::string_view name = token.name;

    if (captureTitle_) return;  // markup inside <title> is ignored
    if (!skipStack_.empty()) {
      // Track nesting by name, not by a bare counter: real pages close a
      // skipped <div> around stray <p></div> sequences, and a counter would
      // end the skip early (leaking chrome) or late (swallowing the article).
      if (!isVoidTag(name) && !token.selfClosing) skipStack_.emplace_back(name);
      return;
    }
    if (inHead_) {
      if (name == "title") {
        captureTitle_ = true;
        titleBuffer_.clear();
      } else if (name == "base") {
        applyBase(token.attribute("href"));
      }
      return;
    }
    if (name == "head") {
      inHead_ = true;
      return;
    }
    if (isSkippedTag(name)) {
      if (!isVoidTag(name) && !token.selfClosing) skipStack_.emplace_back(name);
      return;
    }
    if ((name == "div" || name == "section" || name == "article" || name == "aside" || name == "nav" ||
         name == "header" || name == "footer" || name == "ul" || name == "ol") &&
        isJunkContainer(token)) {
      closeLink();
      flush();
      if (!isVoidTag(name) && !token.selfClosing) skipStack_.emplace_back(name);
      return;
    }

    if (name == "br" || name == "hr") {
      flush();
      return;
    }
    if (name == "a") {
      openLink(token);
      return;
    }
    if (!isBlockTag(name)) return;

    flush();
    const int level = headingLevel(name);
    if (level != 0) {
      currentKind_ = BlockKind::Heading;
      currentLevel_ = static_cast<uint8_t>(level);
    } else if (name == "li") {
      currentKind_ = BlockKind::ListItem;
      currentLevel_ = 0;
    } else if (name == "blockquote") {
      currentKind_ = BlockKind::Quote;
      currentLevel_ = 0;
    } else if (name == "pre") {
      currentKind_ = BlockKind::Pre;
      currentLevel_ = 0;
      ++preDepth_;
    } else {
      currentKind_ = BlockKind::Paragraph;
      currentLevel_ = 0;
    }
  }

  void onEnd(const HtmlToken& token) {
    const std::string_view name = token.name;
    if (captureTitle_) {
      if (name == "title") captureTitle_ = false;
      return;
    }
    if (!skipStack_.empty()) {
      // A close tag pops down to the nearest open tag of that name. Real pages
      // omit </p> and close a div around it; ignoring the mismatch would keep
      // the skip alive and swallow the rest of the article. A close that is
      // not open at all (a stray </span>) is ignored.
      if (!isVoidTag(name)) {
        for (size_t i = skipStack_.size(); i > 0; --i) {
          if (skipStack_[i - 1] == name) {
            skipStack_.resize(i - 1);
            break;
          }
        }
      }
      return;
    }
    if (inHead_) {
      if (name == "head") inHead_ = false;
      return;
    }
    if (name == "a") {
      closeLink();
      return;
    }
    if (name == "pre" && preDepth_ > 0) --preDepth_;
    if (!isBlockTag(name)) return;
    flush();
    currentKind_ = BlockKind::Paragraph;
    currentLevel_ = 0;
  }

  void onText(const std::string_view text) {
    if (captureTitle_) {
      for (const char c : text) {
        if (isAsciiSpace(c)) {
          if (!titleBuffer_.empty() && titleBuffer_.back() != ' ') titleBuffer_.push_back(' ');
        } else {
          titleBuffer_.push_back(c);
        }
      }
      return;
    }
    if (inHead_ || !skipStack_.empty()) return;
    const size_t room = options_.maxTextBytes > pending_.size() ? options_.maxTextBytes - pending_.size() : 0;
    if (text.size() > room) {
      appendText(text.substr(0, room), preDepth_ > 0);
      done_ = true;
      doc_.truncated = true;
      return;
    }
    appendText(text, preDepth_ > 0);
  }

  void openLink(const HtmlToken& token) {
    if (inLink_) return;
    const std::string_view href = token.attribute("href");
    if (href.empty()) return;
    // Two anchors written back to back (`</a><a>`) would otherwise render as
    // one run ("下一章上一章") with a single merged underline. Text between
    // them still glues the run, as in `点击<a>这里</a>继续`.
    if (lastWasLinkEnd_ && !pending_.empty() && pending_.back() != ' ') pending_.push_back(' ');
    std::string resolved;
    bool sameDocument = false;
    if (!resolveHref(base_, href, resolved, sameDocument) || sameDocument || resolved.empty()) return;
    inLink_ = true;
    linkStart_ = static_cast<uint32_t>(pending_.size());
    linkHref_ = std::move(resolved);
  }

  void closeLink() {
    if (!inLink_) return;
    inLink_ = false;
    lastWasLinkEnd_ = true;
    if (pending_.size() > linkStart_) {
      pendingLinks_.push_back(
          PendingLink{linkStart_, static_cast<uint32_t>(pending_.size()) - linkStart_, linkHref_});
    }
    linkHref_.clear();
  }

  void appendText(const std::string_view text, const bool pre) {
    if (!text.empty()) lastWasLinkEnd_ = false;
    if (pre) {
      for (const char c : text) {
        if (c == '\r') continue;
        pending_.push_back(c);
      }
      return;
    }
    for (const char c : text) {
      if (isAsciiSpace(c)) {
        if (!pending_.empty() && pending_.back() != ' ') pending_.push_back(' ');
      } else {
        pending_.push_back(c);
      }
    }
  }

  void flush() {
    closeLink();
    if (pending_.empty()) {
      pendingLinks_.clear();
      return;
    }

    size_t lead = 0;
    size_t trail = pending_.size();
    if (currentKind_ == BlockKind::Pre) {
      while (lead < trail && (pending_[lead] == ' ' || pending_[lead] == '\n')) ++lead;
      while (trail > lead && (pending_[trail - 1] == ' ' || pending_[trail - 1] == '\n')) --trail;
    } else {
      while (lead < trail && pending_[lead] == ' ') ++lead;
      while (trail > lead && pending_[trail - 1] == ' ') --trail;
    }
    if (trail <= lead) {
      pending_.clear();
      pendingLinks_.clear();
      return;
    }

    const size_t requested = trail - lead;
    const size_t available =
        options_.maxTextBytes > doc_.text.size() ? options_.maxTextBytes - doc_.text.size() : 0;
    // U+00B7 + space is three bytes; the length below must match exactly or the
    // block range clips the first byte of the item text.
    // No bullets: novel sites wrap an entire chapter in one <li> with <br>
    // separators, and even navigation lists read fine as plain lines.
    const uint32_t bullet = 0;
    const uint32_t blockStart = static_cast<uint32_t>(doc_.text.size());
    size_t bodyLength = requested;
    bool truncated = false;
    const size_t roomForBody = available > bullet ? available - bullet : 0;
    if (bodyLength > roomForBody) {
      bodyLength = roomForBody;
      truncated = true;
    }
    if (doc_.blocks.size() >= options_.maxBlocks) {
      bodyLength = 0;
      truncated = true;
    }
    if (bodyLength == 0) {
      doc_.truncated = true;
      done_ = true;
      pending_.clear();
      pendingLinks_.clear();
      if (bullet != 0) doc_.text.resize(doc_.text.size() - bullet);
      return;
    }
    if (truncated) {
      doc_.truncated = true;
      done_ = true;
    }
    doc_.text.append(pending_.data() + lead, bodyLength);
    Block block;
    block.kind = currentKind_;
    block.level = currentLevel_;
    block.textStart = blockStart;
    block.textLength = bullet + static_cast<uint32_t>(bodyLength);
    doc_.blocks.push_back(block);

    for (const auto& link : pendingLinks_) {
      if (doc_.links.size() >= options_.maxLinks) {
        doc_.truncated = true;
        break;
      }
      const uint32_t linkBegin = link.relStart;
      const uint32_t linkEnd = link.relStart + link.relLength;
      const uint32_t begin = std::max<uint32_t>(linkBegin, static_cast<uint32_t>(lead));
      const uint32_t end = std::min<uint32_t>(linkEnd, static_cast<uint32_t>(lead + bodyLength));
      if (begin >= end) continue;
      const uint32_t hrefStart = static_cast<uint32_t>(doc_.hrefs.size());
      doc_.hrefs.append(link.href.data(), link.href.size());
      Link entry;
      entry.textStart = blockStart + bullet + (begin - static_cast<uint32_t>(lead));
      entry.textLength = end - begin;
      entry.hrefStart = hrefStart;
      entry.hrefLength = static_cast<uint32_t>(link.href.size());
      doc_.links.push_back(entry);
    }
    pendingLinks_.clear();
    pending_.clear();
  }

  void applyBase(const std::string_view href) {
    if (href.empty()) return;
    std::string resolved;
    bool sameDocument = false;
    if (resolveHref(documentUrl_, href, resolved, sameDocument) && !sameDocument && !resolved.empty()) {
      base_ = std::move(resolved);
    }
  }

  static void trimInto(WebString& out, std::string& buffer) {
    size_t lead = 0;
    size_t trail = buffer.size();
    while (lead < trail && isAsciiSpace(buffer[lead])) ++lead;
    while (trail > lead && isAsciiSpace(buffer[trail - 1])) --trail;
    out.assign(buffer.data() + lead, trail - lead);
  }

  Document& doc_;
  const ExtractOptions& options_;
  std::string documentUrl_;
  std::string base_;

  std::string pending_;
  std::vector<PendingLink> pendingLinks_;
  BlockKind currentKind_ = BlockKind::Paragraph;
  uint8_t currentLevel_ = 0;
  int preDepth_ = 0;
  std::vector<std::string> skipStack_;
  bool inHead_ = false;
  bool captureTitle_ = false;
  bool inLink_ = false;
  bool lastWasLinkEnd_ = false;
  uint32_t linkStart_ = 0;
  std::string linkHref_;
  std::string titleBuffer_;
  WebString title_;
  bool done_ = false;
};

}  // namespace

bool isSkippedTag(const std::string_view name) {
  return name == "script" || name == "style" || name == "noscript" || name == "template" ||
         name == "svg" || name == "canvas" || name == "iframe" || name == "object" ||
         name == "embed" || name == "select" || name == "option" || name == "textarea" ||
         name == "input" || name == "button" || name == "form";
}

bool extractDocument(const std::string_view html, const std::string_view documentUrl, Document& out,
                     const ExtractOptions& options) {
  out.clear();
  Extractor extractor(out, options, documentUrl);
  return extractor.run(html);
}

}  // namespace webbrowser
