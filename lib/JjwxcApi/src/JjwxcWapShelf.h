// JJWXC WAP (m.jjwxc.net) list parsing: the bookshelf (`GET /my/fav`) and search
// results (`GET /search?kw=`).
//
// The WAP shelf is a server-rendered HTML list, not JSON, so this is the HTML
// counterpart of ShelfParser in JjwxcProtocol.h. One `<li>` inside
// `<ul id="favul">` is one book:
//
//   <ul id="favul">
//     <li readingorbookmarktime='1614168098' chapterlastposttime='1569418760'>
//       <a href="/book2/NNNNNNN" class="" >TITLE</a>
//       <span class="haveupdate">(UPDATED)</span>          <!-- optional -->
//       - <a href="/wapauthor/NNNNNNN">AUTHOR</a>
//       &nbsp; -&nbsp;
//       <a href="/my/delbook?novelid=NNNNNNN"><span style="color: #ACABAB">[DEL]</span></a>
//     </li>
//     ...
//     <li style="font-size:14px;"> ...pagination... </li>   <!-- NOT a book -->
//   </ul>
//
// Three rules this file exists to enforce, all measured against a live capture:
//
//   - One record per `<li>`, never per link. Some rows carry TWO `/book2/` anchors
//     (a series/related-work label beside the title, both pointing at the same
//     novel id). The capture has 33 `/book2/` hrefs across 30 rows, so counting
//     links yields 33 books for a 30-book shelf and duplicates three of them.
//     The first anchor in the row is the one kept.
//   - The trailing `<li>` is the pager, and it lives INSIDE `favul`. It has no
//     `/book2/` anchor, which is what distinguishes it -- not its position, and
//     not its `style` attribute.
//   - `coverUrl`, `chapterCount`, `lastChapterId` and `lastChapterName` are NOT in
//     this page and are left empty/zero rather than guessed at. The caller fills
//     them from `https://app-cdn.jjwxc.net/androidapi/novelbasicinfo?novelId=N`,
//     which needs no credential at all (verified: HTTP 200, 190 chapters x 32
//     fields per chapter, anonymous). Inventing a cover URL from the novel id
//     would be a guess that renders as a broken thumbnail.
//
// No heap, no STL containers, no Arduino/ESP/wolfSSL headers: the whole parse runs
// out of fixed stack arrays so it can be built and tested on the host, and so it
// costs the firmware nothing in internal RAM (~50 KB free, of which the display
// needs 45 KB contiguous).
//
// The input must already be UTF-8. The live response is GB18030; transcoding is
// the app layer's job (TxtEncoding::transcodeGbkInPlace), because that is where the
// firmware's transcoder and its scratch buffer live.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

// Field-for-field identical to jjwxc::ShelfEntry (JjwxcProtocol.h), and therefore
// trivially copyable to it.
//
// It is redeclared rather than included because JjwxcProtocol.h pulls in
// <StreamingJsonParser.h> and <memory>, and this parser needs neither: dragging the
// JSON SAX library into a pure HTML scanner would put it on the include path of
// every host test for no reason. The integration point (where both headers are
// visible) should carry
//   static_assert(sizeof(jjwxc::WapShelfEntry) == sizeof(jjwxc::ShelfEntry));
// before memcpy-ing between them, so a future field added to only one of the two
// fails the build instead of silently shifting offsets.
struct WapShelfEntry {
  char novelId[16] = {};
  char novelName[96] = {};
  char authorName[48] = {};
  // Not present on the WAP shelf page. Left empty; filled by the caller from
  // app-cdn's anonymous novelbasicinfo.
  char coverUrl[192] = {};
  char lastChapterName[64] = {};
  // Not present on the WAP shelf page. Left zero; filled by the caller from
  // app-cdn's anonymous novelbasicinfo / chapterList.
  uint32_t chapterCount = 0;
  uint32_t lastChapterId = 0;
  // Unix seconds, taken from the row's `chapterlastposttime` attribute. 0 when the
  // attribute is absent or is not a form this parser recognises -- see
  // parseShelfTimestamp() below for exactly which forms those are.
  uint32_t lastChapterTime = 0;
  // Non-zero when the row carries the site's own "has updates" marker.
  uint8_t renewStatus = 0;
};

