#include "JjwxcDiscover.h"

#include <stdio.h>
#include <string.h>

namespace jjwxc {
namespace {

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool ciMatch(const char* text, const size_t len, const char* lower) {
  const size_t n = strlen(lower);
  if (n > len) return false;
  for (size_t i = 0; i < n; ++i) {
    if (asciiLower(text[i]) != asciiLower(lower[i])) return false;
  }
  return true;
}

// Case-insensitive search for a lowercase needle. Returns a pointer into `text`.
const char* ciFind(const char* text, const size_t len, const char* lower) {
  const size_t n = strlen(lower);
  if (n == 0 || n > len) return nullptr;
  for (size_t i = 0; i + n <= len; ++i) {
    if (ciMatch(text + i, len - i, lower)) return text + i;
  }
  return nullptr;
}

// Index just past the '>' that closes the tag starting at `from` (which must point
// at '<'). Quotes are honoured, because attributes carry '>' inside JSON.
size_t tagClose(const char* html, const size_t len, const size_t from) {
  char quote = 0;
  for (size_t i = from + 1; i < len; ++i) {
    const char c = html[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i + 1;
    }
  }
  return len;
}

// Reads an attribute out of one tag. `tag`/`tagLen` describe the whole `<...>`
// span. Case-insensitive name, accepts single/double/unquoted values. Returns
// false when the attribute is absent or does not fit -- a truncated value is
// never handed back as if it were complete.
bool attrValue(const char* tag, const size_t tagLen, const char* lowerName, char* out, const size_t outCap) {
  if (out == nullptr || outCap == 0) return false;
  out[0] = '\0';
  const size_t nameLen = strlen(lowerName);
  for (size_t i = 1; i + nameLen < tagLen; ++i) {
    const char prev = tag[i - 1];
    if (!(prev == '<' || isSpace(prev) || prev == '/')) continue;
    if (!ciMatch(tag + i, tagLen - i, lowerName)) continue;
    size_t j = i + nameLen;
    while (j < tagLen && isSpace(tag[j])) ++j;
    if (j >= tagLen || tag[j] != '=') continue;
    ++j;
    while (j < tagLen && isSpace(tag[j])) ++j;
    char quote = 0;
    if (j < tagLen && (tag[j] == '"' || tag[j] == '\'')) {
      quote = tag[j];
      ++j;
    }
    const size_t begin = j;
    if (quote != 0) {
      while (j < tagLen && tag[j] != quote) ++j;
    } else {
      while (j < tagLen && !isSpace(tag[j]) && tag[j] != '>') ++j;
    }
    const size_t n = j - begin;
    if (n + 1 > outCap) return false;
    memcpy(out, tag + begin, n);
    out[n] = '\0';
    return true;
  }
  return false;
}

// Appends one UTF-8 sequence whole or not at all, so a truncated title never ends
// inside a code point (the renderer measures advances, not bytes).
void appendBytes(char* out, const size_t cap, size_t& used, const char* s, const size_t n) {
  if (used + n >= cap) return;
  memcpy(out + used, s, n);
  used += n;
  out[used] = '\0';
}

// Decodes the entities JJWXC actually emits in titles. Unknown entities are kept
// verbatim rather than dropped: a title that shows "&foo;" is visibly odd, while
// one that silently loses text reads as a different book.
bool decodeEntity(const char* s, const size_t len, size_t& consumed, char* out, const size_t cap, size_t& used) {
  consumed = 0;
  if (len < 3 || s[0] != '&') return false;
  const size_t semi = [&] {
    for (size_t i = 1; i < len && i < 12; ++i) {
      if (s[i] == ';') return i;
    }
    return static_cast<size_t>(0);
  }();
  if (semi == 0) return false;
  const char* body = s + 1;
  const size_t bodyLen = semi - 1;
  auto emit = [&](const char* text) {
    appendBytes(out, cap, used, text, strlen(text));
    consumed = semi + 1;
    return true;
  };
  if (bodyLen == 3 && ciMatch(body, bodyLen, "amp")) return emit("&");
  if (bodyLen == 2 && ciMatch(body, bodyLen, "lt")) return emit("<");
  if (bodyLen == 2 && ciMatch(body, bodyLen, "gt")) return emit(">");
  if (bodyLen == 4 && ciMatch(body, bodyLen, "quot")) return emit("\"");
  if (bodyLen == 4 && ciMatch(body, bodyLen, "apos")) return emit("'");
  if (bodyLen == 4 && ciMatch(body, bodyLen, "nbsp")) return emit(" ");
  if (bodyLen >= 2 && body[0] == '#') {
    uint32_t cp = 0;
    size_t i = 1;
    if (body[1] == 'x' || body[1] == 'X') {
      i = 2;
      for (; i < bodyLen; ++i) {
        const char c = body[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
        else if (asciiLower(c) >= 'a' && asciiLower(c) <= 'f') digit = static_cast<uint32_t>(asciiLower(c) - 'a' + 10);
        else return false;
        cp = cp * 16 + digit;
        if (cp > 0x10FFFF) return false;
      }
    } else {
      for (; i < bodyLen; ++i) {
        if (body[i] < '0' || body[i] > '9') return false;
        cp = cp * 10 + static_cast<uint32_t>(body[i] - '0');
        if (cp > 0x10FFFF) return false;
      }
    }
    if (cp == 0) return false;
    char utf8[4];
    size_t n = 0;
    if (cp < 0x80) {
      utf8[n++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
      utf8[n++] = static_cast<char>(0xC0 | (cp >> 6));
      utf8[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      utf8[n++] = static_cast<char>(0xE0 | (cp >> 12));
      utf8[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      utf8[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      utf8[n++] = static_cast<char>(0xF0 | (cp >> 18));
      utf8[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      utf8[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      utf8[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    appendBytes(out, cap, used, utf8, n);
    consumed = semi + 1;
    return true;
  }
  return false;
}

// Visible text of an HTML fragment: tags dropped, entities decoded, whitespace
// runs collapsed. Always NUL-terminates; returns the number of bytes written.
size_t extractText(const char* html, const size_t len, char* out, const size_t cap) {
  size_t used = 0;
  if (out == nullptr || cap == 0) return 0;
  out[0] = '\0';
  bool lastSpace = true;  // trims leading whitespace
  size_t i = 0;
  while (i < len) {
    const char c = html[i];
    if (c == '<') {
      if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
        const char* end = ciFind(html + i, len - i, "-->");
        i = end ? static_cast<size_t>(end - html) + 3 : len;
        continue;
      }
      i = tagClose(html, len, i);
      lastSpace = true;
      continue;
    }
    if (c == '&') {
      size_t consumed = 0;
      if (decodeEntity(html + i, len - i, consumed, out, cap, used)) {
        i += consumed;
        lastSpace = used > 0 && out[used - 1] == ' ';
        continue;
      }
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
      if (!lastSpace) {
        if (used + 1 < cap) {
          out[used++] = ' ';
          out[used] = '\0';
        }
        lastSpace = true;
      }
      ++i;
      continue;
    }
    const unsigned char uc = static_cast<unsigned char>(c);
    size_t seq = 1;
    if ((uc & 0xE0) == 0xC0) seq = 2;
    else if ((uc & 0xF0) == 0xE0) seq = 3;
    else if ((uc & 0xF8) == 0xF0) seq = 4;
    if (i + seq > len) seq = 1;
    appendBytes(out, cap, used, html + i, seq);
    lastSpace = false;
    i += seq;
  }
  while (used > 0 && out[used - 1] == ' ') out[--used] = '\0';
  return used;
}

struct RowCandidate {
  char novelId[16] = {};
  char title[96] = {};
  char author[48] = {};
  int score = -1;
};

bool bracketOnly(const char* text) {
  const size_t n = strlen(text);
  if (n < 2) return false;
  const unsigned char a = static_cast<unsigned char>(text[0]);
  const unsigned char b = static_cast<unsigned char>(text[n - 1]);
  if (a == '[' && b == ']') return true;
  // U+3010 / U+3011 ("【" / "】") in UTF-8.
  if (n >= 6 && a == 0xE3 && static_cast<unsigned char>(text[1]) == 0x80 && static_cast<unsigned char>(text[2]) == 0x90 &&
      static_cast<unsigned char>(text[n - 3]) == 0xE3 && static_cast<unsigned char>(text[n - 2]) == 0x80 &&
      static_cast<unsigned char>(text[n - 1]) == 0x91) {
    return true;
  }
  return false;
}

// Pulls the text of <span class='novelname'>...</span> when the anchor wraps an
// activity sentence ("<book> received a rocket"). Returns false when absent.
bool novelNameSpan(const char* inner, const size_t len, char* out, const size_t cap) {
  const char* span = ciFind(inner, len, "<span");
  while (span != nullptr) {
    const size_t at = static_cast<size_t>(span - inner);
    const size_t end = tagClose(inner, len, at);
    char cls[64];
    if (end > at && attrValue(inner + at, end - at, "class", cls, sizeof(cls)) && ciFind(cls, strlen(cls), "novelname")) {
      const char* close = ciFind(inner + end, len - end, "</span");
      if (close != nullptr) {
        extractText(inner + end, static_cast<size_t>(close - (inner + end)), out, cap);
        return out[0] != '\0';
      }
    }
    const char* next = ciFind(inner + end, len - end, "<span");
    if (next == nullptr) break;
    span = next;
  }
  return false;
}

// Parses one row block (already delimited to <li>...</li> or <tr>...</tr>).
// Returns false when the block holds no /book2/ anchor or no usable title.
bool parseRow(const char* html, const size_t len, RowCandidate& row) {
  const char* book = ciFind(html, len, "/book2/");
  if (book == nullptr) return false;
  size_t i = static_cast<size_t>(book - html) + 7;
  char id[16] = {};
  size_t idLen = 0;
  while (i < len && html[i] >= '0' && html[i] <= '9' && idLen + 1 < sizeof(id)) id[idLen++] = html[i++];
  if (idLen == 0) return false;
  memcpy(row.novelId, id, idLen);

  int bestScore = -1;
  const char* a = ciFind(html, len, "<a");
  while (a != nullptr) {
    const size_t at = static_cast<size_t>(a - html);
    const size_t openEnd = tagClose(html, len, at);
    if (openEnd > at) {
      char href[160];
      char cls[64] = {};
      const bool hasHref = attrValue(html + at, openEnd - at, "href", href, sizeof(href));
      attrValue(html + at, openEnd - at, "class", cls, sizeof(cls));
      // Only anchors pointing at the SAME novel as the row's first anchor are title
      // candidates. A row that links to a related work would otherwise let the
      // longest text win and label this book with another book's name.
      const char* hrefBook = hasHref ? ciFind(href, strlen(href), "/book2/") : nullptr;
      bool sameBook = false;
      if (hrefBook != nullptr) {
        size_t k = static_cast<size_t>(hrefBook - href) + 7;
        size_t n = 0;
        while (href[k + n] >= '0' && href[k + n] <= '9') ++n;
        sameBook = (n == idLen && memcmp(href + k, id, n) == 0);
      }
      if (hasHref && sameBook) {
        const char* close = ciFind(html + openEnd, len - openEnd, "</a");
        if (close != nullptr) {
          const size_t innerLen = static_cast<size_t>(close - (html + openEnd));
          char text[96];
          bool fromNovelName = novelNameSpan(html + openEnd, innerLen, text, sizeof(text));
          if (!fromNovelName) extractText(html + openEnd, innerLen, text, sizeof(text));
          if (text[0] != '\0' && !bracketOnly(text)) {
            // Prefer the span the site itself marked as a title: rank pages carry a
            // bracketed category anchor first, and activity rows wrap the name in
            // novelname. Both point at the same id, so the id check is unnecessary.
            int score = static_cast<int>(strlen(text));
            if (fromNovelName) score += 10000;
            if (ciFind(cls, strlen(cls), "track") != nullptr ||
                ciFind(html + at, openEnd - at, "data-recommendinfo") != nullptr) {
              score += 1000;
            }
            if (score > bestScore) {
              bestScore = score;
              snprintf(row.title, sizeof(row.title), "%s", text);
            }
          }
        }
      }
    }
    const char* next = ciFind(html + openEnd, len - openEnd, "<a");
    if (next == nullptr) break;
    a = next;
  }
  if (bestScore < 0) return false;

  const char* author = ciFind(html, len, "/wapauthor/");
  if (author != nullptr) {
    const char* close = ciFind(author, len - static_cast<size_t>(author - html), "</a");
    const char* gt = static_cast<const char*>(memchr(author, '>', close ? static_cast<size_t>(close - author) : 0));
    if (gt != nullptr && close != nullptr && close > gt) {
      extractText(gt + 1, static_cast<size_t>(close - gt - 1), row.author, sizeof(row.author));
    }
  }
  return true;
}

struct ParseCtx {
  DiscoverSink sink;
  void* user;
  size_t count;
  bool stopped;
};

void emitRow(ParseCtx& ctx, const RowCandidate& row) {
  if (ctx.stopped) return;
  DiscoverEntry entry;
  snprintf(entry.novelId, sizeof(entry.novelId), "%s", row.novelId);
  snprintf(entry.novelName, sizeof(entry.novelName), "%s", row.title);
  snprintf(entry.authorName, sizeof(entry.authorName), "%s", row.author);
  if (!ctx.sink(ctx.user, entry, ctx.count)) {
    ctx.stopped = true;
    return;
  }
  ++ctx.count;
}

// Parses every `<li>` or `<tr>` block. `skipWhenHolds` makes the table pass ignore
// wrappers whose cell contains a list -- otherwise a <tr><td><ul><li> layout emits
// a duplicate row built from the first anchor only.
void parseBlocks(const char* html, const size_t len, const char* openLower, const char* closeLower,
                 bool skipWhenHoldsLi, ParseCtx& ctx) {
  size_t i = 0;
  while (i < len && !ctx.stopped) {
    const char* p = ciFind(html + i, len - i, openLower);
    if (p == nullptr) break;
    const size_t at = static_cast<size_t>(p - html);
    const char after = (at + strlen(openLower) < len) ? html[at + strlen(openLower)] : '\0';
    if (!(after == '>' || after == '/' || isSpace(after))) {
      i = at + strlen(openLower);
      continue;
    }
    const char* endp = ciFind(html + at, len - at, closeLower);
    const size_t end = endp ? static_cast<size_t>(endp - html) + strlen(closeLower) : len;
    const size_t blockLen = end - at;
    bool usable = ciFind(html + at, blockLen, "/book2/") != nullptr;
    if (usable && skipWhenHoldsLi && ciFind(html + at, blockLen, "<li") != nullptr) usable = false;
    if (usable) {
      RowCandidate row;
      if (parseRow(html + at, blockLen, row)) emitRow(ctx, row);
    }
    i = end;
  }
}

void parsePager(const char* html, const size_t len, DiscoverPage& page, const uint32_t requested) {
  page.current = requested == 0 ? 1 : requested;
  const char* a = ciFind(html, len, "<a");
  while (a != nullptr) {
    const size_t at = static_cast<size_t>(a - html);
    const size_t openEnd = tagClose(html, len, at);
    char href[192];
    if (openEnd > at && attrValue(html + at, openEnd - at, "href", href, sizeof(href))) {
      uint32_t number = 0;
      const char* q = ciFind(href, strlen(href), "page=");
      if (q != nullptr) {
        const char* d = q + 5;
        while (*d >= '0' && *d <= '9') number = number * 10 + static_cast<uint32_t>(*d++ - '0');
      } else {
        const char* slash = ciFind(href, strlen(href), "/page/");
        if (slash != nullptr) {
          const char* d = slash + 6;
          while (*d >= '0' && *d <= '9') number = number * 10 + static_cast<uint32_t>(*d++ - '0');
        }
      }
      if (number > page.max) page.max = number;
      if (number == page.current + 1 && page.nextUrl[0] == '\0') {
        snprintf(page.nextUrl, sizeof(page.nextUrl), "%s", href);
      }
    }
    const char* next = ciFind(html + openEnd, len - openEnd, "<a");
    if (next == nullptr) break;
    a = next;
  }
}

}  // namespace

size_t parseDiscoverList(const char* html, size_t len, DiscoverSink sink, void* ctx, DiscoverPage* pageOut,
                         uint32_t requestedPage) {
  if (html == nullptr || sink == nullptr || len == 0) return 0;
  ParseCtx pctx{sink, ctx, 0, false};
  // List-style rows first. Table rows are parsed only when they are not wrappers
  // around a list, which is what makes a page that mixes both safe.
  parseBlocks(html, len, "<li", "</li", false, pctx);
  parseBlocks(html, len, "<tr", "</tr", true, pctx);
  if (pageOut != nullptr) parsePager(html, len, *pageOut, requestedPage);
  return pctx.count;
}

size_t parseAssortTags(const char* html, size_t len, AssortTagSink sink, void* ctx) {
  if (html == nullptr || sink == nullptr || len == 0) return 0;
  size_t count = 0;
  size_t i = 0;
  while (i < len) {
    const char* p = ciFind(html + i, len - i, "class=");
    if (p == nullptr) break;
    const size_t at = static_cast<size_t>(p - html);
    // The value must be quoted and contain tag_text. Reading just the value keeps
    // this from matching $('.tag_text') in the page's own scripts.
    size_t j = at + 6;
    if (j >= len || (html[j] != '"' && html[j] != '\'')) {
      i = at + 6;
      continue;
    }
    const char quote = html[j++];
    const size_t valueStart = j;
    while (j < len && html[j] != quote) ++j;
    if (j >= len) break;
    const char* value = html + valueStart;
    const size_t valueLen = j - valueStart;
    if (ciFind(value, valueLen, "tag_text") == nullptr) {
      i = j + 1;
      continue;
    }
    const size_t tagEnd = tagClose(html, len, at);
    const char* labelClose = ciFind(html + tagEnd, len - tagEnd, "</label");
    if (labelClose == nullptr) break;
    AssortTag tag;
    extractText(html + tagEnd, static_cast<size_t>(labelClose - (html + tagEnd)), tag.name, sizeof(tag.name));
    const size_t afterLabel = static_cast<size_t>(labelClose - html) + 8;
    const char* span = ciFind(html + afterLabel, len - afterLabel, "add_tags");
    if (tag.name[0] != '\0' && span != nullptr) {
      const size_t spanAt = static_cast<size_t>(span - html);
      const size_t spanEnd = tagClose(html, len, spanAt);
      if (spanEnd > spanAt && attrValue(html + spanAt, spanEnd - spanAt, "val", tag.id, sizeof(tag.id))) {
        if (!sink(ctx, tag, count)) return count;
        ++count;
      }
    }
    i = afterLabel;
  }
  return count;
}

}  // namespace jjwxc
