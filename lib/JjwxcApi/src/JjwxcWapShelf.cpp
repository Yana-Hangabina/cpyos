#include "JjwxcWapShelf.h"

#include <string.h>

namespace jjwxc {
namespace {

// --------------------------------------------------------------- scanning ---
//
// iStartsWith / findCi / parseUint are the same three helpers JjwxcWap.cpp keeps in
// its own anonymous namespace. They are duplicated rather than shared because both
// files hold them file-locally and sharing would mean editing JjwxcWap.* to expose
// them; if a third HTML parser appears, they should move into an internal header.

// Case-insensitive prefix match. The page mixes `id="favul"` with uppercase tag
// names elsewhere, and a strict match would silently find nothing.
bool iStartsWith(const char* s, size_t len, size_t at, const char* lit) {
  const size_t n = strlen(lit);
  if (at + n > len) return false;
  for (size_t i = 0; i < n; ++i) {
    const char a = s[at + i];
    const char b = lit[i];
    const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
    const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
    if (la != lb) return false;
  }
  return true;
}

// Index of `lit` at or after `from`, case-insensitively. `limit` is the exclusive
// end of the region to search; len on miss.
size_t findCi(const char* s, size_t len, size_t from, size_t limit, const char* lit) {
  const size_t n = strlen(lit);
  if (n == 0 || from >= len) return len;
  const size_t last = (limit < len ? limit : len);
  if (n > last || from + n > last) return len;
  for (size_t i = from; i + n <= last; ++i) {
    if (iStartsWith(s, len, i, lit)) return i;
  }
  return len;
}

bool isTagBoundary(const char c) { return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool isAsciiSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

// "Not found" for the one scanner whose success value can legitimately be `len`
// (findElementEnd, which returns the index just PAST a close tag). Every other
// helper returns an index OF something, so `len` is unambiguous for them.
constexpr size_t kNoEnd = SIZE_MAX;

// Index of the '>' that closes the tag opened at `openAt` (which points at '<'),
// skipping '>' inside a quoted attribute value -- the rows carry
// `style="color: #ACABAB"` and a naive scan is still correct there, but the pager
// carries `style="font-size:14px;"` and other pages could carry a '>' in a title
// attribute. len when the tag never closes.
size_t tagClose(const char* s, size_t len, size_t openAt, size_t limit) {
  char quote = 0;
  const size_t last = (limit < len ? limit : len);
  for (size_t i = openAt + 1; i < last; ++i) {
    const char c = s[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '>') return i;
    if (c == '<') return len;  // a new tag began before this one ended: malformed
  }
  return len;
}

// Index of the '<' that opens a tag named `name` at or after `from`, or len.
size_t findTagOpen(const char* s, size_t len, size_t from, size_t limit, const char* name) {
  const size_t nlen = strlen(name);
  const size_t last = (limit < len ? limit : len);
  for (size_t i = from; i + 1 + nlen <= last; ++i) {
    if (s[i] != '<') continue;
    if (!iStartsWith(s, len, i + 1, name)) continue;
    const size_t after = i + 1 + nlen;
    if (after >= last) return len;
    if (!isTagBoundary(s[after])) continue;  // `<list` is not `<li`
    return i;
  }
  return len;
}

// Index just past the `</name>` that closes the element whose open tag starts at
// `openAt`, or kNoEnd when the element is never closed inside `limit`. Depth is
// counted, so a nested element of the same name does not end the walk early.
//
// kNoEnd is SIZE_MAX and NOT `len`: a well-formed document whose list is the last
// thing in it closes with `</ul>` at the very end, and the index just past that is
// exactly len. Using len as the miss value silently discarded every such page, which
// is any page built by concatenating literals in a test and plenty of real ones.
size_t findElementEnd(const char* s, size_t len, size_t openAt, size_t limit, const char* name) {
  const size_t nlen = strlen(name);
  const size_t last = (limit < len ? limit : len);
  int depth = 0;
  size_t i = openAt;
  while (i < last) {
    if (s[i] != '<') {
      ++i;
      continue;
    }
    const bool closing = (i + 1 < last) && s[i + 1] == '/';
    const size_t nameAt = closing ? i + 2 : i + 1;
    if (nameAt + nlen > last || !iStartsWith(s, len, nameAt, name)) {
      ++i;
      continue;
    }
    const size_t after = nameAt + nlen;
    // The name has to be followed by something that cannot be part of a name, and
    // that character has to actually be there: `</ul` at the end of a truncated
    // buffer is not a close tag.
    if (after >= last || !isTagBoundary(s[after])) {
      ++i;
      continue;
    }
    if (closing) {
      --depth;
      if (depth <= 0) {
        const size_t gt = tagClose(s, len, i, last);
        return gt >= len ? kNoEnd : gt + 1;
      }
      i = after;
      continue;
    }
    const size_t gt = tagClose(s, len, i, last);
    if (gt >= len) return kNoEnd;
    const bool selfClosing = (gt > i) && (s[gt - 1] == '/');
    if (!selfClosing) ++depth;
    i = gt + 1;
  }
  return kNoEnd;
}

// ------------------------------------------------------------- attributes ---

// Value of attribute `name` within [from, to). Handles both quote styles and an
// unquoted value, and requires the name to start at an attribute boundary so that
// searching for `time` cannot land inside `readingorbookmarktime`.
bool attrValue(const char* s, size_t len, size_t from, size_t to, const char* name, size_t& vAt, size_t& vLen) {
  const size_t nlen = strlen(name);
  const size_t last = (to < len ? to : len);
  if (from >= last || nlen == 0) return false;
  for (size_t i = from; i + nlen <= last; ++i) {
    if (i > from && !isAsciiSpace(s[i - 1])) continue;
    if (!iStartsWith(s, len, i, name)) continue;
    size_t j = i + nlen;
    while (j < last && isAsciiSpace(s[j])) ++j;
    if (j >= last || s[j] != '=') continue;
    ++j;
    while (j < last && isAsciiSpace(s[j])) ++j;
    if (j >= last) return false;
    if (s[j] == '"' || s[j] == '\'') {
      const char q = s[j++];
      const size_t start = j;
      while (j < last && s[j] != q) ++j;
      vAt = start;
      vLen = j - start;
      return true;
    }
    const size_t start = j;
    while (j < last && !isAsciiSpace(s[j]) && s[j] != '>' && s[j] != '/') ++j;
    vAt = start;
    vLen = j - start;
    return vLen > 0;
  }
  return false;
}

// Walks `<a>` tags in [cursor, end). On success `cursor` advances past the anchor so
// repeated calls enumerate them in document order.
bool nextAnchor(const char* s, size_t len, size_t end, size_t& cursor, size_t& hrefAt, size_t& hrefLen, size_t& textAt,
                size_t& textLen, size_t& anchorEnd) {
  const size_t a = findTagOpen(s, len, cursor, end, "a");
  if (a >= len) return false;
  const size_t gt = tagClose(s, len, a, end);
  if (gt >= len) return false;

  hrefAt = 0;
  hrefLen = 0;
  attrValue(s, len, a + 2, gt, "href", hrefAt, hrefLen);

  // findTagOpen with the name "/a" matches `</a>` and rejects `</abbr>`, because the
  // character after the name has to be a tag boundary.
  const size_t close = findTagOpen(s, len, gt + 1, end, "/a");
  textAt = gt + 1;
  textLen = (close < len) ? close - textAt : 0;
  anchorEnd = (close < len) ? close + 4 : gt + 1;
  cursor = anchorEnd;
  return true;
}

bool hrefStartsWith(const char* s, size_t hrefAt, size_t hrefLen, const char* prefix) {
  const size_t n = strlen(prefix);
  if (hrefLen < n) return false;
  return memcmp(s + hrefAt, prefix, n) == 0;
}

// Digits immediately following `/book2/` (or any prefix) inside an href. Returns the
// count of digits, 0 when the href does not hold a plain numeric id.
size_t hrefIdDigits(const char* s, size_t hrefAt, size_t hrefLen, const char* prefix, size_t& idAt) {
  const size_t n = strlen(prefix);
  if (hrefLen <= n || memcmp(s + hrefAt, prefix, n) != 0) return 0;
  size_t k = n;
  while (k < hrefLen && s[hrefAt + k] >= '0' && s[hrefAt + k] <= '9') ++k;
  if (k == n) return 0;
  idAt = hrefAt + n;
  return k - n;
}

// ---------------------------------------------------------------- UTF-8 -----

size_t utf8SeqLen(const uint8_t c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 0;
}

size_t utf8Encode(const uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

// Start index of the last UTF-8 sequence in [0, len).
size_t utf8TailStart(const char* s, size_t len) {
  size_t i = len;
  while (i > 0 && (static_cast<uint8_t>(s[i - 1]) & 0xC0) == 0x80) --i;
  if (i > 0) --i;
  return i;
}

// The whitespace a Chinese book list actually uses: ASCII, the &nbsp; this page
// uses as its separator, and U+3000 IDEOGRAPHIC SPACE.
bool isSpaceSeq(const char* s, size_t n) {
  if (n == 1) return isAsciiSpace(s[0]);
  if (n == 2) return static_cast<uint8_t>(s[0]) == 0xC2 && static_cast<uint8_t>(s[1]) == 0xA0;
  if (n == 3) {
    return static_cast<uint8_t>(s[0]) == 0xE3 && static_cast<uint8_t>(s[1]) == 0x80 &&
           static_cast<uint8_t>(s[2]) == 0x80;
  }
  return false;
}

// --------------------------------------------------------------- entities ---

struct NamedEntity {
  const char* name;  // without '&' or ';'
  uint32_t cp;
};

// Only the names that can plausibly appear in a title or an author. An unknown name
// is left verbatim, which is what a browser does and which keeps `AT&T` readable
// instead of eating the tail of the name.
constexpr NamedEntity kNamedEntities[] = {
    {"amp", 0x26}, {"lt", 0x3C},    {"gt", 0x3E},   {"quot", 0x22},   {"apos", 0x27}, {"nbsp", 0xA0},
    {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"middot", 0xB7},
};

// Requires the closing ';'. The semicolon-less numeric form that JjwxcWap.cpp has to
// handle is a property of the VIP watermark, which only ever appears inside a
// decrypted chapter body; accepting it here would let a title's bare '&' eat the
// word after it.
bool parseEntity(const char* s, size_t len, size_t& consumed, uint32_t& cp) {
  if (len < 3 || s[0] != '&') return false;

  if (s[1] == '#') {
    const bool hex = (len > 3) && (s[2] == 'x' || s[2] == 'X');
    size_t i = hex ? 3 : 2;
    uint32_t v = 0;
    size_t digits = 0;
    while (i < len && digits < 6) {
      const char c = s[i];
      uint32_t d;
      if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
      else if (hex && c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
      else if (hex && c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
      else break;
      v = v * (hex ? 16u : 10u) + d;
      ++i;
      ++digits;
    }
    if (digits == 0 || v == 0 || v > 0x10FFFF) return false;
    if (i >= len || s[i] != ';') return false;
    cp = v;
    consumed = i + 1;
    return true;
  }

  for (const auto& e : kNamedEntities) {
    const size_t n = strlen(e.name);
    if (len < n + 2) continue;
    if (memcmp(s + 1, e.name, n) != 0) continue;
    if (s[1 + n] != ';') continue;
    cp = e.cp;
    consumed = n + 2;
    return true;
  }
  return false;
}

// Copies a text run into a NUL-terminated field, decoding entities and trimming
// surrounding whitespace. Truncation lands on a code-point boundary: splitting a
// UTF-8 sequence would leave an invalid byte inside a title, and everything
// downstream (the on-screen list, the EPUB spine) assumes valid UTF-8.
void copyText(char* dst, size_t dstCap, const char* src, size_t srcLen) {
  if (dst == nullptr || dstCap == 0) return;
  if (src == nullptr) srcLen = 0;

  size_t o = 0;
  size_t i = 0;
  while (i < srcLen) {
    if (src[i] == '<') {
      // Markup nested inside an anchor's text. The anonymous search page carries none,
      // but the same request made with a live session wraps the matched keyword in
      // highlight tags, and without this the literal `<font color=...>` ends up in the
      // title and on the screen. Tags are pure ASCII, so skipping to the closing '>' is
      // safe inside a UTF-8 run; an unterminated '<' drops the rest of the run rather
      // than emitting a half tag.
      const char* gt = static_cast<const char*>(memchr(src + i, '>', srcLen - i));
      i = (gt != nullptr) ? static_cast<size_t>(gt - src) + 1 : srcLen;
      continue;
    }
    if (src[i] == '&') {
      uint32_t cp = 0;
      size_t consumed = 0;
      if (parseEntity(src + i, srcLen - i, consumed, cp)) {
        char tmp[4];
        const size_t n = utf8Encode(cp, tmp);
        if (o + n > dstCap - 1) break;
        memcpy(dst + o, tmp, n);
        o += n;
        i += consumed;
        continue;
      }
    }
    size_t n = utf8SeqLen(static_cast<uint8_t>(src[i]));
    if (n == 0 || n > srcLen - i) n = 1;  // not UTF-8: carry the byte through
    if (o + n > dstCap - 1) break;
    memcpy(dst + o, src + i, n);
    o += n;
    i += n;
  }
  dst[o] = '\0';

  size_t start = 0;
  while (start < o) {
    size_t n = utf8SeqLen(static_cast<uint8_t>(dst[start]));
    if (n == 0 || start + n > o) n = 1;
    if (!isSpaceSeq(dst + start, n)) break;
    start += n;
  }
  if (start > 0) {
    memmove(dst, dst + start, o - start);
    o -= start;
    dst[o] = '\0';
  }
  while (o > 0) {
    const size_t st = utf8TailStart(dst, o);
    if (!isSpaceSeq(dst + st, o - st)) break;
    o = st;
    dst[o] = '\0';
  }
}

// True when [s, s+n) holds exactly the UTF-8 bytes of `lit`.
bool seqEqualsLiteral(const char* s, size_t n, const char* lit) {
  const size_t ln = strlen(lit);
  return n == ln && memcmp(s, lit, ln) == 0;
}

// --------------------------------------------------------------- calendar ---

// Howard Hinnant's civil-days formula, proleptic Gregorian, days since 1970-01-01.
// The fourth copy in this repo: src/util/TimeUtils.cpp, lib/hal/HalClock.cpp and
// freeink-sdk/libs/hardware/Rtc/src/Rtc.cpp each carry one, and Rtc.cpp records why
// -- `mktime()` is local-time and `timegm()` is not declared by the newlib <time.h>
// this project builds against, so this is the only timezone-independent route. It is
// duplicated here rather than shared because this library must stay free of firmware
// headers to remain host-testable.
int64_t daysFromCivil(int year, const unsigned month, const unsigned day) {
  year -= (month <= 2) ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

// n digits at s+at, or -1 when any of them is not a digit.
int readDigits(const char* s, size_t len, size_t at, size_t n) {
  if (at + n > len) return -1;
  int v = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[at + i];
    if (c < '0' || c > '9') return -1;
    v = v * 10 + (c - '0');
  }
  return v;
}

// ----------------------------------------------------------------- pager ----

constexpr size_t kMaxPageLinks = 64;  // the live capture has 4; this is a hard stop

// Finds `/my/fav?p=N` at or after `from` and reports N. len on miss.
size_t nextPageLink(const char* s, size_t len, size_t from, size_t limit, uint32_t& page) {
  const size_t last = (limit < len ? limit : len);
  size_t i = from;
  while (i < last) {
    const size_t hit = findCi(s, len, i, last, "/my/fav?p=");
    if (hit >= len) return len;
    size_t j = hit + 10;
    uint64_t v = 0;
    size_t digits = 0;
    while (j < last && s[j] >= '0' && s[j] <= '9' && digits < 9) {
      v = v * 10 + static_cast<uint64_t>(s[j] - '0');
      ++j;
      ++digits;
    }
    if (digits > 0 && v > 0 && v <= 0xFFFFFFFFu) {
      page = static_cast<uint32_t>(v);
      return hit;
    }
    i = hit + 10;  // not a page link (no digits): keep looking
  }
  return len;
}

// The page number inside the pager's highlighted entry. The live capture renders the
// current page as `<span style="background-color:blue;">&nbsp;<a ...
// href="/my/fav?p=1&t=0">1</a>&nbsp;</span>`, so the highlight is what identifies
// "here" and the link inside it is what numbers it. 0 when there is no highlight.
uint32_t detectCurrentPage(const char* s, size_t len, size_t from, size_t limit) {
  const size_t last = (limit < len ? limit : len);
  size_t at = from;
  while (at < last) {
    const size_t hit = findCi(s, len, at, last, "background-color:");
    if (hit >= len) return 0;
    size_t j = hit + 17;
    while (j < last && isAsciiSpace(s[j])) ++j;
    at = hit + 17;
    if (!iStartsWith(s, len, j, "blue")) continue;
    // The link belongs to this span; stop at its close so a following entry cannot
    // be mistaken for the highlighted one.
    size_t spanEnd = findCi(s, len, j, last, "</span");
    if (spanEnd >= len) spanEnd = last;
    uint32_t page = 0;
    if (nextPageLink(s, len, j, spanEnd, page) < len) return page;
  }
  return 0;
}

// ------------------------------------------------------------- the row ------

// The reader-visible marker, matched as raw UTF-8 bytes so no wide-character or
// locale support is needed. Only consulted inside a <span>, see rowHasUpdate().
constexpr const char* kUpdateMarker = "\xEF\xBC\x88\xE6\x9C\x89\xE6\x9B\xB4\xE6\x96\xB0\xEF\xBC\x89";  // (you geng xin)

// True when some <span> in [from, end) has exactly the marker as its whole text.
// Scoping it to a span is what stops a book or an author actually named that way
// from claiming an update it does not have; titles and authors live in <a>.
bool spanHoldsMarker(const char* s, size_t len, size_t from, size_t end) {
  size_t cursor = from;
  while (cursor < end) {
    const size_t sp = findTagOpen(s, len, cursor, end, "span");
    if (sp >= len) return false;
    const size_t gt = tagClose(s, len, sp, end);
    if (gt >= len) return false;
    const size_t close = findTagOpen(s, len, gt + 1, end, "/span");
    const size_t textEnd = (close < len) ? close : end;
    if (textEnd > gt + 1 && seqEqualsLiteral(s + gt + 1, textEnd - (gt + 1), kUpdateMarker)) return true;
    cursor = (close < len) ? close + 1 : gt + 1;
  }
  return false;
}

// `renewStatus` is the row's "has updates" flag. Two independent signals are OR-ed:
//   * `class="haveupdate"` -- ASCII, cannot be produced by a book title, and the one
//     the site's markup hangs the styling off;
//   * the marker text the reader actually sees, but only as a whole <span> body.
// In the live capture they agree exactly: 16 rows carry both, 14 carry neither. The
// second is there so that a class rename does not silently darken every badge --
// this project has been burned repeatedly by a signal failing quietly.
//
// A third route exists and was deliberately NOT used: the page's own JavaScript
// decides updates as `readingorbookmarktime < chapterlastposttime`. That comparison
// agrees with the marker on all 30 rows of the capture, which is good corroboration,
// but the script uses it for its bulk "clear update time" feature, so the two are
// not guaranteed to stay equivalent and the marker is what the site renders.
bool rowHasUpdate(const char* s, size_t len, size_t liAt, size_t liEnd) {
  const size_t last = (liEnd < len ? liEnd : len);
  if (findCi(s, len, liAt, last, "\"haveupdate") < len) return true;
  if (findCi(s, len, liAt, last, "'haveupdate") < len) return true;
  return spanHoldsMarker(s, len, liAt, last);
}

// Parses one `<li>` into an entry. Returns false when the row holds no `/book2/`
// anchor, which is how the trailing pager row (and any other non-book row the site
// may add inside `favul`) is skipped.
bool parseRow(const char* s, size_t len, size_t liAt, size_t liEnd, WapShelfEntry& out) {
  const size_t last = (liEnd < len ? liEnd : len);
  if (liAt >= last) return false;

  const size_t gt = tagClose(s, len, liAt, last);
  const size_t attrsEnd = (gt < len) ? gt : last;

  bool haveBook = false;
  size_t cursor = liAt;
  size_t hrefAt = 0, hrefLen = 0, textAt = 0, textLen = 0, anchorEnd = 0;
  while (nextAnchor(s, len, last, cursor, hrefAt, hrefLen, textAt, textLen, anchorEnd)) {
    size_t idAt = 0;
    const size_t idDigits = hrefIdDigits(s, hrefAt, hrefLen, "/book2/", idAt);
    if (!haveBook && idDigits > 0) {
      // First book anchor wins. Later ones in the same row are the series/related
      // label the site renders beside the title, and they point at the same novel id
      // in every row of the capture -- emitting them too would list one book twice.
      const size_t n = idDigits < sizeof(out.novelId) - 1 ? idDigits : sizeof(out.novelId) - 1;
      memcpy(out.novelId, s + idAt, n);
      out.novelId[n] = '\0';
      copyText(out.novelName, sizeof(out.novelName), s + textAt, textLen);
      haveBook = true;
      continue;
    }
    if (out.authorName[0] == '\0' && hrefStartsWith(s, hrefAt, hrefLen, "/wapauthor/")) {
      copyText(out.authorName, sizeof(out.authorName), s + textAt, textLen);
    }
  }
  if (!haveBook || out.novelId[0] == '\0') return false;

  size_t vAt = 0, vLen = 0;
  // liAt + 3 skips `<li`; findTagOpen only ever returns a row that starts with it.
  if (attrValue(s, len, liAt + 3, attrsEnd, "chapterlastposttime", vAt, vLen)) {
    out.lastChapterTime = parseShelfTimestamp(s + vAt, vLen);
  }
  out.renewStatus = rowHasUpdate(s, len, liAt, last) ? 1 : 0;

  // coverUrl, lastChapterName, chapterCount and lastChapterId stay empty/zero: this
  // page does not carry them. See the header for where the caller gets them.
  return true;
}

// Locates `<ul id="favul">` (any element bearing that id, in fact) and returns its
// content range. False when the document has no such element.
//
// Tolerates either quote style and whitespace after the '=', which is what the
// capture uses; it does NOT tolerate whitespace before the '=' (`id = "favul"`),
// because the page is machine-generated from one template and adding an unexercised
// path here would be a guess. Missing the list fails loudly -- zero rows -- rather
// than producing a wrong shelf.
bool findFavul(const char* s, size_t len, size_t& contentAt, size_t& contentEnd) {
  size_t at = 0;
  while (at < len) {
    const size_t hit = findCi(s, len, at, len, "id=");
    if (hit >= len) return false;
    at = hit + 3;
    size_t j = hit + 3;
    while (j < len && isAsciiSpace(s[j])) ++j;
    if (j >= len) return false;
    const char q = s[j];
    if (q != '"' && q != '\'') continue;
    ++j;
    const size_t vStart = j;
    while (j < len && s[j] != q) ++j;
    if (j >= len) return false;
    if (j - vStart != 5 || memcmp(s + vStart, "favul", 5) != 0) continue;

    // Walk back to the '<' that opens this tag and require a container element, so
    // that `id="favul"` on, say, a script cannot be mistaken for the list.
    size_t open = vStart;
    while (open > 0 && s[open] != '<') --open;
    if (s[open] != '<') return false;
    const size_t gt = tagClose(s, len, open, len);
    if (gt >= len) return false;

    // The tag name is the run between '<' and the first character that cannot be part
    // of a name. Scanning forward rather than back from '>' keeps this correct for
    // `<ul id="favul">`, where the quoted value also contains no whitespace.
    size_t nameLen = 0;
    while (open + 1 + nameLen < gt) {
      const char c = s[open + 1 + nameLen];
      if (isAsciiSpace(c) || c == '/' || c == '>') break;
      ++nameLen;
    }
    if (nameLen == 0 || nameLen > 8) return false;
    char name[9];
    memcpy(name, s + open + 1, nameLen);
    name[nameLen] = '\0';
    for (size_t k = 0; k < nameLen; ++k) {
      if (name[k] >= 'A' && name[k] <= 'Z') name[k] = static_cast<char>(name[k] - 'A' + 'a');
    }

    const size_t end = findElementEnd(s, len, open, len, name);
    if (end == kNoEnd) return false;
    contentAt = gt + 1;
    // Step back over the `</name>` findElementEnd landed past. Guarded because a
    // truncated document can put `end` closer to 0 than the close tag is long.
    contentEnd = (end >= nameLen + 3) ? end - (nameLen + 3) : contentAt;
    if (contentEnd < contentAt) contentEnd = contentAt;
    return true;
  }
  return false;
}

// ----------------------------------------------------------- search: list ----

// Literals the search scanners key on. Named once so the length used to step past a
// match cannot drift from the string it was measured off -- the same bug class as a
// hand-counted `hit + 10`.
constexpr const char* kSearchPagePath = "/search/index/page/";
constexpr const char* kMaxPageId = "max_page";
constexpr size_t kUlCloseLen = 5;  // strlen("</ul>")

// The result list of `/search`: the first `<ul>` whose content holds a `/book2/`
// href. The capture has exactly one `<ul>` and it carries no `id`, so findFavul()
// has nothing to key on. Requiring a book href INSIDE the element is what keeps this
// evidence-based instead of positional -- a nav or footer list added above the
// results later is skipped, and a document with no result list fails loudly with
// zero rows rather than emitting whatever `<li>` happens to be lying around.
bool findSearchUl(const char* s, size_t len, size_t& contentAt, size_t& contentEnd) {
  size_t from = 0;
  while (from < len) {
    const size_t ul = findTagOpen(s, len, from, len, "ul");
    if (ul >= len) return false;
    const size_t gt = tagClose(s, len, ul, len);
    if (gt >= len) return false;
    const size_t end = findElementEnd(s, len, ul, len, "ul");
    // An unclosed `<ul>` is a truncated document, not a different page shape: parse
    // the rows that did arrive instead of dropping them.
    const size_t stop = (end == kNoEnd) ? len : end;
    contentAt = gt + 1;
    // Step back over the `</ul>` findElementEnd landed past.
    contentEnd = (end == kNoEnd) ? len : ((stop >= kUlCloseLen) ? stop - kUlCloseLen : contentAt);
    if (contentEnd < contentAt) contentEnd = contentAt;
    if (findCi(s, len, contentAt, contentEnd, "/book2/") < len) return true;
    from = (stop > from) ? stop : from + 1;
  }
  return false;
}

// ---------------------------------------------------------- search: pager ----

// Finds `/search/index/page/<digits>` at or after `from` and reports the number.
// len on miss. One search covers both the absolute hrefs this page renders and any
// relative form, because the path is a substring of both -- which is also why the
// caller must pass a scope: see the sort bar note in the header.
size_t nextSearchPageLink(const char* s, size_t len, size_t from, size_t limit, uint32_t& page) {
  const size_t plen = strlen(kSearchPagePath);
  const size_t last = (limit < len ? limit : len);
  size_t i = from;
  while (i < last) {
    const size_t hit = findCi(s, len, i, last, kSearchPagePath);
    if (hit >= len) return len;
    size_t j = hit + plen;
    uint64_t v = 0;
    size_t digits = 0;
    while (j < last && s[j] >= '0' && s[j] <= '9' && digits < 9) {
      v = v * 10 + static_cast<uint64_t>(s[j] - '0');
      ++j;
      ++digits;
    }
    if (digits > 0 && v > 0 && v <= 0xFFFFFFFFu) {
      page = static_cast<uint32_t>(v);
      return hit;
    }
    i = hit + plen;  // path with no usable number after it: keep looking
  }
  return len;
}

// The page the search pager says we are on: the whole text of a `<strong>` that
// holds nothing but digits. Requiring all-digits is what makes a `<strong>` around a
// book title harmless, and it is the only reason this can scan the list rather than
// having to locate `class="pagination"` first -- the capture has one `<strong>` and
// it is the pager's, but nothing pins that.
uint32_t detectSearchCurrentPage(const char* s, size_t len, size_t from, size_t limit) {
  const size_t last = (limit < len ? limit : len);
  size_t cursor = from;
  while (cursor < last) {
    const size_t st = findTagOpen(s, len, cursor, last, "strong");
    if (st >= len) return 0;
    const size_t gt = tagClose(s, len, st, last);
    if (gt >= len) return 0;
    const size_t close = findTagOpen(s, len, gt + 1, last, "/strong");
    const size_t textEnd = (close < len) ? close : last;
    size_t a = gt + 1;
    size_t b = textEnd;
    while (a < b && isAsciiSpace(s[a])) ++a;
    while (b > a && isAsciiSpace(s[b - 1])) --b;
    uint64_t v = 0;
    size_t digits = 0;
    bool bad = false;
    for (size_t i = a; i < b; ++i) {
      if (s[i] < '0' || s[i] > '9') {
        bad = true;
        break;
      }
      v = v * 10 + static_cast<uint64_t>(s[i] - '0');
      ++digits;
    }
    if (!bad && digits > 0 && digits <= 9 && v > 0) return static_cast<uint32_t>(v);
    cursor = (close < len) ? close + 1 : gt + 1;
  }
  return 0;
}

// The total page count off `<input type="hidden" id="max_page" value="N">`, which the
// page's own JavaScript reads to build its pager hrefs. 0 when absent or malformed,
// in which case the caller keeps the highest page number it saw linked. Walks back to
// the enclosing tag and reads `value` as an attribute rather than scanning forward
// from the match, so the `$('#max_page')` occurrence inside that same script cannot
// be mistaken for the input.
uint32_t detectSearchMaxPage(const char* s, size_t len) {
  const size_t idLen = strlen(kMaxPageId);
  size_t at = 0;
  while (at < len) {
    const size_t hit = findCi(s, len, at, len, kMaxPageId);
    if (hit >= len) return 0;
    at = hit + idLen;
    size_t open = hit;
    while (open > 0 && s[open] != '<') --open;
    if (s[open] != '<') continue;
    const size_t gt = tagClose(s, len, open, len);
    if (gt >= len) continue;
    size_t vAt = 0, vLen = 0;
    if (!attrValue(s, len, open + 1, gt, "value", vAt, vLen)) continue;
    if (vLen == 0 || vLen > 9) continue;
    uint64_t v = 0;
    bool bad = false;
    for (size_t i = 0; i < vLen; ++i) {
      const char c = s[vAt + i];
      if (c < '0' || c > '9') {
        bad = true;
        break;
      }
      v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (!bad && v > 0 && v <= 0xFFFFFFFFu) return static_cast<uint32_t>(v);
  }
  return 0;
}

}  // namespace

uint32_t parseShelfTimestamp(const char* s, size_t len) {
  if (s == nullptr || len == 0) return 0;
  while (len > 0 && isAsciiSpace(*s)) {
    ++s;
    --len;
  }
  while (len > 0 && isAsciiSpace(s[len - 1])) --len;
  if (len == 0) return 0;

  bool allDigits = true;
  for (size_t i = 0; i < len; ++i) {
    if (s[i] < '0' || s[i] > '9') {
      allDigits = false;
      break;
    }
  }
  if (allDigits) {
    // Every row of the live capture is here: a bare 10-digit unix-seconds value, so
    // no calendar arithmetic and no timezone to get wrong. Longer than 10 digits is
    // refused rather than rescaled -- a 13-digit value is probably milliseconds, but
    // "probably" has repeatedly cost this project a debugging cycle, and a 0 leaves
    // the book undated instead of dating it to the wrong decade.
    if (len > 10) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    if (v > 0xFFFFFFFFu) return 0;
    return static_cast<uint32_t>(v);
  }

  // Defensive second form: YYYY-MM-DD[ HH:MM[:SS]], read as UTC. Not observed in the
  // capture; see the header for the UTC+8 bias this carries and why it is not
  // "corrected".
  if (len != 10 && len != 16 && len != 19) return 0;
  if (s[4] != '-' || s[7] != '-') return 0;
  const int year = readDigits(s, len, 0, 4);
  const int month = readDigits(s, len, 5, 2);
  const int day = readDigits(s, len, 8, 2);
  if (year < 1970 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) return 0;

  int hour = 0, minute = 0, second = 0;
  if (len > 10) {
    if (s[10] != ' ' && s[10] != 'T') return 0;
    if (s[13] != ':') return 0;
    hour = readDigits(s, len, 11, 2);
    minute = readDigits(s, len, 14, 2);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return 0;
    if (len == 19) {
      if (s[16] != ':') return 0;
      second = readDigits(s, len, 17, 2);
      if (second < 0 || second > 59) return 0;
    }
  }

  const int64_t secs = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
                       static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(minute) * 60 + second;
  if (secs < 0 || secs > 0xFFFFFFFFLL) return 0;
  return static_cast<uint32_t>(secs);
}

size_t countShelfRowLinks(const char* li, size_t len) {
  if (li == nullptr || len == 0) return 0;
  size_t count = 0;
  size_t cursor = 0;
  size_t hrefAt = 0, hrefLen = 0, textAt = 0, textLen = 0, anchorEnd = 0;
  while (nextAnchor(li, len, len, cursor, hrefAt, hrefLen, textAt, textLen, anchorEnd)) {
    size_t idAt = 0;
    if (hrefIdDigits(li, hrefAt, hrefLen, "/book2/", idAt) > 0) ++count;
  }
  return count;
}

size_t parseWapShelf(const char* html, size_t len, WapShelfSink sink, void* ctx, WapShelfPage* pageOut,
                     uint32_t requestedPage) {
  WapShelfPage page;
  if (html == nullptr || len == 0) {
    if (pageOut != nullptr) *pageOut = page;
    return 0;
  }

  size_t listAt = 0, listEnd = 0;
  const bool haveList = findFavul(html, len, listAt, listEnd);

  // ---- pager -------------------------------------------------------------
  // Scanned over the whole document rather than only inside `favul`: in the capture
  // the pager is the list's own last row, but nothing pins it there, and
  // `/my/fav?p=N` is specific enough that a stray match elsewhere is not a risk.
  // Being position-independent also means a page that renders a pager both above and
  // below the list works: `next` is the smallest linked page above the current one
  // and `max` is the largest, so duplicate pager blocks simply agree with each other.
  // The highlight is looked for inside the list first, then document-wide.
  if (haveList) {
    page.current = detectCurrentPage(html, len, listAt, listEnd);
  }
  if (page.current == 0) {
    page.current = detectCurrentPage(html, len, 0, len);
  }
  if (requestedPage != 0) {
    // The caller knows what it asked for; the highlight is only ever observed on one
    // capture, so an explicit page wins.
    page.current = requestedPage;
  }

  uint32_t nextCandidate = 0;
  size_t links = 0;
  size_t cursor = 0;
  while (cursor < len && links < kMaxPageLinks) {
    uint32_t p = 0;
    const size_t hit = nextPageLink(html, len, cursor, len, p);
    if (hit >= len) break;
    ++links;
    if (p > page.max) page.max = p;
    if (page.current != 0 && p > page.current && (nextCandidate == 0 || p < nextCandidate)) nextCandidate = p;
    cursor = hit + 10;
  }
  page.next = nextCandidate;  // 0 when the current page is unknown, on purpose

  if (!haveList || sink == nullptr) {
    if (pageOut != nullptr) *pageOut = page;
    return 0;
  }

  // ---- rows --------------------------------------------------------------
  // A row is a book if and only if it holds a `/book2/` anchor. That is what skips
  // the pager row, and it does not depend on the pager being last: it is a `<li>`
  // like any other and lives inside `favul` in the capture.
  size_t emitted = 0;
  size_t at = listAt;
  while (at < listEnd) {
    const size_t li = findTagOpen(html, len, at, listEnd, "li");
    if (li >= len) break;
    const size_t liEnd = findElementEnd(html, len, li, listEnd, "li");
    // An unclosed final `<li>` still holds a book, so a truncated tail is parsed up
    // to the end of the list rather than dropped.
    const size_t stop = (liEnd == kNoEnd) ? listEnd : liEnd;

    WapShelfEntry entry;
    if (parseRow(html, len, li, stop, entry)) {
      if (!sink(ctx, entry, emitted)) {
        ++emitted;  // the caller took this one before saying stop
        break;
      }
      ++emitted;
    }
    at = (stop > at) ? stop : at + 1;
  }

  if (pageOut != nullptr) *pageOut = page;
  return emitted;
}

size_t parseWapSearch(const char* html, size_t len, WapShelfSink sink, void* ctx, WapShelfPage* pageOut,
                      uint32_t requestedPage) {
  WapShelfPage page;
  if (html == nullptr || len == 0) {
    if (pageOut != nullptr) *pageOut = page;
    return 0;
  }

  size_t listAt = 0, listEnd = 0;
  if (!findSearchUl(html, len, listAt, listEnd)) {
    // No result list means no results, and the only `/search/index/page/` links left
    // in the document then belong to the sort bar, which is not a pager. Reporting a
    // next page off those would send the caller after a page 2 of nothing.
    if (pageOut != nullptr) *pageOut = page;
    return 0;
  }

  // ---- pager -------------------------------------------------------------
  // Scoped to the list, never document-wide, because the sort bar above the list
  // uses the same `/search/index/page/` path -- see the header.
  page.current = detectSearchCurrentPage(html, len, listAt, listEnd);
  if (requestedPage != 0) {
    // The caller knows what it asked for; `<strong>` is only ever observed on one
    // capture, so an explicit page wins. Same precedence as parseWapShelf().
    page.current = requestedPage;
  }

  uint32_t nextCandidate = 0;
  size_t links = 0;
  size_t cursor = listAt;
  while (cursor < listEnd && links < kMaxPageLinks) {
    uint32_t p = 0;
    const size_t hit = nextSearchPageLink(html, len, cursor, listEnd, p);
    if (hit >= len) break;
    ++links;
    if (p > page.max) page.max = p;
    if (page.current != 0 && p > page.current && (nextCandidate == 0 || p < nextCandidate)) nextCandidate = p;
    cursor = hit + 1;
  }
  const uint32_t declared = detectSearchMaxPage(html, len);
  if (declared > page.max) page.max = declared;
  page.next = nextCandidate;  // 0 when the current page is unknown, on purpose

  if (sink == nullptr) {
    if (pageOut != nullptr) *pageOut = page;
    return 0;
  }

  // ---- rows --------------------------------------------------------------
  // Identical walk to parseWapShelf(), down to reusing parseRow(): one record per
  // `<li>`, and a row is a book if and only if it holds a `/book2/` anchor. That is
  // what skips the pager row, and it does not depend on the pager being last -- it is
  // a `<li>` like any other and it is last in the capture, but nothing here reads its
  // position or its `class="pagination"`.
  size_t emitted = 0;
  size_t at = listAt;
  while (at < listEnd) {
    const size_t li = findTagOpen(html, len, at, listEnd, "li");
    if (li >= len) break;
    const size_t liEnd = findElementEnd(html, len, li, listEnd, "li");
    // An unclosed final `<li>` still holds a book, so a truncated tail is parsed up
    // to the end of the list rather than dropped.
    const size_t stop = (liEnd == kNoEnd) ? listEnd : liEnd;

    WapShelfEntry entry;
    if (parseRow(html, len, li, stop, entry)) {
      if (!sink(ctx, entry, emitted)) {
        ++emitted;  // the caller took this one before saying stop
        break;
      }
      ++emitted;
    }
    at = (stop > at) ? stop : at + 1;
  }

  if (pageOut != nullptr) *pageOut = page;
  return emitted;
}

}  // namespace jjwxc