// Same shape as ShelfSink in JjwxcProtocol.h: a plain function pointer plus a void*
// context, never std::function. `index` is 0-based over the rows this call emitted.
// Returning false stops the walk immediately and parseWapShelf() returns the count
// emitted so far, which is how a caller with a full buffer says "enough".
using WapShelfSink = bool (*)(void* ctx, const WapShelfEntry& entry, size_t index);

// What the page's pager said. All three are 0 when the page carries no pager at
// all, which is the normal case for a single-page shelf.
struct WapShelfPage {
  // The page this document claims to be. Taken from the highlighted pager entry
  // (`background-color:blue`), or from `requestedPage` when the caller supplied one.
  // 0 means it could not be determined, in which case `next` is forced to 0 rather
  // than guessed at: guessing would send the caller back to a page it has already
  // read, and a shelf that re-reads page 1 forever looks exactly like a hang.
  uint32_t current = 0;
  // The page to fetch next, or 0 when there is none.
  uint32_t next = 0;
  // The highest page number the pager links to, i.e. the last page. 0 when unknown.
  // Note this is the highest number LINKED, which the observed pager reaches via an
  // explicit "last page" entry; a pager that only ever showed `prev 1 2 3 next`
  // would report 3 here, not the true total.
  uint32_t max = 0;
};

// Parses one `/my/fav` document. Returns the number of entries handed to `sink`.
//
// `pageOut` may be null. `requestedPage` is the page number the caller asked the
// server for (1-based); pass 0 to let the parser read it off the highlighted pager
// entry. Supplying it is the more robust route -- the caller knows what it
// requested, while the highlight is only ever observed on one capture.
//
// Returns 0 for a document with no `favul`, which is also what an expired session
// looks like: `/my/fav` answers a dead `sid` with a 302 to `/my/login`, so the
// caller must treat "0 entries" as ambiguous between "empty shelf" and "not logged
// in" and resolve it from the HTTP status, not from this return value.
size_t parseWapShelf(const char* html, size_t len, WapShelfSink sink, void* ctx, WapShelfPage* pageOut = nullptr,
                     uint32_t requestedPage = 0);

// Parses one `/search?kw=<percent-encoded UTF-8>` document. Same row contract and
// same sink as parseWapShelf(), and it shares parseRow() rather than carrying a
// second row reader: a search row is a strict subset of a shelf row.
//
//   <ul>                                                   <!-- NO id attribute -->
//     <li>
//       <a data-recommendInfo='{"recommendid":"6acfaaff960c4","readerid":0,...}'
//          class="track" href="/book2/NNNNNNN">TITLE</a>
//       - <a href="/wapauthor/NNNNNNN">AUTHOR</a>
//     </li>
//     ... 20 of them ...
//     <li>
//       <p class="pagination">
//         <a href="https://m.jjwxc.net/search/index/page/2?kw=%E4%BF%AE%E7%9C%9F">NEXT</a>
//         <strong>1</strong>                               <!-- the current page -->
//         <a href="https://m.jjwxc.net/search/index/page/2?kw=%E4%BF%AE%E7%9C%9F">2</a>
//         PREV                                             <!-- bare text: page 1 -->
//       </p>
//     </li>
//   </ul>
//   <input type="hidden" id="max_page" value="2">
//
// Measured against a live ANONYMOUS capture (no cookies, no account): HTTP 200,
// 20,957 B as served, `charset=gb18030`; one `<ul>`, 21 `<li>` inside it, 20
// `/book2/` and 20 `/wapauthor/` hrefs, all 20 novel ids distinct, and the 21st row
// is the pager.
//
// What differs from the shelf, and why none of the shelf's list/pager code is reused:
//
//   - The list has no `id`, so findFavul() has nothing to key on. It is located here
//     as the first `<ul>` whose content holds a `/book2/` href -- by evidence inside
//     the element, not by position, so a nav list added above it later is skipped.
//   - Only `novelId`, `novelName` and `authorName` are populated. This page carries
//     no cover URL, no chapter count, no last chapter and no `chapterlastposttime`
//     attribute, so `lastChapterTime` and `renewStatus` stay 0 too. Same rule as the
//     shelf: a field the page did not carry is left empty rather than invented,
//     because an invented cover URL renders as a broken thumbnail. The caller fills
//     them from app-cdn's anonymous `novelbasicinfo`, exactly as for the shelf.
//   - The pager hrefs are ABSOLUTE (`https://m.jjwxc.net/search/index/page/N?...`),
//     where the shelf's are relative (`/my/fav?p=N`). Both nextPageLink() and
//     detectCurrentPage() hard-code `/my/fav?p=`, so neither sees this pager at all:
//     they return "no links" and 0, which would silently report next=0 on every page
//     of results and stop the caller after page 1. Hence the search-specific pager
//     below; their shelf behaviour stays pinned by JjwxcWapShelfTest.
//   - The current page is a bare `<strong>1</strong>`, not the shelf's
//     `background-color:blue` span. The capture contains ZERO occurrences of
//     `background-color`, so there is no highlight to find even in principle.
//   - `/search` is ANONYMOUS, so 0 here genuinely means "no matches" (or a page this
//     parser does not recognise). Unlike `/my/fav`, which answers a dead `sid` with a
//     302 to `/my/login` and therefore makes 0 ambiguous between "empty shelf" and
//     "not logged in", this endpoint cannot fail that way.
//   - `max` prefers the page's own `<input id="max_page">` total when present, and
//     falls back to the highest page number linked. That is strictly better than the
//     shelf's highest-linked rule, which under-reports whenever the pager renders a
//     short window; it is not a guess, it is the value the page's own JavaScript
//     reads to build its pager hrefs.
//
// ★ The pager scan is scoped to the result list on purpose. The page renders a sort
// bar ABOVE the `<ul>` holding eight more `/search/index/page/` links -- same path as
// the pager, one per sort order, all pointing at the CURRENT page -- so a
// document-wide scan would read the sort bar as pager entries. Those hrefs are also
// the one place the page is malformed: they are UNQUOTED (`href=/search/...`) and
// their `kw` carries raw UTF-8 bytes (`\xE4\xBF\xAE\xE7\x9C\x9F`) inside a document
// that declares gb18030, so they transcode to mojibake and attrValue() cannot read
// them (an unquoted value stops at the first `/`). Nothing here looks at them.
//
// Also do not depend on any particular query string. An earlier capture of this page
// had `&t=1&o=<sort>` on the pager hrefs; this one has none. Only the
// `/search/index/page/<digits>` prefix is read, which both forms share.
size_t parseWapSearch(const char* html, size_t len, WapShelfSink sink, void* ctx, WapShelfPage* pageOut = nullptr,
                      uint32_t requestedPage = 0);

// --- exposed for tests -------------------------------------------------------

// `chapterlastposttime` / `readingorbookmarktime` attribute value -> unix seconds.
//
// The live capture settles the format question: all 30 rows carry a bare 10-digit
// decimal string, already unix seconds, so the normal path is a digit scan with no
// calendar arithmetic and therefore no timezone to get wrong. The page's own
// JavaScript compares these two attributes numerically, which corroborates it.
//
// A `YYYY-MM-DD[ HH:MM[:SS]]` string is also accepted, interpreted as UTC via
// Howard Hinnant's civil-days formula. That is the convention this repo already
// uses in three places (src/util/TimeUtils.cpp, lib/hal/HalClock.cpp and
// freeink-sdk/libs/hardware/Rtc/src/Rtc.cpp), and the reason is written down there:
// `mktime()` is local-time and `timegm()` is not declared by the newlib `<time.h>`
// this project builds against, so a civil-days formula is the only
// timezone-independent route. The device has no meaningful local timezone anyway.
//
// Limitation of that fallback, stated plainly because it is a real bias: JJWXC is a
// UTC+8 site, so a date-only value is CST wall-clock midnight read as UTC midnight,
// which is 8 hours earlier than the instant it names. It is only reached for a form
// the live page does not emit, and it is deliberately NOT "fixed" by adding 8h --
// that would be a guess about a format nobody has observed. Anything unrecognised
// returns 0, matching the "leave it zero rather than invent it" rule above.
uint32_t parseShelfTimestamp(const char* s, size_t len);

// Number of `/book2/` anchors in one row. Exposed so a test can pin the "two
// anchors, one record" rule to a synthetic row instead of only to the live capture.
size_t countShelfRowLinks(const char* li, size_t len);

}  // namespace jjwxc
