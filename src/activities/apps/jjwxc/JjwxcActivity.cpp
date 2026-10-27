#include "JjwxcActivity.h"

#include "JjwxcCommentActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <TxtEncoding.h>

#include <Arduino.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <strings.h>
#include <type_traits>

#include "CrossPointState.h"
#include "NetworkStartup.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/listIcons48.h"
#include "components/themes/cpy/CpyTheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

#include "JjwxcEpub.h"
#include "JjwxcUpdates.h"
#include "JjwxcPayKey.h"
#include "JjwxcPurchaseConfirmActivity.h"

#include <JjwxcComments.h>
#include <JjwxcCrypto.h>
#include <JjwxcDeviceAuth.h>
#include <JjwxcFavorites.h>
#include <JjwxcFontMap.h>
#include <JjwxcProgress.h>
#include <JjwxcPurchase.h>
#include <JjwxcWap.h>
#include <JjwxcWapLogin.h>

#ifdef ARDUINO
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#endif

namespace {

constexpr const char* kTag = "JJWXC";

// JJWXC is an application screen, not a reading surface: use the same UI face
// ladder as Settings/Home. The legacy NOTOSANS_* reader ids all resolve to the
// single embedded 12 pt CJK fallback, so using them here mixed 12 pt titles with
// 14/16 pt labels and ignored the SD UI font entirely.
constexpr int kBodyFont = UI_12_FONT_ID;
constexpr int kSmallFont = SMALL_FONT_ID;
constexpr int kListFont = UI_10_FONT_ID;

// Gap between the header-adjacent info block and the first list row. Rule #14
// wants at least 6 px of visible space between interactive controls; the block
// itself is inert text, and 12 px keeps it clear of the selection background.
constexpr int kInfoBlockGap = 12;
// Detail data band (design frame 11): three big numbers above the fact lines.
constexpr int kDetailBandH = 112;
constexpr int kDetailBandGap = 10;

void drawDetailIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y,
                    const int size) {
  const int rowBytes = (icon.w + 7) / 8;
  for (int row = 0; row < size; ++row) {
    const int sourceY = row * icon.h / size;
    const uint8_t* sourceRow = icon.bits + sourceY * rowBytes;
    for (int column = 0; column < size; ++column) {
      const int sourceX = column * icon.w / size;
      if (((sourceRow[sourceX / 8] >> (7 - (sourceX & 7))) & 1U) == 0) {
        renderer.drawPixel(x + column, y + row, true);
      }
    }
  }
}
// CPY frame 12: the double rule between the pill band and the blurb, with its
// gaps (5px rule block + air on both sides).
constexpr int kCpyBlurbRuleBlock = 17;
// Longest composed info-block line: label + a 96-byte NovelInfo text field.
constexpr size_t kInfoLineBytes = 192;
// Longest one wrapped blurb line held on the stack while it is drawn. At
// NOTOSANS_14 a 684-px panel fits ~40 Chinese characters (120 bytes) and an 800-px
// one ~50 (150 bytes), so this is 2.5x the realistic worst case. A line that did
// not fit would be cut on a code-point boundary by copyWrappedLine() rather than
// split mid-character, and the next line still starts where the measurement said it
// would, so nothing is duplicated or lost -- only that one line comes up short.
constexpr size_t kBlurbLineBytes = 384;
// Appended to the last visible blurb line when the rest did not fit.
constexpr const char* kEllipsis = "\xE2\x80\xA6";

// Same 5% bucketing WeRead uses for chapter progress
// (WeReadActivity.cpp:102-106): a 300-chapter download then asks the panel for
// ~20 refreshes instead of 300.
constexpr uint8_t progressBucket(const size_t completed, const size_t total, const uint8_t bucketCount) {
  if (total == 0 || bucketCount == 0) return 0;
  const uint64_t bucket = (static_cast<uint64_t>(completed) * bucketCount) / total;
  return static_cast<uint8_t>(std::min<uint64_t>(bucket, bucketCount));
}

static_assert(progressBucket(0, 100, 20) == 0);
static_assert(progressBucket(4, 100, 20) == 0);
static_assert(progressBucket(5, 100, 20) == 1);
static_assert(progressBucket(99, 100, 20) == 19);
static_assert(progressBucket(100, 100, 20) == 20);
static_assert(progressBucket(7, 0, 20) == 0);

// Parses a keyboard entry as a 1-based chapter ordinal. Anything that is not a
// plain positive decimal is rejected rather than partially interpreted, so "12a"
// cannot silently become 12.
int parseOrdinal(const std::string& text, const int maximum) {
  if (text.empty() || text.size() > 7) return -1;
  int value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return -1;
    value = value * 10 + (c - '0');
  }
  if (value < 1 || value > maximum) return -1;
  return value;
}

// Truncates a UTF-8 string in place so it fits `maxWidth` px, never splitting a
// code point. renderer.truncatedText() would do this but returns a std::string,
// and this runs inside render(), which must stay allocation-free.
void truncateToWidth(const GfxRenderer& renderer, const int fontId, char* buf, const int maxWidth) {
  if (buf == nullptr) return;
  if (maxWidth <= 0 || buf[0] == '\0') {
    buf[0] = '\0';
    return;
  }
  const size_t total = strlen(buf);
  int width = 0;
  size_t i = 0;
  while (i < total) {
    // Length of the code point starting at i, from its lead byte.
    const auto lead = static_cast<unsigned char>(buf[i]);
    size_t len = 1;
    if (lead >= 0xF0) len = 4;
    else if (lead >= 0xE0) len = 3;
    else if (lead >= 0xC0) len = 2;
    if (i + len > total) len = total - i;  // truncated tail: take what is left

    char glyph[8] = {};
    if (len >= sizeof(glyph)) break;
    memcpy(glyph, buf + i, len);
    const int advance = renderer.getTextAdvanceX(fontId, glyph, EpdFontFamily::REGULAR);
    if (width + advance > maxWidth) {
      buf[i] = '\0';
      return;
    }
    width += advance;
    i += len;
  }
}

// Copies `len` bytes of a wrapped line into `dst`, NUL-terminating it and backing
// off to a code-point boundary if the buffer is narrower than the line.
// measureWrappedLine() already guarantees a clean boundary at `len`; this covers the
// other side, where a very wide panel could measure a line longer than the stack
// buffer. Half a UTF-8 sequence reaching drawText() renders as a box, which is the
// one failure this app's text handling is not allowed to have.
void copyWrappedLine(char* dst, const size_t dstSize, const char* src, const size_t len) {
  if (dst == nullptr || dstSize == 0) return;
  size_t n = len < dstSize - 1 ? len : dstSize - 1;
  // A continuation byte is 0b10xxxxxx; stepping back while the first dropped byte is
  // one lands on a lead byte after at most three steps.
  while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// Marks a line whose continuation was left out, by cutting the line back by exactly what
// the mark will add and then appending it. Both the fact column and the blurb use it: the
// line was measured WITHOUT the mark, so appending it without shortening the text would
// overflow the column by one glyph.
void appendEllipsis(const GfxRenderer& renderer, const int fontId, char* buf, const size_t bufSize,
                    const int maxWidth) {
  if (buf == nullptr || bufSize == 0 || buf[0] == '\0') return;
  const int markWidth = renderer.getTextWidth(fontId, kEllipsis);
  if (markWidth <= 0 || maxWidth <= markWidth) return;
  truncateToWidth(renderer, fontId, buf, maxWidth - markWidth);
  const size_t used = strlen(buf);
  const size_t markLen = strlen(kEllipsis);
  if (used + markLen < bufSize) memcpy(buf + used, kEllipsis, markLen + 1);
}

// Keeps both ends of a title that does not fit, because a tail-only cut makes two
// books whose names share a long prefix ("重生之…（一）" / "（二）") render as the
// same string. The split is half for the head and half for the tail; the mark in
// the middle is what says a cut happened.
void copyMiddleEllipsis(const GfxRenderer& renderer, const int fontId, const char* src, char* out,
                        const size_t outSize, const int maxWidth,
                        const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (out == nullptr || outSize == 0) return;
  out[0] = '\0';
  if (src == nullptr || src[0] == '\0' || maxWidth <= 0) return;
  const size_t total = strlen(src);
  if (renderer.getTextWidth(fontId, src, style) <= maxWidth) {
    const size_t n = std::min(total, outSize - 1);
    memcpy(out, src, n);
    out[n] = '\0';
    return;
  }
  const int markWidth = renderer.getTextWidth(fontId, kEllipsis);
  if (markWidth <= 0 || maxWidth <= markWidth) {
    // Too narrow for a mark: keep the head, which is still better than nothing.
    snprintf(out, outSize, "%s", src);
    truncateToWidth(renderer, fontId, out, maxWidth);
    return;
  }
  const int usable = maxWidth - markWidth;
  const int tailBudget = usable / 2;
  const int headBudget = usable - tailBudget;

  // Walk backwards over whole code points until the tail budget is used up.
  size_t tailStart = total;
  int tailWidth = 0;
  while (tailStart > 0) {
    size_t cpStart = tailStart - 1;
    while (cpStart > 0 && (static_cast<unsigned char>(src[cpStart]) & 0xC0) == 0x80) --cpStart;
    const size_t len = tailStart - cpStart;
    if (len >= 8) break;
    char glyph[8] = {};
    memcpy(glyph, src + cpStart, len);
    const int advance = renderer.getTextAdvanceX(fontId, glyph, style);
    if (tailWidth + advance > tailBudget) break;
    tailWidth += advance;
    tailStart = cpStart;
  }

  size_t headLen = 0;
  int headWidth = 0;
  while (headLen < tailStart) {
    const auto lead = static_cast<unsigned char>(src[headLen]);
    size_t len = 1;
    if (lead >= 0xF0) len = 4;
    else if (lead >= 0xE0) len = 3;
    else if (lead >= 0xC0) len = 2;
    if (headLen + len > tailStart) break;
    if (len >= 8) break;
    char glyph[8] = {};
    memcpy(glyph, src + headLen, len);
    const int advance = renderer.getTextAdvanceX(fontId, glyph, style);
    if (headWidth + advance > headBudget) break;
    headWidth += advance;
    headLen += len;
  }
  if (headLen == 0 || tailStart >= total) {
    // A single glyph is wider than half the line: keep the head and mark it.
    snprintf(out, outSize, "%s", src);
    appendEllipsis(renderer, fontId, out, outSize, maxWidth);
    return;
  }
  size_t used = std::min(headLen, outSize - 1);
  memcpy(out, src, used);
  const size_t markLen = strlen(kEllipsis);
  if (used + markLen + (total - tailStart) < outSize) {
    memcpy(out + used, kEllipsis, markLen);
    used += markLen;
    memcpy(out + used, src + tailStart, total - tailStart);
    used += total - tailStart;
  }
  out[used] = '\0';
}

// The catalogue sink needs both the activity (for the in-memory list) and the
// merger that is streaming toc.bin, so it gets one context carrying both. The
// merger lives on the worker task's stack for the duration of the fetch.
struct ChapterListCtx {
  JjwxcActivity* self;
  JjwxcStore::TocMerger* merger;
};

// Bridges jjwxc::fetchChapter's text sink onto the XHTML writer for ONE chapter.
struct ChapterSinkCtx {
  JjwxcEpub::ChapterWriter* writer;
  bool failed = false;
};

struct CoverSinkCtx {
  HalFile* file = nullptr;
  size_t bytes = 0;
  bool failed = false;
};

struct CoverHeaderCtx {
  // Longest cover URL seen in NovelInfo::coverUrl is 191 bytes; a redirect target
  // gets the same budget.
  char location[256] = {};
};

// --- discovery sources -------------------------------------------------------
//
// Every path was fetched from the device's own session on 2026-10-17 and
// answered 200 with a book list. Paths (not full URLs) so the host constant is
// the one place the scheme lives; the WAP client is HTTPS-only.
struct DiscoverSource {
  const char* path;
  StrId label;
};

constexpr DiscoverSource kDiscoverSources[] = {
    {"/channel/index/gy", StrId::STR_JJWXC_CH_GY},
    {"/channel/index/xy", StrId::STR_JJWXC_CH_XY},
    {"/channel/index/dm", StrId::STR_JJWXC_CH_DM},
    {"/channel/index/tr", StrId::STR_JJWXC_CH_TR},
    {"/channel/index/nocp_plus", StrId::STR_JJWXC_CH_NOCP},
    {"/channel/channel/bhxs", StrId::STR_JJWXC_CH_BH},
    {"/endnovel", StrId::STR_JJWXC_ENDED},
    {"/pindao/vip", StrId::STR_JJWXC_ALL_BOOKS},
    {"/endnovel/index/half", StrId::STR_JJWXC_MONTHLY},
    {"/pindao/vip?type=dp", StrId::STR_JJWXC_SHORT_STORY},
    {"/free", StrId::STR_JJWXC_FREE},
    {"/rank", StrId::STR_JJWXC_RANKING},
    {"/ranks/vipgold/yq", StrId::STR_JJWXC_GOLD_RANK},
    {"/ranks/recommend/yq", StrId::STR_JJWXC_RECOMMEND_RANK},
};

constexpr size_t kDiscoverSourceCount = sizeof(kDiscoverSources) / sizeof(kDiscoverSources[0]);

// /assort's sortType values, in the order the site renders them. The label is
// what the form row shows; `value` is what goes into the URL.
struct AdvSortOption {
  int value;
  StrId label;
};

constexpr AdvSortOption kAdvSorts[] = {
    {2, StrId::STR_JJWXC_SORT_POINTS},   {1, StrId::STR_JJWXC_SORT_UPDATED},      {3, StrId::STR_JJWXC_SORT_NEWEST},
    {5, StrId::STR_JJWXC_SORT_WORDS},    {4, StrId::STR_JJWXC_SORT_FAVORITES},    {10, StrId::STR_JJWXC_SORT_FINISHED_SCORE},
    {9, StrId::STR_JJWXC_SORT_RANDOM},
};

constexpr size_t kAdvSortCount = sizeof(kAdvSorts) / sizeof(kAdvSorts[0]);

// tr() is a macro over an identifier (`StrId::id`); a label stored in a table is
// already a StrId value, so it needs the lookup directly.
const char* trId(const StrId id) { return I18n::getInstance().get(id); }

// Resolves the tag NAMES the user typed into the numeric ids /assort's form
// actually posts. Done while streaming the dictionary, so no tag table has to be
// kept: the page carries 279 tags and storing them would be ~12 KB of stack for
// a lookup that runs once per search.
constexpr size_t kMaxAdvTags = 4;  // the site itself refuses more than four with AND

struct TagResolveCtx {
  const char* include = nullptr;
  const char* exclude = nullptr;
  uint8_t includeFound = 0;  // bit per token
  uint8_t excludeFound = 0;
  uint8_t includeCount = 0;
  uint8_t excludeCount = 0;
  char includeIds[64] = {};
  char excludeIds[64] = {};
  char missing[32] = {};
};

// Iterates comma-separated tokens, trimming ASCII spaces. Returns false at end.
bool nextToken(const char*& p, char* out, size_t cap) {
  if (p == nullptr || *p == '\0') return false;
  while (*p == ' ') ++p;
  const char* start = p;
  while (*p != '\0' && *p != ',') ++p;
  const char* end = p;
  while (end > start && end[-1] == ' ') --end;
  if (*p == ',') ++p;
  const size_t n = static_cast<size_t>(end - start);
  if (n == 0 || n + 1 > cap) {
    out[0] = '\0';
    return *p != '\0' || n > 0;
  }
  memcpy(out, start, n);
  out[n] = '\0';
  return true;
}

uint8_t countTokens(const char* list) {
  if (list == nullptr || list[0] == '\0') return 0;
  const char* p = list;
  char token[32];
  uint8_t n = 0;
  while (nextToken(p, token, sizeof(token))) {
    if (token[0] != '\0') ++n;
    if (n >= kMaxAdvTags) break;
  }
  return n;
}

bool tokenIndexForName(const char* list, const char* name, uint8_t* indexOut, uint8_t* countOut) {
  if (list == nullptr || list[0] == '\0') return false;
  const char* p = list;
  char token[32];
  uint8_t i = 0;
  while (nextToken(p, token, sizeof(token))) {
    if (i >= kMaxAdvTags) break;
    if (token[0] != '\0' && strcmp(token, name) == 0) {
      *indexOut = i;
      if (countOut != nullptr) *countOut = static_cast<uint8_t>(i + 1);
      return true;
    }
    ++i;
  }
  return false;
}

void appendId(char* out, size_t cap, const char* id) {
  const size_t used = strlen(out);
  if (used > 0 && used + 1 < cap) {
    out[used] = ',';
    out[used + 1] = '\0';
  }
  snprintf(out + strlen(out), cap - strlen(out), "%s", id);
}

bool resolveTagSink(void* ctx, const jjwxc::AssortTag& tag, size_t) {
  auto* c = static_cast<TagResolveCtx*>(ctx);
  uint8_t idx = 0;
  if (tokenIndexForName(c->include, tag.name, &idx, &c->includeCount)) {
    c->includeFound |= static_cast<uint8_t>(1u << idx);
    appendId(c->includeIds, sizeof(c->includeIds), tag.id);
  }
  if (tokenIndexForName(c->exclude, tag.name, &idx, &c->excludeCount)) {
    c->excludeFound |= static_cast<uint8_t>(1u << idx);
    appendId(c->excludeIds, sizeof(c->excludeIds), tag.id);
  }
  return true;
}

// Copies the first unresolved token into `missing`.
void firstMissing(const char* list, const uint8_t found, const uint8_t count, char* missing, size_t cap) {
  if (list == nullptr || list[0] == '\0') return;
  const char* p = list;
  char token[32];
  uint8_t i = 0;
  while (nextToken(p, token, sizeof(token)) && i < count) {
    if (token[0] != '\0' && (found & (1u << i)) == 0) {
      snprintf(missing, cap, "%s", token);
      return;
    }
    ++i;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Worker task
//
// A chapter download can take tens of seconds. Running it inside loop() would
// stop main.cpp from calling yield(), so the idle task on this core would starve
// and the task watchdog would fire. Everything that touches the network or the
// SD card therefore runs on a pinned worker task; loop() only polls jobDone_.
// ---------------------------------------------------------------------------

namespace {

// Collects a whole response into a caller-owned buffer, NUL-terminating it. Used by
// the WAP paths, which all hand their answer to a parser that wants a complete
// document rather than a stream: the verification-code JSON (a few hundred bytes) and
// the shelf and chapter pages (tens of kilobytes, transcoded in place afterwards).
struct BufferCollector {
  char* buf;
  size_t cap;
  size_t len;
  bool overflow;
};

bool collectBuffer(void* ctx, const uint8_t* data, size_t len) {
  auto* c = static_cast<BufferCollector*>(ctx);
  const size_t room = c->cap > c->len + 1 ? c->cap - c->len - 1 : 0;
  const size_t n = len < room ? len : room;
  if (n > 0) memcpy(c->buf + c->len, data, n);
  c->len += n;
  c->buf[c->len] = '\0';
  if (n < len) c->overflow = true;
  // Keep receiving: an over-long answer still has to be reported as one, and aborting
  // would surface as a transport error with no hint that the buffer was the problem.
  return true;
}

// The task never deletes itself. The owning activity deletes the handle exactly
// once in reapJob(), which removes the race between self-deletion and a forced
// delete of the same handle -- and any code after vTaskDelete(nullptr) would be
// unreachable anyway.
void jobTrampoline(void* param) {
  auto* self = static_cast<JjwxcActivity*>(param);
  self->runJobBody();
  self->signalJobDone();
  vTaskSuspend(nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// Reader deep link
// ---------------------------------------------------------------------------

JjwxcActivity::DeepLinkAction JjwxcActivity::pendingDeepLinkAction_ = JjwxcActivity::DeepLinkAction::None;
JjwxcActivity::MenuShortcut JjwxcActivity::pendingMenuShortcut_ = JjwxcActivity::MenuShortcut::None;
char JjwxcActivity::pendingDeepLinkNovelId_[16] = {};
char JjwxcActivity::pendingDeepLinkQuery_[32] = {};
uint32_t JjwxcActivity::pendingDeepLinkChapterId_ = 0;
bool JjwxcActivity::pendingDeepLinkOpenChapter_ = false;

void JjwxcActivity::requestDeepLink(const char* novelId, const DeepLinkAction action, const uint32_t chapterId,
                                    const bool openChapter) {
  pendingDeepLinkAction_ = DeepLinkAction::None;
  pendingDeepLinkNovelId_[0] = '\0';
  pendingDeepLinkChapterId_ = 0;
  pendingDeepLinkOpenChapter_ = false;
  pendingDeepLinkQuery_[0] = '\0';
  const bool needsBook = action != DeepLinkAction::RefreshUpdates && action != DeepLinkAction::Search;
  if (action == DeepLinkAction::None) return;
  if (needsBook && (novelId == nullptr || novelId[0] == '\0')) return;
  pendingDeepLinkAction_ = action;
  snprintf(pendingDeepLinkNovelId_, sizeof(pendingDeepLinkNovelId_), "%s", novelId ? novelId : "");
  pendingDeepLinkChapterId_ = chapterId;
  pendingDeepLinkOpenChapter_ = openChapter;
}

void JjwxcActivity::requestSearchDeepLink(const char* query) {
  pendingDeepLinkAction_ = DeepLinkAction::None;
  pendingDeepLinkNovelId_[0] = '\0';
  pendingDeepLinkChapterId_ = 0;
  pendingDeepLinkOpenChapter_ = false;
  pendingDeepLinkQuery_[0] = '\0';
  if (query == nullptr || query[0] == '\0') return;
  pendingDeepLinkAction_ = DeepLinkAction::Search;
  snprintf(pendingDeepLinkQuery_, sizeof(pendingDeepLinkQuery_), "%s", query);
}

void JjwxcActivity::requestMenuShortcut(const MenuShortcut shortcut) {
  pendingMenuShortcut_ = shortcut;
}

void JjwxcActivity::finishDeepLink(const DeepLinkAction action, const bool completed, const bool readable) {
  JjwxcReaderResult::Action readerAction = JjwxcReaderResult::Action::None;
  switch (action) {
    case DeepLinkAction::UpdateChapterList:
      readerAction = JjwxcReaderResult::Action::ChapterList;
      break;
    case DeepLinkAction::UpdateCurrentChapter:
      readerAction = JjwxcReaderResult::Action::CurrentChapter;
      break;
    case DeepLinkAction::CacheChapter:
      readerAction = JjwxcReaderResult::Action::ChapterCached;
      break;
    case DeepLinkAction::PurchaseChapter:
      readerAction = JjwxcReaderResult::Action::ChapterPurchased;
      break;
    case DeepLinkAction::OpenBookDetail:
    case DeepLinkAction::RefreshUpdates:  // the Library reads `completed`, not an action
    case DeepLinkAction::None:
      break;
  }
  ActivityResult result;
  result.data = JjwxcReaderResult{readerAction, completed, deepLinkChapterId_, readable, deepLinkOpenChapter_};
  setResult(std::move(result));
  finish();
}

// ---------------------------------------------------------------------------
// WAP session
// ---------------------------------------------------------------------------

uint64_t JjwxcActivity::cookieClock() {
  const time_t t = time(nullptr);
  // 2020-01-01. An ESP32 with no NTP yet reports seconds since its own boot, or a
  // stale RTC value, and either is a small non-zero number. Passing that to the jar
  // would compare it against a sid whose Max-Age was stamped from a real clock and
  // find it expired, which surfaces as "logged out" on every cold boot -- the exact
  // failure the jar's nowUnix == 0 convention exists to prevent.
  if (t < 1577836800) return 0;
  return static_cast<uint64_t>(t);
}

bool JjwxcActivity::ensureWapSession() {
  if (wap_ != nullptr && jar_ != nullptr && wapIo_ != nullptr) {
    wap_->setNowUnix(cookieClock());
    return true;
  }

  // makePsramByteBufferNoThrow already carries the BOARD_HAS_PSRAM / heap_caps guards
  // and falls back to calloc, so this does not need its own #if. The objects are
  // placement-new'd into it and released by hand in releaseWapSession(); free() is the
  // documented owner release for a heap_caps allocation.
  if (jar_ == nullptr) {
    auto buf = memory::makePsramByteBufferNoThrow(sizeof(jjwxc::CookieJar));
    if (!buf) {
      LOG_ERR(kTag, "no PSRAM for the cookie jar (%u bytes)", (unsigned)sizeof(jjwxc::CookieJar));
      return false;
    }
    jar_ = new (buf.release()) jjwxc::CookieJar();
  }
  if (wapIo_ == nullptr) {
    auto buf = memory::makePsramByteBufferNoThrow(sizeof(jjwxc::WapIo));
    if (!buf) {
      LOG_ERR(kTag, "no PSRAM for the WAP io buffers (%u bytes)", (unsigned)sizeof(jjwxc::WapIo));
      return false;
    }
    wapIo_ = new (buf.release()) jjwxc::WapIo();
  }
  if (wap_ == nullptr) {
    auto client = makeUniqueNoThrow<jjwxc::WapClient>(transport_, *jar_, *wapIo_);
    if (!client) {
      LOG_ERR(kTag, "no RAM for the WAP client");
      return false;
    }
    wap_ = client.release();
  }
  wap_->setNowUnix(cookieClock());
  return true;
}

void JjwxcActivity::releaseWapSession() {
  if (wap_ != nullptr) {
    delete wap_;
    wap_ = nullptr;
  }
  // Placement-new'd, so the destructor runs by hand and the memory goes back through
  // the same release the allocator documents.
  if (wapIo_ != nullptr) {
    wapIo_->~WapIo();
    free(wapIo_);
    wapIo_ = nullptr;
  }
  if (jar_ != nullptr) {
    jar_->~CookieJar();
    free(jar_);
    jar_ = nullptr;
  }
}

bool JjwxcActivity::haveSession() const {
  return jar_ != nullptr && JjwxcStore::hasSession(*jar_, cookieClock());
}

void JjwxcActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  // Reclaims the SD font and the renderer font cache when PSRAM/internal
  // headroom is low, before the Wi-Fi driver starts allocating. This board needs
  // ~45 KB of *contiguous* internal RAM for the display DMA and a dead panel
  // reports nothing, so the same call WeRead makes on entry
  // (WeReadActivity.cpp:482) applies here: it is a no-op while headroom is
  // healthy (NetworkStartup.cpp:71-82).
  NetworkStartup::prepare(renderer);

  if (!scratch_) {
    // Try the full region, then the fallback. The VIP chain needs the page and its
    // UTF-8 form in one buffer at once, so a scratch that is merely "some PSRAM" is not
    // usable; fetchWapVipChapter checks against kWapVipScratchBytes and says so.
    size_t want = kScratchBytes;
    while (want >= kScratchBytesFallback) {
      auto buf = memory::makePsramByteBufferUninitializedNoThrow(want);
      if (buf) {
        scratch_ = buf.release();
        scratchCap_ = want;
        if (want != kScratchBytes) LOG_ERR(kTag, "PSRAM scratch reduced to %u bytes", (unsigned)want);
        break;
      }
      LOG_ERR(kTag, "PSRAM scratch alloc failed at %u bytes", (unsigned)want);
      want = kScratchBytesFallback;
      if (scratchCap_ != 0) break;
    }
  }
  JjwxcStore::ensureRoot();
  // One-shot migration off the pre-EPUB TXT products. Nothing reads them any more
  // and the reader progress they anchored is discarded by design, so this only
  // reclaims card space. It is idempotent (a missing directory is success) and a
  // failure here must never keep the app from starting.
  if (!JjwxcStore::cleanupLegacyBooks()) {
    LOG_ERR(kTag, "legacy TXT cleanup incomplete; will retry on next entry");
  }
  JjwxcStore::loadCredentials(cred_);
  // The WAP session is a separate file from `credentials` and is what "logged in"
  // means now: sid is 133 characters and Credentials::token is 48. A failure here is
  // not an error worth showing -- it means "no session", which the account page
  // already offers a way out of.
  if (ensureWapSession()) {
    if (JjwxcStore::loadCookies(*jar_, cookieClock())) {
      LOG_INF(kTag, "WAP session restored: %u cookies, readerId=%lu", (unsigned)jar_->count(cookieClock()),
              (unsigned long)jar_->readerIdFromSid(cookieClock()));
    } else {
      LOG_INF(kTag, "no persisted WAP session");
    }
  }
  // The fingerprint outlives the session: a logout removes `credentials` and must
  // not cost us the identity that stops JJWXC treating every login as a new device.
  JjwxcStore::DeviceIdentity device;
  if (JjwxcStore::loadDevice(device)) {
    if (!cred_.hasIdentifiers()) snprintf(cred_.identifiers, sizeof(cred_.identifiers), "%s", device.identifiers);
    if (cred_.readerId[0] == '\0') snprintf(cred_.readerId, sizeof(cred_.readerId), "%s", device.readerId);
  }
  // The login shortcut is a separate file from the session on purpose, so it is
  // read separately: a logout clears the token and deliberately leaves this one.
  if (!JjwxcStore::loadAccount(savedAccount_)) savedAccount_ = JjwxcStore::SavedAccount{};
  // A saved scope is the next default; with nothing saved, preselect the middle
  // option so the password stays opt-in.
  saveScope_ = savedAccount_.valid() ? savedAccount_.scope : JjwxcStore::SaveScope::AccountOnly;
  // Never carry a plaintext password across an entry into the app.
  password_.clear();
  jobAccountPersisted_.store(false);
  // A half-finished verification sequence must not survive leaving the app.
  needDeviceCode_ = false;
  pendingPromptDeviceCode_ = false;
  deviceCodeSent_ = false;

  vipChainProven_ = false;
  busyScreen_ = false;
  sleepHoldoffUntil_ = 0;
  lastProgressBucket_ = 0;
  lastProgressPhase_ = 0;
  dlPhase_ = 0;
  openAfterJob_ = false;
  cacheMode_ = CacheMode::Incremental;
  refreshPending_ = false;
  appendOnlyRun_ = true;
  jobPackageFailed_ = false;
  jobPurchased_ = false;
  jobStoppedAtPurchase_ = false;
  jobStoppedOrdinal_ = 0;
  jobTargetSpine_ = -1;
  dlSkipped_ = 0;
  tokenProbed_ = false;
  tokenLiveness_ = jjwxc::TokenLiveness::Alive;
  probeStatus_ = jjwxc::Status::Ok;
  deepLinkAction_ = DeepLinkAction::None;
  deepLinkReturnAction_ = DeepLinkAction::None;
  deepLinkChapterId_ = 0;
  deepLinkOpenChapter_ = false;
  deepLinkOpenBookDetail_ = false;
  pendingDeepLinkOrdinal_ = -1;
  memset(contentKey_, 0, sizeof(contentKey_));
  contentKeyPath_.clear();
  dlPath_.clear();
  novel_ = JjwxcStore::NovelRecord{};
  intro_.clear();
  tocSummary_ = JjwxcStore::TocSummary{};
  detailLayoutValid_ = false;
  optionPopup_.dismiss();
  optionPopupClosing_ = false;

  enter(JjwxcStore::isDisclaimerAccepted() ? State::Menu : State::Disclaimer);

  // A request from the reader's menu goes straight to the book and then runs one
  // action; it never bypasses the disclaimer.
  const DeepLinkAction requested = pendingDeepLinkAction_;
  char requestedNovel[sizeof(pendingDeepLinkNovelId_)] = {};
  if (requested != DeepLinkAction::None) {
    snprintf(requestedNovel, sizeof(requestedNovel), "%s", pendingDeepLinkNovelId_);
  }
  const uint32_t requestedChapterId = pendingDeepLinkChapterId_;
  const bool requestedOpenChapter = pendingDeepLinkOpenChapter_;
  char requestedQuery[sizeof(pendingDeepLinkQuery_)] = {};
  snprintf(requestedQuery, sizeof(requestedQuery), "%s", pendingDeepLinkQuery_);
  pendingDeepLinkAction_ = DeepLinkAction::None;
  pendingDeepLinkNovelId_[0] = '\0';
  pendingDeepLinkChapterId_ = 0;
  pendingDeepLinkOpenChapter_ = false;
  pendingDeepLinkQuery_[0] = '\0';
  if (requested == DeepLinkAction::Search) {
    if (!JjwxcStore::isDisclaimerAccepted() || requestedQuery[0] == '\0') {
      enter(State::Menu);
      return;
    }
    keyword_ = requestedQuery;
    searchType_ = 1;
    deepLinkAction_ = DeepLinkAction::None;
    deepLinkReturnAction_ = DeepLinkAction::None;
    runSearch();
    return;
  }
  if (requested == DeepLinkAction::RefreshUpdates) {
    // A sync request must never leave the user staring at the app's menu: with
    // no accepted disclaimer (or no session, checked inside runUpdateCheck) the
    // honest answer is "could not check", returned to the caller at once.
    if (!JjwxcStore::isDisclaimerAccepted()) {
      LOG_INF(kTag, "update check skipped: disclaimer not accepted");
      finishDeepLink(DeepLinkAction::RefreshUpdates, false);
      return;
    }
    deepLinkAction_ = DeepLinkAction::RefreshUpdates;
    deepLinkReturnAction_ = DeepLinkAction::RefreshUpdates;
    runUpdateCheck();
    return;
  }
  if (requested != DeepLinkAction::None && requestedNovel[0] != '\0' && JjwxcStore::isDisclaimerAccepted()) {
    deepLinkAction_ = requested;
    // The detail page is a destination, not a maintenance job: it stays open
    // until Back, so it must not auto-finish after openBook().
    deepLinkReturnAction_ = requested == DeepLinkAction::OpenBookDetail ? DeepLinkAction::None : requested;
    deepLinkChapterId_ = requestedChapterId;
    deepLinkOpenChapter_ = requestedOpenChapter;
    deepLinkOpenBookDetail_ = requested == DeepLinkAction::OpenBookDetail;
    openBook(requestedNovel);
  }

  // Find-page shortcuts (书架 / 发现 / 搜索 / 账号) land on the app's own
  // surface. A shortcut never overrides a reader deep link, and it is dropped
  // while the disclaimer is still pending so the notice cannot be skipped.
  const MenuShortcut shortcut = pendingMenuShortcut_;
  pendingMenuShortcut_ = MenuShortcut::None;
  if (requested == DeepLinkAction::None && shortcut != MenuShortcut::None &&
      JjwxcStore::isDisclaimerAccepted()) {
    switch (shortcut) {
      case MenuShortcut::Shelf:
        shelfClassId_.clear();
        shelfTitle_.clear();
        loadShelf();
        break;
      case MenuShortcut::Discover:
        enter(State::Discover);
        break;
      case MenuShortcut::Search:
        enter(State::AdvancedSearch);
        break;
      case MenuShortcut::Account:
        enter(State::Account);
        break;
      default:
        break;
    }
  }
}

void JjwxcActivity::onExit() {
  // Rule #9: the worker task must be gone before the activity is destroyed, and
  // the popup must not outlive the screen that owns it.
  optionPopup_.dismiss();
  stopJob();
  // Rule #5, and the bitmap points at this file: both go before the activity does.
  releaseCover();
  transport_.resetSession();
  // Drop TLS before the radio, and only tear Wi-Fi down if this activity brought
  // it up -- a connection the user already had is not ours to close.
  if (wifiSessionActive_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    esp_wifi_deinit();
  }
  wifiSessionActive_ = false;
  if (scratch_) {
    free(scratch_);
    scratch_ = nullptr;
    scratchCap_ = 0;
  }
  releaseWapSession();
  // The PSRAM-backed catalogues need no explicit release here: exitActivity()
  // destroys this activity immediately after onExit() returns
  // (ActivityManager.cpp:442-450), so their destructors give the memory back.
  Activity::onExit();
}

// A long download can outlast the auto-sleep window: main.cpp keeps accumulating
// inactivity while preventAutoSleep() holds the timer off (main.cpp:1223), so
// without a grace window the device would sleep the instant the result appeared.
// The window is bounded and loop() drops it on the first real input, so a device
// nobody comes back to still sleeps.
bool JjwxcActivity::preventAutoSleep() {
  if (jobRunning_) return true;
  return sleepHoldoffUntil_ != 0 && static_cast<int32_t>(millis() - sleepHoldoffUntil_) < 0;
}

void JjwxcActivity::enter(State next) {
  state_ = next;
  selected_ = 0;
  scroll_ = 0;
  // Every page opens collapsed and unscrolled: a book the user comes back to later should
  // look like the one they left, not like a paragraph they had expanded once.
  blurbExpanded_ = false;
  detailScroll_ = 0;
  infoLineCount_ = -1;
  detailLayoutValid_ = false;
  // The cover belongs to one BookDetail screen: it is opened on the way in and let go
  // on the way out, so no SD handle is held across a download or a reader hand-off.
  releaseCover();
  optionPopup_.dismiss();
  optionPopupClosing_ = false;
  // A download finishes by entering Library directly, so the on-disk list has to
  // be re-read here rather than only in loadLibrary().
  if (next == State::Library) loadLibrary();
  if (next == State::BookDetail && novel_.novelId[0] != '\0') {
    // Re-read what the card holds: a cache job that just finished changed the
    // counts and, on a first download, the title the artefact's path is built
    // from. One 380-byte record -- deliberately not readTocSummary(), whose walk
    // of a 4000-chapter index has no business running inside an input handler.
    char keepId[16];
    snprintf(keepId, sizeof(keepId), "%s", novel_.novelId);
    if (JjwxcStore::loadNovel(keepId, novel_)) {
      snprintf(novel_.novelId, sizeof(novel_.novelId), "%s", keepId);
    }
  }
  // Opened here rather than per frame: a Bitmap parses its headers once and keeps its
  // row scratch for every draw after that, which is what keeps render() free of
  // allocations.
  if (next == State::BookDetail) loadCoverThumb();
  // Refresh only applies to the range screen the confirmation led to.
  if (next != State::DownloadRange) refreshPending_ = false;
  armInputBarrier();
  rebuildRows();
  // The detail page's first row can be the expand/collapse marker, which is an affordance
  // rather than the thing the user came here to do: land on the first real action instead,
  // so a stray Confirm caches chapters rather than unfolding the blurb.
  if (next == State::BookDetail && rows_.size() > 1 && rowIds_[0] == "intro") selected_ = 1;
  requestUpdate();
}

void JjwxcActivity::armInputBarrier() {
  // The gesture that caused a transition is still in this frame's shared input
  // snapshot, so the frame is dropped whole; then wait out any physical button
  // that is genuinely still held. One gesture must produce one action.
  entryFrame_ = true;
  // Deliberately NOT arming a full GC16 here: a state change repaints the whole
  // frame, and the differential GL16 push drives every changed pixel cleanly.
  // Forcing FULL on all ~38 transitions made the app flash black/white on every
  // step (shelf -> detail -> chapters -> range ...), far past the panel's
  // 1-in-N sweep cadence that every other activity relies on. The genuinely
  // full-frame rewrites that benefit from a clean sweep (entering the download
  // screen, a chip flipping filled<->outlined, a blurb expanding a screenful of
  // prose) still arm fullRefreshPending_ at their own call sites.
  waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
}

void JjwxcActivity::showToast(const char* text, const uint32_t durationMs) {
  toast_ = text != nullptr ? text : "";
  toastUntilMs_ = millis() + durationMs;
  requestUpdate();
}

void JjwxcActivity::showMessage(std::string text, State returnTo, std::string heading) {
  message_ = std::move(text);
  messageHeading_ = std::move(heading);
  returnState_ = returnTo;
  state_ = State::Message;
  infoLineCount_ = -1;
  detailLayoutValid_ = false;
  busyScreen_ = false;
  armInputBarrier();
  requestUpdate();
}

// Maps a protocol status onto a translated, actionable message. The library's
// statusText() returns hardcoded Chinese and stays a logging helper only: every
// user-visible string has to come from tr() (AGENTS.md rule #3).
StrId JjwxcActivity::statusMessageId(jjwxc::Status s) {
  switch (s) {
    case jjwxc::Status::Ok:
      return StrId::STR_JJWXC_LOADING;  // never shown; keeps the switch total
    case jjwxc::Status::NeedLogin:
      return StrId::STR_JJWXC_NEED_LOGIN;
    case jjwxc::Status::NeedDeviceCode:
      return StrId::STR_JJWXC_ERR_DEVICE_CODE;
    case jjwxc::Status::BadCredentials:
      return StrId::STR_JJWXC_ERR_BAD_CREDENTIALS;
    case jjwxc::Status::BadUserAgent:
    case jjwxc::Status::BadVersion:
    case jjwxc::Status::WrongCredentialSource:
    case jjwxc::Status::SignError:
      return StrId::STR_JJWXC_ERR_CLIENT_REJECTED;
    case jjwxc::Status::EmptyResult:
      return StrId::STR_JJWXC_ERR_EMPTY;
    case jjwxc::Status::PurchaseRequired:
      return StrId::STR_JJWXC_ERR_PURCHASE;
    case jjwxc::Status::Server:
      return StrId::STR_JJWXC_ERR_SERVER;
    case jjwxc::Status::Network:
    case jjwxc::Status::NoNetwork:
      return StrId::STR_JJWXC_ERR_NETWORK;
    case jjwxc::Status::Protocol:
      return StrId::STR_JJWXC_ERR_PROTOCOL;
    case jjwxc::Status::Decrypt:
      return StrId::STR_JJWXC_ERR_DECRYPT;
    case jjwxc::Status::TooLarge:
      return StrId::STR_JJWXC_ERR_TOO_LARGE;
    case jjwxc::Status::Cancelled:
      return StrId::STR_JJWXC_CANCELLED;
  }
  return StrId::STR_JJWXC_ERR_UNKNOWN;
}

void JjwxcActivity::fail(jjwxc::Status s) {
  LOG_ERR(kTag, "%s -> %s (code=%ld)", jobWhat_.c_str(), jjwxc::statusText(s), jobDetailCode_);
  if (jobServerMessage_[0] != '\0') LOG_ERR(kTag, "server said: %s", jobServerMessage_);
  StrId id = statusMessageId(s);
  switch (jobFailKind_) {
    case FailKind::StorageWrite:
      // "Check the free space and the SD card" is the only actionable advice for
      // a write failure; a generic server error would send the user looking in
      // the wrong place.
      id = StrId::STR_JJWXC_ERR_STORAGE;
      break;
    case FailKind::OutOfMemory:
      id = StrId::STR_JJWXC_ERR_MEMORY;
      break;
    case FailKind::LoginReturned:
      id = StrId::STR_JJWXC_ERR_LOGIN_RETURNED;
      break;
    case FailKind::Protocol:
      if (s == jjwxc::Status::NoNetwork) id = StrId::STR_JJWXC_NO_WIFI;
      break;
  }
  // The failed operation becomes the heading, so no translated fragment is ever
  // concatenated into a sentence.
  std::string text = I18N.get(id);
  // "Server error" is the arm statusFromCode() falls into for every code it does not
  // know, so on its own it says nothing -- and it has sent four debugging sessions
  // looking for a broken server instead of an unrecognised answer. Where the service
  // did send a sentence, that sentence replaces the generic one; the numeric code is
  // still appended below, so an empty message is still diagnosable from the screen.
  if ((id == StrId::STR_JJWXC_ERR_SERVER || id == StrId::STR_JJWXC_ERR_DEVICE_CODE) && jobServerMessage_[0] != '\0') {
    text = jobServerMessage_;
  }
  // Append the service's own numeric code when it gave one. It is a number, not a
  // word, so this splices no untranslated text into the message -- and for any code
  // statusFromCode() does not know it is the only part of the screen that says
  // what actually happened.
  if (jobDetailCode_ != 0) {
    char code[24];
    snprintf(code, sizeof(code), "%ld", jobDetailCode_);
    text += " (";
    text += code;
    text += ")";
  }
  showMessage(std::move(text), returnState_, jobWhat_);
}

const char* JjwxcActivity::stateLabel(const uint8_t rawState) {
  switch (static_cast<JjwxcStore::ChapterState>(rawState)) {
    case JjwxcStore::ChapterState::Cached:
      return tr(STR_JJWXC_STATE_CACHED);
    case JjwxcStore::ChapterState::Unpurchased:
      return tr(STR_JJWXC_CHAPTER_UNPURCHASED);
    case JjwxcStore::ChapterState::Locked:
      return tr(STR_JJWXC_STATE_LOCKED);
    case JjwxcStore::ChapterState::Missing:
      break;
  }
  return tr(STR_JJWXC_STATE_MISSING);
}

// ---------------------------------------------------------------------------
// Row model
// ---------------------------------------------------------------------------

void JjwxcActivity::rebuildRows() {
  rows_.clear();
  rowIds_.clear();
  // The detail block reserves room for the action rows, so a change in how many
  // there are changes the block's height. Invalidating here rather than at each call
  // site is what keeps that true for rows added later.
  detailLayoutValid_ = false;
  char sub[128];
  switch (state_) {
    case State::Disclaimer:
      break;
    case State::Menu:
      rows_.push_back({tr(STR_JJWXC_SHELF), haveSession() ? tr(STR_JJWXC_SHELF_HINT) : tr(STR_JJWXC_NEED_LOGIN)});
      rowIds_.push_back("shelf");
      // Discovery and both search modes are anonymous, so none of them is gated
      // on the session. The subtitle says waht the row does, not what it needs.
      rows_.push_back({tr(STR_JJWXC_DISCOVER), tr(STR_JJWXC_DISCOVER_HINT)});
      rowIds_.push_back("discover");
      // One entry: the page itself switches between title, author and advanced.
      rows_.push_back({tr(STR_JJWXC_SEARCH), tr(STR_JJWXC_SEARCH_HINT)});
      rowIds_.push_back("search");
      // Favourites are the one browsing surface that needs the session; the
      // other lists above are anonymous.
      if (haveSession()) {
        rows_.push_back({tr(STR_JJWXC_FAVORITES), tr(STR_JJWXC_FAVORITES_HINT)});
        rowIds_.push_back("favorites");
      }
      rows_.push_back({tr(STR_JJWXC_LIBRARY), tr(STR_JJWXC_LIBRARY_HINT)});
      rowIds_.push_back("library");
      // WAP hands back no nickName in a JSON field; there is a `nickname` cookie, but
      // it is percent-encoded UTF-8 and decoding it is not worth a branch here. The
      // generic label is honest about what we know.
      rows_.push_back({tr(STR_JJWXC_ACCOUNT),
                       haveSession() ? (cred_.nickName[0] ? cred_.nickName : tr(STR_JJWXC_LOGGED_IN))
                                     : tr(STR_JJWXC_NOT_LOGGED_IN)});
      rowIds_.push_back("account");
      break;
    case State::Account: {
      // Two distinct entry points rather than one "Login" that silently reuses the
      // saved name: the user has to be able to tell whether confirming will log in
      // immediately, ask for a password, or ask for an account too. The subtitle
      // answers that before the key is pressed.
      if (savedAccount_.valid()) {
        snprintf(sub, sizeof(sub), "%s · %s", savedAccount_.loginName.c_str(),
                 savedAccount_.hasPassword() ? tr(STR_JJWXC_SAVED_PASSWORD) : tr(STR_JJWXC_NEEDS_PASSWORD));
        rows_.push_back({tr(STR_JJWXC_LOGIN_LAST), sub});
        rowIds_.push_back("last");
      }
      const char* loginLabel = tr(STR_JJWXC_LOGIN);
      if (savedAccount_.valid()) {
        loginLabel = tr(STR_JJWXC_LOGIN_MANUAL);
      } else if (haveSession()) {
        loginLabel = tr(STR_JJWXC_RELOGIN);
      }
      rows_.push_back({loginLabel, tr(STR_JJWXC_LOGIN_HINT)});
      rowIds_.push_back("login");
      if (haveSession()) {
        // Already signed in: approval is for the next login, not this one.
      } else if (hasLoginPassword()) {
        rows_.push_back({tr(STR_JJWXC_AUTH_APPROVE), tr(STR_JJWXC_AUTH_APPROVE_HINT)});
        rowIds_.push_back("authapprove");
      }
      if (haveSession()) {
        rows_.push_back({tr(STR_JJWXC_LOGOUT), tr(STR_JJWXC_LOGOUT_HINT)});
        rowIds_.push_back("logout");
      }
      // Separate from logout: clearing the shortcut is the only way to get a
      // stored password off the card, and logging out must not do it by surprise.
      if (savedAccount_.valid()) {
        rows_.push_back({tr(STR_JJWXC_CLEAR_ACCOUNT), tr(STR_JJWXC_CLEAR_ACCOUNT_HINT)});
        rowIds_.push_back("clearaccount");
      }
      if (jjwxcpay::available()) {
        rows_.push_back({tr(STR_JJWXC_CLEAR_PAY_PASSWORD), tr(STR_JJWXC_CLEAR_PAY_PASSWORD_HINT)});
        rowIds_.push_back("clearpay");
      }
      break;
    }
    case State::Shelf:
      for (const auto& b : shelf_) {
        // The WAP shelf page carries no chapter count -- only the title, the author,
        // the update marker and two timestamps -- so it arrives as 0. Printing "0章"
        // would state a fact the page never gave us; the count is filled in later by
        // the anonymous app-cdn chapterList when the book is opened.
        // TODO(§9.11): this line and its " · 有更新" fragment are hardcoded Chinese and
        // should go through tr(); it needs a format-string key, not a concatenation.
        if (b.chapterCount > 0) {
          snprintf(sub, sizeof(sub), "%s · %lu章%s", b.authorName, static_cast<unsigned long>(b.chapterCount),
                   b.renewStatus ? " · 有更新" : "");
        } else {
          snprintf(sub, sizeof(sub), "%s%s", b.authorName, b.renewStatus ? " · 有更新" : "");
        }
        rows_.push_back({b.novelName, sub});
        rowIds_.push_back(b.novelId);
      }
      break;
    case State::Discover:
      for (const auto& source : kDiscoverSources) {
        rows_.push_back({trId(source.label), ""});
        rowIds_.push_back(source.path);
      }
      break;
    case State::DiscoverList:
      for (const auto& b : discover_) {
        rows_.push_back({b.novelName, b.authorName});
        rowIds_.push_back(b.novelId);
      }
      if (!discoverNextUrl_.empty() && rows_.size() < kMaxRows) {
        rows_.push_back({tr(STR_JJWXC_NEXT_PAGE), ""});
        rowIds_.push_back("__next");
      }
      break;
    case State::AdvancedSearch: {
      // The mode is chosen with the three chips drawn above this list
      // (renderSearchChips()). It is deliberately NOT a row any more: the old
      // "search mode" row repeated the same three labels a second time, two
      // controls for one setting, and the chips are the one a touch user aims at.
      const char* any = tr(STR_JJWXC_ADV_ANY);
      rows_.push_back({tr(STR_JJWXC_ADV_KEYWORD), advKeyword_.empty() ? any : advKeyword_.c_str()});
      rowIds_.push_back("advkeyword");
      if (searchMode_ == 2) {
        rows_.push_back({tr(STR_JJWXC_ADV_INCLUDE), advInclude_.empty() ? any : advInclude_.c_str()});
        rowIds_.push_back("advinclude");
        rows_.push_back({tr(STR_JJWXC_ADV_EXCLUDE), advExclude_.empty() ? any : advExclude_.c_str()});
        rowIds_.push_back("advexclude");
        const char* finish = advFinish_ == 1 ? tr(STR_JJWXC_ADV_SERIAL)
                             : advFinish_ == 2 ? tr(STR_JJWXC_ADV_FINISHED)
                                               : any;
        rows_.push_back({tr(STR_JJWXC_ADV_STATUS), finish});
        rowIds_.push_back("advfinish");
        rows_.push_back({tr(STR_JJWXC_ADV_SORT), trId(kAdvSorts[advSort_].label)});
        rowIds_.push_back("advsort");
      }
      // Always last: renderSearchRows() paints this row's band as the search
      // button, and onListConfirm() is what both the tap and the keyboard reach.
      rows_.push_back({tr(STR_JJWXC_ADV_RUN), ""});
      rowIds_.push_back("advrun");
      break;
    }
    case State::Favorites: {
      rows_.push_back({tr(STR_JJWXC_FAV_ALL), ""});
      rowIds_.push_back("__all");
      for (const auto& c : favClasses_) {
        // The site's negative ids are pseudo views (weekly activity, recent
        // reading, temp shelf, local import), not folders a book can be filed in.
        if (c.id[0] == '-') continue;
        rows_.push_back({c.name, ""});
        rowIds_.push_back(c.id);
      }
      rows_.push_back({tr(STR_JJWXC_FAV_CREATE), ""});
      rowIds_.push_back("__create");
      for (const auto& c : favClasses_) {
        if (c.id[0] != '-' && strcmp(c.id, "0") != 0) {
          rows_.push_back({tr(STR_JJWXC_FAV_MANAGE), ""});
          rowIds_.push_back("__manage");
          break;
        }
      }
      break;
    }
    case State::FavoritePicker:
      for (const auto& c : favClasses_) {
        if (c.id[0] == '-') continue;
        rows_.push_back({c.name, ""});
        rowIds_.push_back(c.id);
      }
      break;
    case State::FavoriteManage:
      for (const auto& c : favClasses_) {
        if (c.id[0] == '-' || strcmp(c.id, "0") == 0) continue;
        rows_.push_back({c.name, ""});
        rowIds_.push_back(c.id);
      }
      break;
    case State::ParagraphComments:
      for (const auto& p : paragraphs_) {
        char label[64];
        snprintf(label, sizeof(label), tr(STR_JJWXC_COMMENTS_PARAGRAPH_FMT),
                 static_cast<unsigned long>(strtoul(p.paragraphId, nullptr, 10)),
                 static_cast<unsigned long>(p.total));
        rows_.push_back({label, ""});
        rowIds_.push_back(p.paragraphId);
      }
      break;
    case State::CommentList:
      for (const auto& c : comments_) {
        rows_.push_back({c.body, c.author});
        rowIds_.push_back("comment");
      }
      if (comments_.size() < commentTotal_) {
        rows_.push_back({tr(STR_JJWXC_COMMENTS_MORE), ""});
        rowIds_.push_back("__more");
      }
      break;
    case State::SearchResults:
      for (const auto& s : search_) {
        // The WAP search page carries only the title, the author and two links, so
        // sizeText and introShort are empty for every row it produces (see
        // onSearchEntryAdapter). Printing the separator anyway left every result
        // reading "岐山娘 · " -- a dangling "·" that claims a second fact the page
        // never gave, which is the same mistake the shelf row's "0章" was.
        if (s.sizeText[0] != '\0') {
          snprintf(sub, sizeof(sub), "%s · %s", s.authorName, s.sizeText);
        } else if (s.introShort[0] != '\0') {
          snprintf(sub, sizeof(sub), "%s · %s", s.authorName, s.introShort);
        } else {
          snprintf(sub, sizeof(sub), "%s", s.authorName);
        }
        rows_.push_back({s.novelName, sub});
        rowIds_.push_back(s.novelId);
      }
      break;
    case State::BookDetail: {
      // The book's facts live in the info block above these rows (renderInfoBlock),
      // so the rows themselves are pure actions and carry no subtitle.
      rows_.push_back({tr(STR_JJWXC_CACHE_CHAPTERS), ""});
      rowIds_.push_back("cache");
      rows_.push_back({tr(STR_JJWXC_CHAPTERS), ""});
      rowIds_.push_back("chapters");
      if (haveSession()) {
        // The site's add endpoint rejects a book that is already favourited (1059),
        // so the label and the action depend on the state fetched with the book.
        // The destructive row appears ONLY when we know the book is favourited;
        // before this the cancel button was always shown and always "succeeded".
        const bool knownFav = favoriteKnown_ && isFavorite_;
        rows_.push_back({knownFav ? tr(STR_JJWXC_FAV_CHANGE) : tr(STR_JJWXC_FAV_ADD),
                         knownFav && !favoriteClassName_.empty() ? favoriteClassName_.c_str() : ""});
        rowIds_.push_back("favadd");
        if (knownFav) {
          rows_.push_back({tr(STR_JJWXC_FAV_REMOVE), ""});
          rowIds_.push_back("favdel");
        }
      }
      // Downloaded-book maintenance. These used to be unreachable: the
      // "Downloaded" list opened the reader directly, so there was no way to see
      // or refresh a book's chapters without going through search again.
      rows_.push_back({tr(STR_JJWXC_UPDATE_LIST), ""});
      rowIds_.push_back("synclist");
      rows_.push_back({tr(STR_JJWXC_UPDATE_CURRENT), ""});
      rowIds_.push_back("updatecurrent");
      if (haveSession() && novel_.cachedChapterCount > 0) {
        // Explicit because a long novel's bodies are thousands of requests. The
        // pass is resumable, so the row stays useful across several runs.
        rows_.push_back({tr(STR_JJWXC_COMMENT_PREFETCH), ""});
        rowIds_.push_back("commentbodies");
      }
      if (novel_.packagedChapterCount > 0 || Storage.exists(JjwxcStore::finalBookPath(novel_).c_str())) {
        rows_.push_back({tr(STR_JJWXC_DELETE_CACHE), ""});
        rowIds_.push_back("deletecache");
      }
      if (cloudHasProgress_ && cloudOrdinalValid_) {
        // The cloud position is offered only when this card can actually reach
        // that chapter; otherwise the row would promise a jump it cannot make.
        char cloud[96];
        snprintf(cloud, sizeof(cloud), tr(STR_JJWXC_CLOUD_PROGRESS_FMT),
                 static_cast<unsigned long>(cloudOrdinal_ + 1));
        rows_.push_back({cloud, ""});
        rowIds_.push_back("cloudjump");
      }
      // Gated on a chapter that can actually be read, not on the artefact merely
      // existing: a book whose every entry is a purchase stub used to offer itself
      // here and then page through nothing but refusals.
      if (novel_.cachedChapterCount > 0 && Storage.exists(JjwxcStore::finalBookPath(novel_).c_str())) {
        rows_.push_back({tr(STR_JJWXC_OPEN_LOCAL), ""});
        rowIds_.push_back("open");
      }
      // Refreshing is an explicit action, never a side effect of the artefact
      // already existing: that is the half of WeRead's `refreshingBook` logic this
      // app deliberately does not have.
      if (novel_.packagedChapterCount > 0) {
        rows_.push_back({tr(STR_JJWXC_REFRESH_CACHE), ""});
        rowIds_.push_back("refresh");
      }
      // The expand/collapse row, decided from the COLLAPSED layout and never from the
      // current one: an expanded page shows everything by definition, so asking "is
      // anything hidden right now" would remove the very row the user needs in order to
      // collapse it again. The reserve saturates at kDetailRowsReserve rows and this
      // state always has at least that many action rows, so the answer is the same with
      // and without this row -- it cannot decide its own existence into a loop.
      const bool savedExpanded = blurbExpanded_;
      blurbExpanded_ = false;
      detailLayoutValid_ = false;
      const DetailLayout& collapsed = detailLayout();
      const bool hidden =
          collapsed.factCut || collapsed.blurbCut || (blurbLen() > 0 && collapsed.blurbLines == 0);
      blurbExpanded_ = savedExpanded;
      detailLayoutValid_ = false;
      if (hidden) {
        // First, not last: the collapsed page guarantees only the first
        // kDetailRowsReserve rows, so this is the one row that is always on screen, and
        // an affordance the user has to scroll to find is not an affordance. The label is
        // the section it controls (the blurb's own heading, or "more details" when the
        // book has no blurb and it is the fact column that was cut); the subtitle is the
        // state, so the row reads as an action rather than as a heading.
        const char* label = blurbLen() > 0 ? tr(STR_JJWXC_DETAIL_INTRO) : tr(STR_MORE_DETAILS);
        rows_.insert(rows_.begin(), Row{label, savedExpanded ? tr(STR_HIDE) : tr(STR_SHOW)});
        rowIds_.insert(rowIds_.begin(), jjwxc::PsramString("intro"));
      }
      break;
    }
    case State::DownloadRange: {
      // Presets instead of the old "walk the endpoints one key press at a time"
      // picker: the common cases are one confirm, and the custom case is two
      // keyboard entries rather than N direction-key presses.
      // Every run now carries a time estimate: a whole book is ~18 min for 195
      // chapters (~6 s each, VIP chapters cost a second request for the font).
      // Starting one without knowing that was the single most common way to be
      // surprised by a job that cannot be cancelled mid-request.
      char estimate[48];
      const auto estimateText = [&](const unsigned long chapters) -> const char* {
        const unsigned long minutes = (chapters * 6 + 59) / 60;
        char unit[8];
        snprintf(unit, sizeof(unit), "%lu", minutes);
        snprintf(estimate, sizeof(estimate), tr(STR_JJWXC_ESTIMATE_MIN), unit);
        return estimate;
      };
      snprintf(sub, sizeof(sub), "%lu · %s", static_cast<unsigned long>(tocSummary_.count),
               estimateText(tocSummary_.count));
      rows_.push_back({tr(STR_JJWXC_RANGE_WHOLE), sub});
      rowIds_.push_back("whole");

      snprintf(sub, sizeof(sub), "%d", firstN_);
      rows_.push_back({tr(STR_JJWXC_RANGE_FIRST_N), sub});
      rowIds_.push_back("firstn");

      // "Not cached yet" spans every chapter without a real body, refused ones
      // included. The old Missing-only range is what made a book whose tail was 72
      // purchase stubs offer nothing to do and report itself fully cached.
      if (tocSummary_.hasPending) {
        const unsigned long pending =
            static_cast<unsigned long>(tocSummary_.lastPending - tocSummary_.firstPending + 1);
        snprintf(sub, sizeof(sub), "%lu - %lu · %s", static_cast<unsigned long>(tocSummary_.firstPending + 1),
                 static_cast<unsigned long>(tocSummary_.lastPending + 1), estimateText(pending));
      } else {
        snprintf(sub, sizeof(sub), "%s", tr(STR_JJWXC_ALL_CACHED));
      }
      rows_.push_back({tr(STR_JJWXC_RANGE_UNCACHED), sub});
      rowIds_.push_back("uncached");

      if (tocSummary_.unpurchased > 0) {
        // Re-requesting a recorded refusal is the only way a chapter bought later,
        // on the phone, ever reaches the card -- so it is its own row and says how
        // many. It is also the only preset that re-sends a request the service may
        // answer with a purchase attempt (cacheChapterRange's refusal branch), which
        // is why it is not folded into "uncached".
        snprintf(sub, sizeof(sub), "%lu", static_cast<unsigned long>(tocSummary_.unpurchased));
        rows_.push_back({tr(STR_JJWXC_RETRY_UNPURCHASED), sub});
        rowIds_.push_back("retry");
      }

      if (rangeFrom_ >= 0 && rangeTo_ >= 0) {
        snprintf(sub, sizeof(sub), "%d - %d", rangeFrom_ + 1, rangeTo_ + 1);
      } else {
        sub[0] = '\0';
      }
      rows_.push_back({tr(STR_JJWXC_RANGE_CUSTOM), sub});
      rowIds_.push_back("custom");
      break;
    }
    case State::ChapterList: {
      // Windowed rather than truncated. A 1500-chapter novel used to stop at row
      // 300 with no route to the rest. A long book's layout is
      // [previous?][jump][chapters][next?], which is why three rows are reserved.
      const size_t total = chapters_.size();
      const bool longBook = total > kMaxRows;
      const int span = longBook ? static_cast<int>(kMaxRows - 3) : static_cast<int>(kMaxRows);
      chapterPageSpan_ = span;
      // Pages advance by a full span and the last one may be short. Clamping the
      // window to total - span instead would make "next page" leap backwards on
      // a book only slightly longer than kMaxRows (301 chapters would page from
      // 1-297 to 5-301).
      const int lastStart = total == 0 ? 0 : static_cast<int>(total) - 1;
      chapterWindow_ = std::clamp(chapterWindow_, 0, lastStart);
      const int first = chapterWindow_;
      const int end = longBook ? std::min(static_cast<int>(total), first + span) : static_cast<int>(total);
      if (longBook) {
        if (first > 0) {
          rows_.push_back({tr(STR_JJWXC_PREV_PAGE), ""});
          rowIds_.push_back("__prevpage");
        }
        char range[48];
        snprintf(range, sizeof(range), "%d-%d / %lu", first + 1, end, static_cast<unsigned long>(total));
        rows_.push_back({tr(STR_JJWXC_JUMP_CHAPTER), range});
        rowIds_.push_back("__jump");
      }
      chapterRowStart_ = static_cast<int>(rows_.size());
      for (int i = first; i < end && rows_.size() < kMaxRows; ++i) {
        const auto& c = chapters_[static_cast<size_t>(i)];
        const size_t chapterIndex = static_cast<size_t>(i);
        const uint8_t rawState = chapterIndex < chapterStates_.size() ? chapterStates_[chapterIndex] : 0;
        const bool haveRange = rangeFrom_ >= 0 && rangeTo_ >= 0;
        const bool inRange =
            haveRange && i >= std::min(rangeFrom_, rangeTo_) && i <= std::max(rangeFrom_, rangeTo_);
        snprintf(sub, sizeof(sub), "%s%lu · %s%s · %lu字", inRange ? "● " : "", static_cast<unsigned long>(i + 1),
                 stateLabel(rawState), c.isVip ? " · VIP" : "", static_cast<unsigned long>(c.size));
        rows_.push_back({c.name, sub});
        // Not std::to_string(): that builds a std::string, whose characters would land
        // in internal RAM -- the exact allocation this container exists to avoid.
        char idBuf[12];
        snprintf(idBuf, sizeof(idBuf), "%lu", static_cast<unsigned long>(c.chapterId));
        rowIds_.push_back(idBuf);
      }
      if (longBook && end < static_cast<int>(total)) {
        rows_.push_back({tr(STR_JJWXC_NEXT_PAGE), ""});
        rowIds_.push_back("__nextpage");
      }
      break;
    }
    case State::Library:
      for (const auto& b : books_) {
        // readable/remote. A record written before the split has the two equal (see
        // loadNovel()'s compatibility note), so the extra clause simply does not fire.
        const uint32_t blocked = b.packagedChapterCount > b.cachedChapterCount
                                     ? b.packagedChapterCount - b.cachedChapterCount
                                     : 0;
        if (b.remoteChapterCount > 0) {
          snprintf(sub, sizeof(sub), "%s · %lu/%lu", b.author, static_cast<unsigned long>(b.cachedChapterCount),
                   static_cast<unsigned long>(b.remoteChapterCount));
        } else {
          // A zero denominator reads as a bug, so show only what is known.
          snprintf(sub, sizeof(sub), "%s · %lu", b.author, static_cast<unsigned long>(b.cachedChapterCount));
        }
        if (blocked > 0) {
          const size_t used = strlen(sub);
          snprintf(sub + used, sizeof(sub) - used, " · %s%lu", tr(STR_JJWXC_DETAIL_BLOCKED_SHORT),
                   static_cast<unsigned long>(blocked));
        }
        rows_.push_back({b.title, sub});
        rowIds_.push_back(b.novelId);
      }
      break;
    case State::Downloading:
    case State::Message:
      break;
  }
  if (selected_ >= static_cast<int>(rows_.size())) selected_ = rows_.empty() ? 0 : static_cast<int>(rows_.size()) - 1;
  // The rows just changed, so the page's own scroll limit changed with them. Clamping
  // here rather than at each call site is what keeps a stale offset from leaving the user
  // looking at blank panel after a rebuild; it is free outside BookDetail.
  clampDetailScroll();
}

int JjwxcActivity::chapterIndexForRow(const int row) const {
  if (state_ != State::ChapterList || row < chapterRowStart_) return -1;
  const int index = chapterWindow_ + (row - chapterRowStart_);
  return index >= 0 && static_cast<size_t>(index) < chapters_.size() ? index : -1;
}

void JjwxcActivity::moveSelection(int delta) {
  if (rows_.empty()) return;
  selected_ = (selected_ + delta + static_cast<int>(rows_.size())) % static_cast<int>(rows_.size());
  ensureVisible();
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Network / SD jobs
// ---------------------------------------------------------------------------

// Brings Wi-Fi up when needed, then runs `job` -- inline if already connected,
// otherwise from the Wi-Fi activity's result callback. Mirrors WeRead's
// connectThen(): the autoConnect flag is what makes WifiSelectionActivity reuse
// the credentials already saved in WifiCredentialStore instead of presenting a
// network picker, so a configured device connects without user interaction.
void JjwxcActivity::withNetwork(std::function<void()> job, const State onCancel, const bool returnDeepLinkOnCancel) {
  const auto abort = [this, onCancel, returnDeepLinkOnCancel] {
    (void)onCancel;
    if (returnDeepLinkOnCancel && deepLinkReturnAction_ != DeepLinkAction::None) {
      const DeepLinkAction action = deepLinkReturnAction_;
      deepLinkReturnAction_ = DeepLinkAction::None;
      finishDeepLink(action, /*completed=*/false, /*readable=*/false);
      return;
    }
    // Cancelling the Wi-Fi sheet is a deliberate user action: stay on the page
    // they were on and say why nothing happened. A full-screen message page
    // made them dismiss a second screen just to get back.
    showToast(tr(STR_JJWXC_NO_WIFI));
  };
  if (transport_.networkReady()) {
    job();
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
  if (!wifi) {
    LOG_ERR(kTag, "OOM: Wi-Fi activity");
    abort();
    return;
  }
  wifiSessionActive_ = true;
  startActivityForResult(std::move(wifi), [this, job, abort](const ActivityResult& result) {
    // The child can close on a key edge; arm the release barrier so that same
    // edge cannot also drive this activity.
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (!result.isCancelled && transport_.networkReady()) {
      job();
      return;
    }
    // Back to where the user was, not to the main menu: they backed out of the
    // Wi-Fi page, they did not ask to leave the book.
    abort();
  });
}

bool JjwxcActivity::startJob(std::function<void()> body, const char* busyText, const bool openAfter) {
  if (jobRunning_) return false;
  jobBody_ = std::move(body);
  jobDone_.store(false);
  jobStatus_ = jjwxc::Status::Ok;
  // Without the PSRAM scratch buffer an envelope-shaped response cannot be
  // decoded at all, so every failure of that job is reported as a memory problem
  // rather than as a server one.
  jobFailKind_ = scratch_ != nullptr ? FailKind::Protocol : FailKind::OutOfMemory;
  jobReplacedTarget_ = false;
  jobPackageFailed_ = false;
  jobRangeUnreadable_ = false;
  jobPurchased_ = false;
  jobDetailCode_ = 0;
  jobServerMessage_[0] = '\0';
  jobAccountPersisted_.store(false);
  openAfterJob_ = openAfter;
  jobRunning_ = true;
  busyText_ = busyText ? busyText : "";
  busyStartedMs_ = millis();
  lastBusyTickMs_ = busyStartedMs_;
  busyCancelRect_ = Rect{};
  // Drop the TLS session BEFORE creating the task: it holds internal-RAM buffers that the
  // handshake inside the job is about to need, and the job then reuses its own session for
  // every request it makes. Cost is one extra handshake (~0.7 s measured) per job.
  transport_.resetSession();

  // The stack goes in PSRAM, and that is the actual fix for a download that reports a
  // network error. Measured on the device, one job task creation followed by one request:
  //
  //   [197645] wolfSSL TLS closed: free=32675 largest=18420        <- UI task, pre-job
  //   [199783] wolfSSL TLS new: host=m.jjwxc.net free=18871 largest=8692
  //   [200653] MEM Free: 8156 MaxAlloc: 5620                       <- handshake took 10,715
  //            ... 40 s of heartbeats, Free pinned at 8156, nothing moving ...
  //   [SecureClient] handshake timeout (auto): last err 2, transport up, free heap 8156
  //
  // Creating the task cost 13,804 bytes of internal RAM and the handshake then needed
  // 10,715 more. At 8,156 free / 5,620 largest wolfSSL could no longer get a buffer -- and
  // it does not fail fast on that, it stalls until the timeout. The abandoned handshake
  // then took DNS and the socket layer down with it: every later request in that session
  // died with `errno 113` and `DNS Failed ... -54`, including to app-cdn.jjwxc.net which
  // had answered minutes earlier, and with 30 KB free. That is why the failure looked like
  // a broken network, why it did not recover, and why opening another book hung too.
  //
  // Every handshake in every captured log that started above ~30 KB free completed; the one
  // that started at 18,871 did not. So the requirement is not "a bigger stack" but "do not
  // spend the internal RAM the handshake is about to need". PSRAM is the obvious place to
  // spend it instead: 4.9 MB free against ~30 KB of internal headroom.
  //
  // CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is set for readpico, and the framework keeps
  // the TCB in internal RAM itself -- which is the reason to use this API rather than
  // handing xTaskCreateStaticPinnedToCore a heap_caps_malloc'd buffer. Note that the
  // effective stack is kJobStackBytes * sizeof(StackType_t): the allocation and the kernel
  // both scale the depth by sizeof(StackType_t), so the header's "number of bytes" is off by
  // that factor. 48 KB of PSRAM is a deliberate purchase -- it also removes any chance of
  // overflowing on the deepest path here (sfnt walk + AES + in-place GBK transcode), whose
  // true peak was never measured.
  //
  // A PSRAM-stacked task must not write internal flash, since the cache is disabled during
  // a write and the stack would vanish underneath it. That holds: this app touches only the
  // SD card, and there is no NVS, Preferences or esp_flash call anywhere in jjwxc/ or
  // lib/JjwxcApi/.
  BaseType_t ok = pdFAIL;
#ifdef JJWXC_ALLOCATOR_HAS_HEAP_CAPS
  ok = xTaskCreatePinnedToCoreWithCaps(jobTrampoline, "jjwxcJob", kJobStackPsramDepth, this, 1, &jobTask_, 0,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  jobStackInPsram_ = ok == pdPASS;
  if (ok != pdPASS) LOG_ERR(kTag, "PSRAM job stack refused (%d); falling back to internal RAM", (int)ok);
#else
  // Never silent. The last time this branch was reached by accident the cause was a guard
  // macro that Memory.h #undef'd, and the only evidence was ten task creations each
  // reporting "stack in INTERNAL RAM" with no explanation anywhere.
  LOG_ERR(kTag, "no PSRAM stack in this build; job stack must come from internal RAM");
#endif
  // Pinned to core 0 so the UI core keeps yielding and feeding the watchdog.
  if (ok != pdPASS) {
    jobStackInPsram_ = false;
    ok = xTaskCreatePinnedToCore(jobTrampoline, "jjwxcJob", kJobStackBytes, this, 1, &jobTask_, 0);
  }
  if (ok == pdPASS) {
    // Printed so the next log settles where the stack landed and what it cost. Before this
    // change the same point in the flow read free=18871; a stack in PSRAM should leave it
    // near the 32675 the UI task had a moment earlier, which is the range every successful
    // handshake in every captured log started from.
    LOG_INF(kTag, "job task up: stack in %s, free=%u largest=%u", jobStackInPsram_ ? "PSRAM" : "INTERNAL RAM",
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  }
  if (ok != pdPASS) {
    jobTask_ = nullptr;
    jobRunning_ = false;
    jobStackInPsram_ = false;
    // Both numbers, because only the pair is diagnostic. This once failed on a device with
    // 25,815 bytes free -- plenty on paper -- and a largest contiguous block of 12,276
    // against a 12,288 requirement: short by twelve bytes, killed by fragmentation rather
    // than exhaustion. Logging only the stack size says "out of memory" and sends the next
    // reader hunting for a leak that is not there.
    LOG_ERR(kTag, "job task creation failed (PSRAM then internal): need %u/%u, free=%u largest=%u",
            (unsigned)(kJobStackPsramDepth * sizeof(StackType_t)), (unsigned)kJobStackBytes,
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return false;
  }
  // The busy page replaces whatever was on screen, so the job owns the display
  // from here on; loop() gates every other input while it is up.
  busyScreen_ = true;
  requestUpdate();
  return true;
}

bool JjwxcActivity::launchJob(std::function<void()> body, const char* busyText, const bool openAfter) {
  if (startJob(std::move(body), busyText, openAfter)) return true;
  // startJob() failed, so jobRunning_ is false and pollJob() will never run:
  // leaving busyScreen_ up here would strand the user on a page with no way out.
  // A task-creation failure means the 12 KB internal-RAM stack was not
  // available, which is a memory condition, not a server one.
  busyScreen_ = false;
  LOG_ERR(kTag, "job launch failed for %s", jobWhat_.c_str());
  showMessage(tr(STR_JJWXC_JOB_START_FAILED), returnState_);
  return false;
}

void JjwxcActivity::runJobBody() {
  if (jobBody_) jobBody_();
}

void JjwxcActivity::signalJobDone() { jobDone_.store(true); }

void JjwxcActivity::reapJob() {
  if (jobTask_) {
    // Must match how the task was created. A task made with xTaskCreatePinnedToCoreWithCaps
    // owns a heap_caps-allocated stack that plain vTaskDelete() will not free -- it treats
    // the stack as caller-owned -- so every job would leak 48 KB of PSRAM. Deleting from
    // here (the UI task) rather than from the job itself is also what the API asks for.
    if (jobStackInPsram_) {
      vTaskDeleteWithCaps(jobTask_);
    } else {
      vTaskDelete(jobTask_);
    }
    jobTask_ = nullptr;
    jobStackInPsram_ = false;
  }
  jobRunning_ = false;
  busyScreen_ = false;
  inlineJob_ = false;
  jobDone_.store(false);
  // Whatever the job cost, give the user a bounded window to see the result
  // before the inactivity timer can sleep the device.
  sleepHoldoffUntil_ = millis() + kSleepHoldoffMs;
}

void JjwxcActivity::stopJob() {
  if (!jobRunning_) return;
  // The job holds no locks, so waiting for it to notice jobCancel_ is enough.
  jobCancel_.store(true);
  const uint32_t deadline = millis() + kJobStopTimeoutMs;
  while (!jobDone_.load() && millis() < deadline) {
    delay(20);
  }
  if (!jobDone_.load()) {
    // A blocking TLS read can outlast the deadline. reapJob() deletes the task
    // either way, so it can no longer touch this activity or scratch_ once
    // onExit() frees them. Whatever it held inside the HTTP client dies with it,
    // which is why onExit() also drops the session.
    LOG_ERR(kTag, "job did not stop within %ums; deleting task", (unsigned)kJobStopTimeoutMs);
  }
  jobCancel_.store(false);
  reapJob();
}

void JjwxcActivity::pollJob() {
  if (!jobRunning_ || !jobDone_.load()) return;
  reapJob();
  if (updateCheckOnly_) {
    updateCheckOnly_ = false;
    const bool wasCancelled = jobCancel_.exchange(false);
    const bool ok = !wasCancelled && jobStatus_ == jjwxc::Status::Ok;
    LOG_INF(kTag, "update check %s: %u books on the shelf", ok ? "ok" : jjwxc::statusText(jobStatus_),
            (unsigned)shelf_.size());
    if (!ok && jobDetailCode_ != 0) LOG_ERR(kTag, "update check detail code %ld", jobDetailCode_);
    finishDeepLink(DeepLinkAction::RefreshUpdates, ok);
    return;
  }
  // Published by the login job; ordered after jobDone_, which is what makes the
  // flag safe to read here.
  applyPersistedAccount();
  const bool cancelled = jobCancel_.exchange(false);
  const State after = jobNextState_;
  const bool openAfter = openAfterJob_;
  openAfterJob_ = false;

  const bool wasCache = jobWasCache_;
  jobWasCache_ = false;
  cacheMode_ = CacheMode::Incremental;

  if (cancelled) {
    // Keep what the stream already delivered instead of discarding it: a
    // cancelled shelf, search or catalogue fetch still has usable rows, and a
    // cancelled download keeps every chapter that reached the card. Landing a
    // cancelled download back on the chapter list is deliberate -- its per-chapter
    // states were updated in place, so the list itself is the report of what got
    // cached, and the busy page already said "cancelled" the moment Back was
    // pressed.
    const size_t streamed = after == State::Shelf            ? shelf_.size()
                            : after == State::SearchResults  ? search_.size()
                            : after == State::DiscoverList   ? discover_.size()
                            : after == State::Favorites      ? favClasses_.size()
                            : after == State::FavoritePicker ? favClasses_.size()
                            : after == State::FavoriteManage ? favClasses_.size()
                            : after == State::ParagraphComments ? paragraphs_.size()
                            : after == State::CommentList    ? comments_.size()
                            : after == State::ChapterList    ? chapters_.size()
                                                             : 0;
    if (streamed > 0) {
      enter(after);
      return;
    }
    showMessage(tr(STR_JJWXC_CANCELLED), returnState_);
    return;
  }
  if (needDeviceCode_) {
    needDeviceCode_ = false;
    if (deviceCodeSent_) {
      // The service asked for a code again after we sent one and the user typed it
      // in. Sending another would email them once per attempt with no way out, so
      // stop and report. Whatever 6018 really means, it is not satisfied by the
      // code we just supplied, and guessing again is not free.
      LOG_ERR(kTag, "verification code rejected or still required (code=%ld); not sending another", jobDetailCode_);
      fail(jjwxc::Status::NeedDeviceCode);
      return;
    }
    deviceCodeSent_ = true;
    const std::string checkType = pendingCheckType_.empty() ? std::string("email") : pendingCheckType_;
    // Ask the server to send the code, then collect it on the keyboard.
    jobWhat_ = tr(STR_JJWXC_DEVICE_CODE);
    jobNextState_ = State::Account;
    returnState_ = State::Account;
    // promptDeviceCode() is issued once the send job reports back.
    pendingPromptDeviceCode_ = true;
    launchJob(
        [this, checkType] {
          if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < kWapFormBytes + 4096) {
            jobStatus_ = jjwxc::Status::TooLarge;
            jobFailKind_ = FailKind::OutOfMemory;
            return;
          }
          // WAP asks with a GET carrying checktype/username in the query; the App API
          // used a POST. Same backend, different shape, and the answer is still that
          // backend's JSON envelope -- so parseDeviceCode() reads it unchanged, and
          // with it the rule that success here is code 200 rather than 0.
          BufferCollector col{reinterpret_cast<char*>(scratch_), 4096, 0, false};
          const int http = jjwxc::wapRequestDeviceCode(*wap_, accountName_.c_str(), checkType.c_str(), &collectBuffer,
                                                       &col);
          if (http < 0) {
            jobStatus_ = jjwxc::Status::Network;
            jobDetailCode_ = http;
            LOG_ERR(kTag, "getDeviceSecurityCode transport error %d", http);
            return;
          }
          if (col.overflow) {
            jobStatus_ = jjwxc::Status::TooLarge;
            LOG_ERR(kTag, "getDeviceSecurityCode answer exceeded %u bytes", (unsigned)col.cap);
            return;
          }
          jjwxc::DeviceCodeResult res;
          if (!jjwxc::parseDeviceCode(col.buf, col.len, res)) {
            jobStatus_ = jjwxc::Status::Protocol;
            jobDetailCode_ = http;
            LOG_ERR(kTag, "getDeviceSecurityCode answer did not parse (http=%d, %u bytes)", http, (unsigned)col.len);
            return;
          }
          jobStatus_ = res.status;
          // Same rule as the login path: the mapped status is lossy and the service's
          // own sentence is the only account of what it actually objected to.
          snprintf(jobServerMessage_, sizeof(jobServerMessage_), "%s", res.message);
          if (jobStatus_ != jjwxc::Status::Ok) {
            LOG_ERR(kTag, "getDeviceSecurityCode -> %s http=%d message='%s'", jjwxc::statusText(jobStatus_), http,
                    res.message);
          }
        },
        tr(STR_JJWXC_SENDING_CODE));
    return;
  }
  if (pendingPromptDeviceCode_) {
    pendingPromptDeviceCode_ = false;
    if (jobStatus_ != jjwxc::Status::Ok) {
      fail(jobStatus_);
      return;
    }
    promptDeviceCode();
    return;
  }
  // A packaging failure is not a protocol status, so it gets its own wording: the
  // chapters are on the card and only the assembly step failed, which tells the
  // user a retry is cheap.
  if (jobPackageFailed_) {
    jobPackageFailed_ = false;
    LOG_ERR(kTag, "%s -> packaging failed", jobWhat_.c_str());
    showMessage(tr(STR_JJWXC_EPUB_FAILED), returnState_, jobWhat_);
    return;
  }
  if (jobStatus_ != jjwxc::Status::Ok) {
    if (purchaseAttemptInFlight_) {
      // A stored password the server rejected must not be replayed forever; the
      // next purchase will ask again. A network failure leaves it in place.
      if (purchaseUsedSavedPassword_ && jobPurchaseWrongPassword_) {
        jjwxcpay::clear();
        LOG_INF(kTag, "saved payment password rejected; cleared it");
      }
      purchaseAttemptInFlight_ = false;
      purchaseUsedSavedPassword_ = false;
      jobPurchaseWrongPassword_ = false;
      pendingPaySave_ = false;
      memset(lastPurchasePassword_, 0, sizeof(lastPurchasePassword_));
    }
    // The deep-linked action can never run without the book's details; drop it
    // here so a later ordinary visit does not surprise the user with an update.
    deepLinkAction_ = DeepLinkAction::None;
    deepLinkReturnAction_ = DeepLinkAction::None;
    jobNotice_.clear();
    jobReloadFavorites_ = false;
    jobFavoriteAfter_ = -1;
    jobFavoriteClassName_.clear();
    jobDeletedNovel_ = false;
    // A tag name /assort's dictionary does not carry is a user-input problem, not
    // a protocol failure; naming the tag is the only way to fix it.
    if (!jobTagMissing_.empty()) {
      showMessage(std::string(tr(STR_JJWXC_TAG_NOT_FOUND)) + ": " + jobTagMissing_, returnState_, jobWhat_);
      jobTagMissing_.clear();
      return;
    }
    fail(jobStatus_);
    return;
  }
  if (openAfter && !dlPath_.empty()) {
    if (jobRangeUnreadable_) {
      // "Cache and read this chapter" was aimed at a chapter this account cannot
      // read. The book was still packaged -- every other cached chapter belongs in
      // it -- but opening it would drop the reader into a document that does not
      // contain what they asked for, so say what happened instead.
      jobRangeUnreadable_ = false;
      jobTargetSpine_ = -1;
      showMessage(tr(STR_JJWXC_ERR_PURCHASE), returnState_, jobWhat_);
      return;
    }
    // The EPUB is complete and closed, so hand it to the reader instead of
    // entering a list state. One artefact per novel means this is the same file a
    // whole-book cache produces. jobTargetSpine_ is what makes "read this chapter"
    // land on that chapter: without it the reader restores progress.bin, so a tap
    // on chapter 50 of a book last read at chapter 120 reopened chapter 120.
    if (jobTargetSpine_ >= 0) writeReaderSpineTarget(dlPath_, static_cast<uint32_t>(jobTargetSpine_));
    jobTargetSpine_ = -1;
    handOffToReader(dlPath_);
    return;
  }
  jobRangeUnreadable_ = false;
  jobTargetSpine_ = -1;
  jobReloadFavorites_ = false;
  jobNextState_ = State::Menu;
  if (jobDeletedNovel_) {
    jobDeletedNovel_ = false;
    novel_ = JjwxcStore::NovelRecord{};
    tocSummary_ = JjwxcStore::TocSummary{};
    chapters_.clear();
    chapterStates_.clear();
    cloudHasProgress_ = false;
    cloudOrdinalValid_ = false;
    // The list is the result; a full-screen "deleted" page between the overlay
    // and it was one more flash for no new information.
    showToast(tr(STR_JJWXC_CACHE_DELETED));
  }
  if (jobFavoriteAfter_ >= 0) {
    isFavorite_ = jobFavoriteAfter_ != 0;
    favoriteKnown_ = true;
    if (isFavorite_ && !jobFavoriteClassName_.empty()) favoriteClassName_ = jobFavoriteClassName_;
    if (!isFavorite_) {
      favoriteClassId_.clear();
      favoriteClassName_.clear();
    }
    jobFavoriteClassName_.clear();
    jobFavoriteAfter_ = -1;
  }
  if (jobPurchaseConfirmPending_) {
    jobPurchaseConfirmPending_ = false;
    promptPurchaseConfirm();
    return;
  }
  // The worker cannot touch NVS/SD (it runs on a PSRAM stack); the successful
  // manually-typed password is handed over here and wrapped on the UI task.
  if (pendingPaySave_) {
    if (lastPurchasePassword_[0] != '\0' && !jjwxcpay::save(lastPurchasePassword_)) {
      LOG_ERR(kTag, "purchase succeeded but the payment password could not be remembered");
    }
    memset(lastPurchasePassword_, 0, sizeof(lastPurchasePassword_));
    pendingPaySave_ = false;
  }
  const auto purchaseAttemptDone = [this] {
    purchaseAttemptInFlight_ = false;
    purchaseUsedSavedPassword_ = false;
    jobPurchaseWrongPassword_ = false;
    memset(lastPurchasePassword_, 0, sizeof(lastPurchasePassword_));
  };
  if (jobPurchaseAlready_) {
    jobPurchaseAlready_ = false;
    purchaseAttemptDone();
    if (deepLinkReturnAction_ == DeepLinkAction::PurchaseChapter && pendingPurchaseOrdinal_ >= 0) {
      // Already owned: the reader's "acquire" chip must still fetch the body.
      jobNotice_.clear();
      cacheSingleChapter(static_cast<uint32_t>(pendingPurchaseOrdinal_), /*openAfter=*/false);
      return;
    }
    showMessage(tr(STR_JJWXC_ALREADY_BOUGHT), State::ChapterList);
    return;
  }
  if (jobPurchased_ && deepLinkReturnAction_ == DeepLinkAction::PurchaseChapter && pendingPurchaseOrdinal_ >= 0) {
    // Purchase done: chain the body fetch, then the cache completion below
    // closes the child activity and returns to the reader's Contents panel.
    jobPurchased_ = false;
    purchaseAttemptDone();
    jobNotice_.clear();
    cacheSingleChapter(static_cast<uint32_t>(pendingPurchaseOrdinal_), /*openAfter=*/false);
    return;
  }
  if (jobPurchased_) purchaseAttemptDone();
  jobPurchased_ = false;
  // The reader asked for one maintenance action and expects to be returned to
  // the book as soon as it finishes, rather than landing on a list screen.
  if (deepLinkReturnAction_ != DeepLinkAction::None) {
    const bool currentDone = deepLinkReturnAction_ == DeepLinkAction::UpdateCurrentChapter && wasCache;
    // A failed fetch leaves the app on its message screen: silently returning
    // would make a network error look like a successful catalogue update.
    const bool listDone = deepLinkReturnAction_ == DeepLinkAction::UpdateChapterList &&
                          after == State::ChapterList && !wasCache && jobStatus_ == jjwxc::Status::Ok;
    const bool chapterDone =
        (deepLinkReturnAction_ == DeepLinkAction::CacheChapter ||
         deepLinkReturnAction_ == DeepLinkAction::PurchaseChapter) &&
        wasCache;
    if (currentDone || listDone || chapterDone) {
      const DeepLinkAction action = deepLinkReturnAction_;
      deepLinkReturnAction_ = DeepLinkAction::None;
      const bool readable =
          pendingDeepLinkOrdinal_ >= 0 && static_cast<size_t>(pendingDeepLinkOrdinal_) < chapterStates_.size() &&
          chapterStates_[static_cast<size_t>(pendingDeepLinkOrdinal_)] ==
              static_cast<uint8_t>(JjwxcStore::ChapterState::Cached);
      finishDeepLink(action, /*completed=*/true, readable);
      return;
    }
  }
  if (!jobNotice_.empty()) {
    std::string notice = std::move(jobNotice_);
    jobNotice_.clear();
    showMessage(std::move(notice), after, jobWhat_);
    return;
  }
  if (wasCache) {
    showMessage(cacheSummary(), after, jobWhat_);
    return;
  }
  if (after == State::BookDetail && deepLinkAction_ != DeepLinkAction::None) {
    const DeepLinkAction action = deepLinkAction_;
    deepLinkAction_ = DeepLinkAction::None;
    enter(after);
    if (action == DeepLinkAction::UpdateCurrentChapter) {
      refreshCurrentChapter();
    } else if (action == DeepLinkAction::UpdateChapterList) {
      // Repackage placeholders after the list refresh: new chapters then appear
      // in the reader's Contents panel as "not cached" without downloading them.
      loadChapters(State::ChapterList, /*repackageAfter=*/true);
    } else if (action == DeepLinkAction::OpenBookDetail) {
      // The detail page is the destination; Back (handled in loop()) closes the
      // child and returns to the reader.
    } else {
      int ordinal = -1;
      for (size_t i = 0; i < chapters_.size(); ++i) {
        if (chapters_[i].chapterId == deepLinkChapterId_) {
          ordinal = static_cast<int>(i);
          break;
        }
      }
      uint32_t found = 0;
      if (ordinal < 0 && JjwxcStore::findOrdinalForChapter(novelId_.c_str(), deepLinkChapterId_, found)) {
        ordinal = static_cast<int>(found);
      }
      if (ordinal < 0 || static_cast<size_t>(ordinal) >= chapters_.size()) {
        LOG_ERR(kTag, "deep link chapter %lu not in the local catalogue",
                static_cast<unsigned long>(deepLinkChapterId_));
        showMessage(tr(STR_JJWXC_ERR_PROTOCOL), State::BookDetail);
        deepLinkReturnAction_ = DeepLinkAction::None;
        return;
      }
      pendingDeepLinkOrdinal_ = ordinal;
      if (action == DeepLinkAction::CacheChapter) {
        cacheSingleChapter(static_cast<uint32_t>(ordinal), /*openAfter=*/false);
      } else {
        promptPurchase(static_cast<uint32_t>(ordinal));
      }
    }
    // A missing progress record (or a missing offline catalogue) leaves the app
    // on a Message page. There is nothing to return to as a success; let the
    // user read the message and Back out manually.
    if (state_ == State::Message) deepLinkReturnAction_ = DeepLinkAction::None;
    return;
  }
  enter(after);
}

// ---- individual jobs -------------------------------------------------------

bool JjwxcActivity::onWapShelfEntry(void* ctx, const jjwxc::WapShelfEntry& e, size_t index) {
  // WapShelfEntry is a redeclaration of ShelfEntry with the same layout. The asserts
  // are here rather than in the header because this is the only place both types are
  // visible together: JjwxcWapShelf.h deliberately does not include JjwxcProtocol.h,
  // which would drag the JSON SAX library into every host test of the shelf parser.
  static_assert(sizeof(jjwxc::WapShelfEntry) == sizeof(jjwxc::ShelfEntry), "WAP shelf entry layout drifted");
  static_assert(offsetof(jjwxc::WapShelfEntry, novelId) == offsetof(jjwxc::ShelfEntry, novelId), "novelId offset");
  static_assert(offsetof(jjwxc::WapShelfEntry, novelName) == offsetof(jjwxc::ShelfEntry, novelName), "novelName offset");
  static_assert(offsetof(jjwxc::WapShelfEntry, authorName) == offsetof(jjwxc::ShelfEntry, authorName),
                "authorName offset");
  static_assert(offsetof(jjwxc::WapShelfEntry, coverUrl) == offsetof(jjwxc::ShelfEntry, coverUrl), "coverUrl offset");
  static_assert(offsetof(jjwxc::WapShelfEntry, lastChapterName) == offsetof(jjwxc::ShelfEntry, lastChapterName),
                "lastChapterName offset");
  static_assert(offsetof(jjwxc::WapShelfEntry, chapterCount) == offsetof(jjwxc::ShelfEntry, chapterCount),
                "chapterCount offset");
  static_assert(offsetof(jjwxc::WapShelfEntry, lastChapterId) == offsetof(jjwxc::ShelfEntry, lastChapterId),
                "lastChapterId offset");
  static_assert(offsetof(jjwxc::WapShelfEntry, lastChapterTime) == offsetof(jjwxc::ShelfEntry, lastChapterTime),
                "lastChapterTime offset");
  static_assert(offsetof(jjwxc::WapShelfEntry, renewStatus) == offsetof(jjwxc::ShelfEntry, renewStatus),
                "renewStatus offset");
  static_assert(std::is_trivially_copyable<jjwxc::WapShelfEntry>::value, "WapShelfEntry must stay trivially copyable");

  jjwxc::ShelfEntry copy;
  memcpy(&copy, &e, sizeof(copy));
  return onShelfEntry(ctx, copy, index);
}

jjwxc::Status JjwxcActivity::fetchWapShelf() {
  if (!ensureWapSession()) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  if (scratch_ == nullptr || scratchCap_ < kWapPageRawCap + 32 * 1024) {
    LOG_ERR(kTag, "scratch too small for a WAP shelf page (%u bytes)", (unsigned)scratchCap_);
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  wap_->setNowUnix(cookieClock());

  auto* page = reinterpret_cast<char*>(scratch_);
  size_t collected = 0;
  uint32_t requested = 1;

  for (int hop = 0; hop < kMaxShelfPages; ++hop) {
    if (jobCancel_.load()) return jjwxc::Status::Cancelled;

    // `fav_filter_action=1` is what makes this the live filtered view. Without
    // it, `?c=<id>` returns a stale copy that keeps showing deleted books.
    char url[160];
    const int n = shelfClassId_.empty()
                      ? snprintf(url, sizeof(url), "%s/my/fav?p=%u", jjwxc::kWapHost, (unsigned)requested)
                      : snprintf(url, sizeof(url), "%s/my/fav?fav_filter_action=1&c=%s&p=%u", jjwxc::kWapHost,
                                 shelfClassId_.c_str(), (unsigned)requested);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return jjwxc::Status::Protocol;

    BufferCollector col{page, kWapPageRawCap, 0, false};
    // maxRedirects stays 0 on purpose. A dead sid answers 302 -> /my/login, and
    // following it would hand back a login page that parses as an empty shelf -- so
    // "no books" and "signed out" would become the same screen.
    jjwxc::WapOptions opt;
    jjwxc::WapResponse res;
    const int http = wap_->get(url, opt, &collectBuffer, &col, res);
    if (http < 0) {
      jobDetailCode_ = http;
      LOG_ERR(kTag, "shelf request failed (%d) url=%.60s", http, url);
      return jjwxc::Status::Network;
    }
    if (res.sawRedirect) {
      LOG_ERR(kTag, "shelf redirected to %.80s -- the session is dead", res.location);
      return jjwxc::Status::NeedLogin;
    }
    if (http != 200) {
      jobDetailCode_ = http;
      LOG_ERR(kTag, "shelf page %u returned %d", (unsigned)requested, http);
      return jjwxc::Status::Server;
    }
    if (col.overflow) {
      LOG_ERR(kTag, "shelf page %u exceeded %u bytes", (unsigned)requested, (unsigned)kWapPageRawCap);
      return jjwxc::Status::TooLarge;
    }
    if (col.len == 0) return jjwxc::Status::EmptyResult;
    page[col.len] = '\0';

    // The page is GB18030 and the parser wants UTF-8. transcodeGbkInPlace writes into
    // the same buffer, which is why the collection cap is well under scratchCap_.
    size_t textLen = col.len;
    const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
    if (enc == txt_encoding::Encoding::Gbk) {
      const auto conv =
          txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
      if (conv.rawLength == 0 || conv.utf8Length == 0) {
        LOG_ERR(kTag, "shelf page %u would not transcode (%u raw bytes)", (unsigned)requested, (unsigned)col.len);
        return jjwxc::Status::Protocol;
      }
      textLen = conv.utf8Length;
      page[textLen] = '\0';
    }

    jjwxc::WapShelfPage pager;
    // `requested` is passed rather than left to the parser: the caller knows what it
    // asked for, while the highlighted pager entry has only ever been observed on one
    // capture. Without it an undetectable current page forces next=0 and the walk ends
    // at page one.
    const size_t got = jjwxc::parseWapShelf(page, textLen, &onWapShelfEntry, this, &pager, requested);
    collected += got;
    LOG_INF(kTag, "shelf page %u: %u books, pager current=%u next=%u max=%u (%u bytes, gbk=%d)", (unsigned)requested,
            (unsigned)got, (unsigned)pager.current, (unsigned)pager.next, (unsigned)pager.max, (unsigned)col.len,
            (int)(enc == txt_encoding::Encoding::Gbk));

    if (jobCancel_.load()) return jjwxc::Status::Cancelled;
    if (pager.next == 0 || pager.next <= requested) break;
    if (collected >= kMaxRows) {
      LOG_INF(kTag, "shelf stopped at %u books (kMaxRows=%u)", (unsigned)collected, (unsigned)kMaxRows);
      break;
    }
    requested = pager.next;
  }

  if (collected == 0) {
    // Not an error worth a popup on its own terms -- an account with no favourites is
    // a legitimate state -- but EmptyResult is what the existing UI already words.
    return jjwxc::Status::EmptyResult;
  }
  return jjwxc::Status::Ok;
}

namespace {

// Scans for the per-session watermark font's name, which is what the outline-hash
// table is keyed against. The shape is fixed: `jjwxcfont_` plus five lowercase
// alphanumerics, observed identical across every capture and every chapter.
bool findWapFontName(const char* page, size_t len, char* out, size_t outCap) {
  static const char kPrefix[] = "jjwxcfont_";
  constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
  constexpr size_t kSuffixLen = 5;
  if (outCap < kPrefixLen + kSuffixLen + 1) return false;
  for (size_t i = 0; i + kPrefixLen + kSuffixLen <= len; ++i) {
    if (memcmp(page + i, kPrefix, kPrefixLen) != 0) continue;
    bool ok = true;
    for (size_t k = 0; k < kSuffixLen; ++k) {
      const char c = page[i + kPrefixLen + k];
      const bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
      if (!valid) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;
    memcpy(out, page + i, kPrefixLen + kSuffixLen);
    out[kPrefixLen + kSuffixLen] = '\0';
    return true;
  }
  return false;
}

// Bounded byte search. memmem() is a GNU extension and is not declared by the newlib
// this project builds against, which is the same reason the repo carries its own
// civil-from-days conversion instead of timegm().
const uint8_t* findBytes(const uint8_t* hay, size_t hayLen, const char* needle) {
  const size_t nl = strlen(needle);
  if (nl == 0 || nl > hayLen) return nullptr;
  for (size_t i = 0; i + nl <= hayLen; ++i) {
    if (memcmp(hay + i, needle, nl) == 0) return hay + i;
  }
  return nullptr;
}

// Backs jjwxc::PuaLookup with the map the font parser built. Returning 0 for an
// unmapped code point is deliberate and load-bearing: wapPageToText counts those and
// reports them, so a font whose glyph set is not in the shipped table fails loudly
// instead of quietly putting a box or a wrong character into the book.
size_t puaLookupFromMap(void* ctx, uint32_t codePoint, char out[4]) {
  const auto* map = static_cast<const jjwxc::PuaMap*>(ctx);
  if (map == nullptr) return 0;
  const jjwxc::PuaEntry* e = map->find(codePoint);
  if (e == nullptr || e->utf8Len == 0) return 0;
  memcpy(out, e->utf8, e->utf8Len);
  return e->utf8Len;
}

}  // namespace

jjwxc::Status JjwxcActivity::fetchWapVipChapter(const char* novelId, const uint32_t chapterId,
                                                jjwxc::ChapterMeta& meta, jjwxc::TextSink onText, void* textCtx) {
  meta = jjwxc::ChapterMeta{};
  if (novelId == nullptr || novelId[0] == '\0' || chapterId == 0 || onText == nullptr) return jjwxc::Status::Protocol;
  if (!ensureWapSession()) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  if (scratch_ == nullptr || scratchCap_ < kWapVipScratchBytes) {
    LOG_ERR(kTag, "scratch too small for the WAP VIP chain: have %u, need %u", (unsigned)scratchCap_,
            (unsigned)kWapVipScratchBytes);
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  wap_->setNowUnix(cookieClock());

  uint8_t* pageSlice = scratch_;
  uint8_t* fontSlice = scratch_ + kWapPageSlice;
  uint8_t* workSlice = fontSlice + kWapFontSlice;
  uint8_t* bodySlice = workSlice + kWapWorkSlice;
  uint8_t* textSlice = bodySlice + kWapBodySlice;
  auto* mapSlice = reinterpret_cast<jjwxc::PuaMap*>(textSlice + kWapTextSlice);

  char url[128];
  const int n = snprintf(url, sizeof(url), "%s/vip/%s/%lu", jjwxc::kWapHost, novelId, (unsigned long)chapterId);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return jjwxc::Status::Protocol;

  BufferCollector col{reinterpret_cast<char*>(pageSlice), kWapPageRawCap, 0, false};
  // maxRedirects stays 0. The VIP path's dead-session answer is NOT always a redirect
  // (see below), and following one would hand back a login page that the decrypt chain
  // would then report as a malformed chapter.
  jjwxc::WapOptions opt;
  jjwxc::WapResponse res;
  // Diagnostics for the failure paths below. "Progress stuck at 0%, then 网络错误" is
  // unactionable as reported: it does not say which of the two requests this chapter
  // makes failed, how long it took, or whether the session was even attached. Every
  // failure path now says all three, because a 45 s transport timeout retried once is
  // 90 s of a frozen progress bar with nothing on screen to explain it.
  const uint32_t t0 = millis();
  const auto failInfo = [&](const char* stage, int code) {
    LOG_ERR(kTag, "VIP ch %lu %s failed: code=%d after %lums cookie=%uB%s url=%.100s", (unsigned long)chapterId, stage,
            code, (unsigned long)(millis() - t0), (unsigned)strlen(wapIo_->cookie),
            res.cookieHeaderTruncated ? " TRUNCATED" : "", res.finalUrl);
  };
  const int http = wap_->get(url, opt, &collectBuffer, &col, res);
  if (http < 0) {
    jobDetailCode_ = http;
    failInfo("page request", http);
    return jjwxc::Status::Network;
  }
  if (res.cookieHeaderTruncated) {
    // buildHeader() stops at the first cookie that does not fit, so a truncated header
    // may be missing sid entirely -- which the server answers as an anonymous request.
    // Loud, because every symptom downstream of it looks like something else.
    LOG_ERR(kTag, "VIP ch %lu: cookie header hit the %u-byte cap; the session may not have been sent",
            (unsigned long)chapterId, (unsigned)jjwxc::WapIo::kCookieCap);
  }
  if (res.sawRedirect) {
    // Classified in the library, not here, so a host test can pin it: this exact rule
    // was guessed wrong once already, and the wrong guess aborted a whole-book download
    // at the first unpurchased chapter while telling the user to sign in again.
    const auto meaning = jjwxc::wapClassifyChapterRedirect(res.location);
    switch (meaning) {
      case jjwxc::WapRedirectMeaning::Purchase:
        // 302 -> /buy/<novelId>/<chapterId>. The service named the chapter, not the
        // credential, so this is per-chapter: stub it and carry on. WAP does not
        // auto-purchase -- it just sends you to the purchase page.
        LOG_INF(kTag, "VIP chapter %lu is not purchased (%.80s)", (unsigned long)chapterId, res.location);
        return jjwxc::Status::PurchaseRequired;
      case jjwxc::WapRedirectMeaning::SessionDead:
        LOG_ERR(kTag, "VIP chapter %lu redirected to the login page -- the session is dead", (unsigned long)chapterId);
        return jjwxc::Status::NeedLogin;
      case jjwxc::WapRedirectMeaning::Unknown:
      default:
        // A shape nobody has observed. Reporting it as "not purchased" would silently
        // fill a book with placeholders; reporting it as a dead session would send the
        // user to log in for nothing. Say what actually happened.
        LOG_ERR(kTag, "VIP chapter %lu redirected somewhere unrecognised: %.120s", (unsigned long)chapterId,
                res.location);
        return jjwxc::Status::Protocol;
    }
  }
  if (http != 200) {
    jobDetailCode_ = http;
    return jjwxc::Status::Server;
  }
  if (col.overflow) {
    LOG_ERR(kTag, "VIP chapter %lu page exceeded %u bytes", (unsigned long)chapterId, (unsigned)kWapPageRawCap);
    return jjwxc::Status::TooLarge;
  }
  if (col.len == 0) return jjwxc::Status::EmptyResult;
  pageSlice[col.len] = '\0';

  size_t pageLen = col.len;
  const bool wasGbk = txt_encoding::detect(pageSlice, col.len, true) == txt_encoding::Encoding::Gbk;
  if (wasGbk) {
    // In place, so the slice has to hold the raw page and its UTF-8 form at once --
    // which is what kWapPageSlice is sized for and why the collection cap is far below it.
    const auto conv = txt_encoding::transcodeGbkInPlace(pageSlice, col.len, kWapPageSlice, true);
    if (conv.rawLength == 0 || conv.utf8Length == 0) {
      LOG_ERR(kTag, "VIP chapter %lu would not transcode (%u raw bytes)", (unsigned long)chapterId, (unsigned)col.len);
      return jjwxc::Status::Protocol;
    }
    pageLen = conv.utf8Length;
    pageSlice[pageLen] = '\0';
  }

  jjwxc::WapVars vars;
  if (!jjwxc::parseWapVars(reinterpret_cast<const char*>(pageSlice), pageLen, vars)) {
    // No contentvars means there is no body to decrypt. Two causes, and telling them
    // apart matters: one is recoverable by signing in again, the other is not.
    //
    // Measured anonymously (so nothing could be charged): GET /vip/<nid>/<cid> with no
    // cookie answers HTTP 200 -- NOT a redirect -- with a 10,240-byte login page whose
    // one stable ASCII marker is a login link carrying the requested URL back as its
    // referer. Everything else with no contentvars is treated as "not readable".
    if (findBytes(pageSlice, pageLen, "/my/login?referer=") != nullptr) {
      LOG_ERR(kTag, "VIP chapter %lu: the service answered with a login page (%u bytes)", (unsigned long)chapterId,
              (unsigned)col.len);
      return jjwxc::Status::NeedLogin;
    }
    // NOT probed against a live unpurchased chapter: requesting one through the App API
    // was measured to trigger the account's own server-side auto-purchase (code 1013,
    // "余额不足"), and whether WAP does the same is unknown. Spending the user's money
    // to learn the shape of an error page is not a trade worth making, so this branch is
    // reasoned from "there is no body here" rather than matched against observed text.
    LOG_INF(kTag, "VIP chapter %lu has no contentvars (%u bytes, gbk=%d) -- treating as not readable",
            (unsigned long)chapterId, (unsigned)col.len, (int)wasGbk);
    return jjwxc::Status::PurchaseRequired;
  }

  char fontName[24] = {};
  if (!findWapFontName(reinterpret_cast<const char*>(pageSlice), pageLen, fontName, sizeof(fontName))) {
    // A VIP page always names its watermark font. Without one there is no way to
    // resolve the PUA code points, and emitting them raw would put boxes in the book.
    LOG_ERR(kTag, "VIP chapter %lu names no watermark font", (unsigned long)chapterId);
    return jjwxc::Status::Protocol;
  }

  char fontUrl[160];
  const int fn = snprintf(fontUrl, sizeof(fontUrl), "https://static.jjwxc.net/tmp/fonts/%s.ttf?h=m.jjwxc.net", fontName);
  if (fn <= 0 || static_cast<size_t>(fn) >= sizeof(fontUrl)) return jjwxc::Status::Protocol;

  BufferCollector fontCol{reinterpret_cast<char*>(fontSlice), kWapFontSlice - 1, 0, false};
  jjwxc::WapOptions fontOpt;
  jjwxc::WapResponse fontRes;
  const uint32_t tFont = millis();
  dlStage_ = 1;  // fetching the per-chapter watermark font
  const int fontHttp = wap_->get(fontUrl, fontOpt, &collectBuffer, &fontCol, fontRes);
  if (fontHttp < 0 || fontHttp != 200 || fontCol.len == 0 || fontCol.overflow) {
    // This is a SECOND host (static.jjwxc.net), so it costs a fresh TLS handshake and a
    // host switch on the shared session. Naming the stage matters: a timeout here looks
    // identical to one on the page request unless the log says which.
    LOG_ERR(kTag, "VIP ch %lu font %s failed: http=%d bytes=%u overflow=%d after %lums (total %lums) url=%.100s",
            (unsigned long)chapterId, fontName, fontHttp, (unsigned)fontCol.len, (int)fontCol.overflow,
            (unsigned long)(millis() - tFont), (unsigned long)(millis() - t0), fontRes.finalUrl);
    return fontHttp < 0 ? jjwxc::Status::Network : jjwxc::Status::Protocol;
  }

  dlStage_ = 2;  // resolving the glyph map / decoding the body
  new (mapSlice) jjwxc::PuaMap();
  jjwxc::FontMapResult fm;
  if (!jjwxc::buildPuaMap(fontSlice, fontCol.len, *mapSlice, fm)) {
    // Reported, never papered over: this means the site changed the glyph set and the
    // shipped 200-row outline table no longer covers it, which needs a firmware update
    // rather than a retry. A partial map would put wrong characters in the book.
    LOG_ERR(kTag, "font %s: status=%d pua=%u resolved=%u unresolved=%u tableRows=%u", fontName, (int)fm.status,
            (unsigned)fm.puaCount, (unsigned)fm.resolved, (unsigned)fm.unresolved,
            (unsigned)jjwxc::glyphTableSize());
    return jjwxc::Status::Decrypt;
  }

  jjwxc::WapBuffers bufs;
  bufs.work = reinterpret_cast<char*>(workSlice);
  bufs.workCap = kWapWorkSlice;
  bufs.body = reinterpret_cast<char*>(bodySlice);
  // The CSS displacement step INSERTS text, so the fragment buffer needs slack beyond
  // the decrypted length -- measured at 36 bytes for one span, and a few hundred bytes
  // covers a whole chapter. The slice is far larger than the fragment, so it has it.
  bufs.bodyCap = kWapBodySlice;
  bufs.text = reinterpret_cast<char*>(textSlice);
  bufs.textCap = kWapTextSlice;

  const jjwxc::WapResult wr = jjwxc::wapPageToText(reinterpret_cast<const char*>(pageSlice), pageLen,
                                                  &puaLookupFromMap, mapSlice, bufs);
  if (wr.status != jjwxc::WapStatus::Ok) {
    // JsVerMismatch is the one worth distinguishing: it means the service changed the
    // algorithm and no amount of retrying will help, so the user should be told to
    // upgrade rather than to try again.
    LOG_ERR(kTag, "VIP chapter %lu decode failed: %s (pageJsVer='%s', pua=%u unresolved=%u)",
            (unsigned long)chapterId, jjwxc::wapStatusText(wr.status), wr.pageJsVer, (unsigned)wr.puaTotal,
            (unsigned)wr.puaUnresolved);
    if (wr.status == jjwxc::WapStatus::JsVerMismatch) {
      snprintf(jobServerMessage_, sizeof(jobServerMessage_), "jsver=%s", wr.pageJsVer);
      return jjwxc::Status::BadVersion;
    }
    if (wr.status == jjwxc::WapStatus::Buffer) return jjwxc::Status::TooLarge;
    return wr.status == jjwxc::WapStatus::MissingVars ? jjwxc::Status::PurchaseRequired : jjwxc::Status::Decrypt;
  }
  if (wr.puaUnresolved != 0) {
    // The chain reports Ok with unresolved code points counted; emitting them would put
    // characters in the book that no one can verify. Refuse instead.
    LOG_ERR(kTag, "VIP chapter %lu left %u of %u watermark code points unresolved", (unsigned long)chapterId,
            (unsigned)wr.puaUnresolved, (unsigned)wr.puaTotal);
    return jjwxc::Status::Decrypt;
  }
  if (wr.textLen == 0) return jjwxc::Status::EmptyResult;

  // Emitted only once the whole chain has succeeded, so a rejected chapter can never
  // leak partial bytes into the book. That is a stronger guarantee than the App API
  // path gives, where `content` can arrive before the reply's own `code` does.
  if (!onText(textCtx, reinterpret_cast<const char*>(textSlice), wr.textLen)) {
    return jjwxc::Status::Cancelled;
  }
  meta.textBytes = wr.textLen;
  meta.isVip = true;
  // One line per chapter is too chatty for a 200-chapter download, but the FIRST
  // success is worth seeing in full: it is the only place the whole chain's sizes and
  // cost are visible together, and it is what distinguishes "the chain works and
  // chapter 40 is different" from "the chain has never worked".
  if (!vipChainProven_) {
    vipChainProven_ = true;
    LOG_INF(kTag, "VIP chain ok: ch %lu page=%uB gbk=%d font=%s(%uB) text=%uB pua=%u/%u in %lums",
            (unsigned long)chapterId, (unsigned)col.len, (int)wasGbk, fontName, (unsigned)fontCol.len,
            (unsigned)wr.textLen, (unsigned)wr.puaTotal - (unsigned)wr.puaUnresolved, (unsigned)wr.puaTotal,
            (unsigned long)(millis() - t0));
  }
  // The page's own contentvars carry a chapterid, so this is the SERVER's answer to
  // "which chapter did I just give you", not an echo of what we asked for -- which is
  // what makes the caller's mismatch gate meaningful on this path rather than vacuous.
  meta.chapterId = vars.chapterId;
  // meta.size stays 0 on purpose: the WAP page carries no chapter size, and the
  // catalogue already supplied one. A 0 here must not overwrite it -- the caller only
  // copies non-zero values, and that rule is what keeps cache invalidation working.
  return jjwxc::Status::Ok;
}

void JjwxcActivity::loadShelf() {
  if (!haveSession()) {
    showMessage(tr(STR_JJWXC_NEED_LOGIN), State::Menu);
    return;
  }
  withNetwork([this] {
    jobWhat_ = tr(STR_JJWXC_SHELF);
    jobNextState_ = State::Shelf;
    // A failed request must not cost the user the shelf they already had, so the
    // vector is only cleared once the first entry of the new response arrives
    // (onShelfEntry). WeRead follows the same rule: it swaps in a result only
    // when the fetch completed.
    returnState_ = shelf_.empty() ? State::Menu : State::Shelf;
    launchJob(
        [this] {
          jobStatus_ = fetchWapShelf();
          // Only the account's whole shelf is a valid snapshot: a favourites
          // filter would make "3 new" mean "3 of the ones in this folder", and
          // the next reader would trust it for every book.
          if (jobStatus_ == jjwxc::Status::Ok && shelfClassId_.empty()) saveShelfSnapshot();
        },
        tr(STR_JJWXC_LOADING));
  });
}

bool JjwxcActivity::saveShelfSnapshot() {
  // A filtered favourites view is not the account's shelf: writing it would make
  // "N new chapters" mean "N of the ones in this folder" for every later reader.
  if (!shelfClassId_.empty()) return false;
  if (shelf_.empty()) return false;

  const size_t count = std::min<size_t>(shelf_.size(), jjwxcUpdates::kMaxEntries);
  auto buf = memory::makePsramByteBufferUninitializedNoThrow(count * sizeof(jjwxcUpdates::Entry));
  if (!buf) {
    LOG_ERR(kTag, "no PSRAM for %u update rows", (unsigned)count);
    return false;
  }
  auto* rows = reinterpret_cast<jjwxcUpdates::Entry*>(buf.get());
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  for (size_t i = 0; i < count; ++i) {
    const jjwxc::ShelfEntry& src = shelf_[i];
    rows[i] = jjwxcUpdates::Entry{};
    snprintf(rows[i].novelId, sizeof(rows[i].novelId), "%s", src.novelId);
    rows[i].lastChapterId = src.lastChapterId;
    rows[i].chapterCount = src.chapterCount;
    rows[i].fetchedAt = now;
    rows[i].renewStatus = src.renewStatus;
  }
  return jjwxcUpdates::saveSnapshot(rows, count);
}

void JjwxcActivity::runUpdateCheck() {
  if (!haveSession()) {
    // The shelf could not answer anyway; returning at once is what lets the
    // Library say "sign in from the JJWXC app" instead of opening a login page.
    LOG_INF(kTag, "update check skipped: no WAP session");
    finishDeepLink(DeepLinkAction::RefreshUpdates, false);
    return;
  }
  withNetwork(
      [this] {
        jobWhat_ = tr(STR_JJWXC_CHECK_UPDATES);
        jobNextState_ = State::Menu;
        returnState_ = State::Menu;
        updateCheckOnly_ = true;
        const bool started = launchJob(
            [this] {
              // Clear the favourites filter: this run must describe the whole
              // account shelf, which is what the snapshot is keyed on.
              shelfClassId_.clear();
              jobStatus_ = fetchWapShelf();
              if (jobStatus_ == jjwxc::Status::Ok) saveShelfSnapshot();
            },
            tr(STR_JJWXC_CHECK_UPDATES));
        if (!started) {
          updateCheckOnly_ = false;
          finishDeepLink(DeepLinkAction::RefreshUpdates, false);
        }
      },
      State::Menu, /*returnDeepLinkOnCancel=*/true);
}

bool JjwxcActivity::onShelfEntry(void* ctx, const jjwxc::ShelfEntry& e, size_t) {
  auto* self = static_cast<JjwxcActivity*>(ctx);
  if (self->jobCancel_.load()) return false;
  // Only the worker task touches shelf_ while a job runs: loop() returns early on
  // busyScreen_ and rebuildRows() never sees State::Shelf until pollJob() enters
  // it, so this needs no lock. jobReplacedTarget_ is written here and reset by
  // startJob() on the UI task before the task is created.
  if (!self->jobReplacedTarget_) {
    self->shelf_.clear();
    self->jobReplacedTarget_ = true;
  }
  if (self->shelf_.size() < kMaxRows) self->shelf_.push_back(e);
  return true;
}

void JjwxcActivity::promptSearchKeyword(const uint8_t type) {
  // Anonymous: /search needs no sid and no token, so unlike the shelf this must NOT be
  // gated on haveSession(). Gating it would send a signed-out user to a login page for a
  // request that never carried a credential in the first place.
  const auto handler = [this, type](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      enter(State::Menu);
      return;
    }
    const auto& kb = std::get<KeyboardResult>(result.data);
    keyword_ = kb.text;
    searchType_ = type;
    if (keyword_.empty()) {
      enter(State::Menu);
      return;
    }
    runSearch();
  };
  startActivityForResultWith<KeyboardEntryActivity>(
      handler, type == 2 ? tr(STR_JJWXC_SEARCH_AUTHOR_HINT) : tr(STR_JJWXC_SEARCH_HINT), keyword_, 24,
      InputType::Text);
}

void JjwxcActivity::runSearch() {
  withNetwork(
      [this] {
        jobWhat_ = tr(STR_JJWXC_SEARCH);
        jobNextState_ = State::SearchResults;
        // Same rule as the shelf: a failed search must not discard the previous
        // results, and either way Back returns to the unified search page.
        returnState_ = search_.empty() ? State::AdvancedSearch : State::SearchResults;
        launchJob(
            [this] {
              jobStatus_ = fetchWapSearch();
            },
            tr(STR_JJWXC_LOADING));
      },
      State::AdvancedSearch);
}

// parseWapSearch emits WapShelfEntry because the search row is a subset of the shelf row;
// the results list holds SearchItem. Three fields exist on both, and the other three
// SearchItem members (sizeText, introShort, isVipMonth) are not on this page at all, so
// they stay empty rather than being invented.
bool JjwxcActivity::onSearchEntryAdapter(void* ctx, const jjwxc::WapShelfEntry& e, size_t index) {
  jjwxc::SearchItem item;
  snprintf(item.novelId, sizeof(item.novelId), "%s", e.novelId);
  snprintf(item.novelName, sizeof(item.novelName), "%s", e.novelName);
  snprintf(item.authorName, sizeof(item.authorName), "%s", e.authorName);
  return onSearchEntry(ctx, item, index);
}

jjwxc::Status JjwxcActivity::fetchWapSearch() {
  if (!ensureWapSession()) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  if (scratch_ == nullptr || scratchCap_ < kWapPageRawCap + 32 * 1024) {
    LOG_ERR(kTag, "scratch too small for a WAP search page (%u bytes)", (unsigned)scratchCap_);
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  wap_->setNowUnix(cookieClock());

  char encoded[96];
  if (jjwxc::wapUrlEncode(keyword_.c_str(), keyword_.size(), encoded, sizeof(encoded)) == 0) {
    LOG_ERR(kTag, "search keyword would not fit the encoded cap (%u bytes)", (unsigned)keyword_.size());
    return jjwxc::Status::Protocol;
  }

  auto* page = reinterpret_cast<char*>(scratch_);
  size_t collected = 0;
  uint32_t requested = 1;

  for (int hop = 0; hop < kMaxShelfPages; ++hop) {
    if (jobCancel_.load()) return jjwxc::Status::Cancelled;

    char url[160];
    // t=1 searches titles, t=2 searches author names. The site's own pager
    // preserves the parameter, but this loop builds its page URL itself, so the
    // selector has to be carried here too.
    const int n = (requested == 1)
                      ? snprintf(url, sizeof(url), "%s/search?kw=%s&t=%u", jjwxc::kWapHost, encoded,
                                 (unsigned)searchType_)
                      : snprintf(url, sizeof(url), "%s/search/index/page/%u?kw=%s&t=%u", jjwxc::kWapHost,
                                 (unsigned)requested, encoded, (unsigned)searchType_);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return jjwxc::Status::Protocol;

    BufferCollector col{page, kWapPageRawCap, 0, false};
    jjwxc::WapOptions opt;
    jjwxc::WapResponse res;
    const int http = wap_->get(url, opt, &collectBuffer, &col, res);
    if (http < 0) {
      jobDetailCode_ = http;
      LOG_ERR(kTag, "search request failed (%d) url=%.60s", http, url);
      return jjwxc::Status::Network;
    }
    if (res.sawRedirect) {
      // Anonymous endpoint, so a redirect here is not a dead session; it is a shape this
      // code has never seen. Say so instead of pretending it is an empty result.
      LOG_ERR(kTag, "search redirected to %.80s -- unexpected for an anonymous endpoint", res.location);
      return jjwxc::Status::Protocol;
    }
    if (http != 200) {
      jobDetailCode_ = http;
      LOG_ERR(kTag, "search page %u returned %d", (unsigned)requested, http);
      return jjwxc::Status::Server;
    }
    if (col.overflow) {
      LOG_ERR(kTag, "search page %u exceeded %u bytes", (unsigned)requested, (unsigned)kWapPageRawCap);
      return jjwxc::Status::TooLarge;
    }
    if (col.len == 0) return jjwxc::Status::EmptyResult;
    page[col.len] = '\0';

    size_t textLen = col.len;
    const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
    if (enc == txt_encoding::Encoding::Gbk) {
      const auto conv =
          txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
      if (conv.rawLength == 0 || conv.utf8Length == 0) {
        LOG_ERR(kTag, "search page %u would not transcode (%u raw bytes)", (unsigned)requested, (unsigned)col.len);
        return jjwxc::Status::Protocol;
      }
      textLen = conv.utf8Length;
      page[textLen] = '\0';
    }

    jjwxc::WapShelfPage pager;
    const size_t got = jjwxc::parseWapSearch(page, textLen, &onSearchEntryAdapter, this, &pager, requested);
    collected += got;
    LOG_INF(kTag, "search page %u: %u books, pager current=%u next=%u max=%u (%u bytes, gbk=%d)", (unsigned)requested,
            (unsigned)got, (unsigned)pager.current, (unsigned)pager.next, (unsigned)pager.max, (unsigned)col.len,
            (int)(enc == txt_encoding::Encoding::Gbk));
    if (pager.next == 0 || pager.next <= requested) break;
    requested = pager.next;
  }

  if (collected == 0) return jjwxc::Status::EmptyResult;
  searchTotal_ = static_cast<uint32_t>(collected);
  return jjwxc::Status::Ok;
}

// ---- discovery -------------------------------------------------------------

void JjwxcActivity::loadDiscoverSource(const size_t index) {
  if (index >= kDiscoverSourceCount) return;
  char url[256];
  const int n = snprintf(url, sizeof(url), "%s%s", jjwxc::kWapHost, kDiscoverSources[index].path);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return;
  discoverTitle_ = trId(kDiscoverSources[index].label);
  discoverListOrigin_ = State::Discover;
  discoverRequestUrl_.assign(url);
  withNetwork(
      [this] {
        jobWhat_ = discoverTitle_;
        jobNextState_ = State::DiscoverList;
        returnState_ = State::Discover;
        jobAppendTarget_ = false;
        jobReplacedTarget_ = false;
        discoverNextUrl_.clear();
        launchJob(
            [this] {
              jobStatus_ = fetchDiscoverPage(discoverRequestUrl_, /*append=*/false, 1);
            },
            tr(STR_JJWXC_LOADING));
      },
      State::Discover);
}

void JjwxcActivity::loadDiscoverPage(const std::string& url, const bool append) {
  if (url.empty()) return;
  if (append && discover_.size() >= kMaxRows) return;
  discoverRequestUrl_ = url;
  const uint32_t requested = append ? discoverPage_ + 1 : 1;
  withNetwork(
      [this, append, requested] {
        jobWhat_ = discoverTitle_;
        jobNextState_ = State::DiscoverList;
        returnState_ = discoverListOrigin_;
        // The sink clears only when the job is a replacement; an appended page
        // must keep what the first page already delivered.
        jobAppendTarget_ = append;
        jobReplacedTarget_ = append;
        launchJob(
            [this, append, requested] {
              jobStatus_ = fetchDiscoverPage(discoverRequestUrl_, append, requested);
            },
            tr(STR_JJWXC_LOADING));
      },
      discoverListOrigin_);
}

jjwxc::Status JjwxcActivity::fetchDiscoverPage(const std::string& url, const bool append,
                                               const uint32_t requestedPage) {
  if (!ensureWapSession()) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  if (scratch_ == nullptr || scratchCap_ < kDiscoverPageRawCap + 64 * 1024) {
    LOG_ERR(kTag, "scratch too small for a discovery page (%u bytes)", (unsigned)scratchCap_);
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  wap_->setNowUnix(cookieClock());

  char absolute[320];
  if (url.compare(0, 4, "http") == 0) {
    snprintf(absolute, sizeof(absolute), "%s", url.c_str());
  } else if (!url.empty() && url[0] == '/') {
    snprintf(absolute, sizeof(absolute), "%s%s", jjwxc::kWapHost, url.c_str());
  } else {
    LOG_ERR(kTag, "discovery URL is neither absolute nor rooted: %.60s", url.c_str());
    return jjwxc::Status::Protocol;
  }

  auto* page = reinterpret_cast<char*>(scratch_);
  BufferCollector col{page, kDiscoverPageRawCap, 0, false};
  jjwxc::WapOptions opt;
  jjwxc::WapResponse res;
  const int http = wap_->get(absolute, opt, &collectBuffer, &col, res);
  if (http < 0) {
    jobDetailCode_ = http;
    LOG_ERR(kTag, "discovery request failed (%d) url=%.80s", http, absolute);
    return jjwxc::Status::Network;
  }
  if (res.sawRedirect) {
    // These are anonymous pages; a redirect is a shape this client has not seen,
    // not a dead session. Say so instead of claiming the shelf is empty.
    LOG_ERR(kTag, "discovery redirected to %.80s", res.location);
    return jjwxc::Status::Protocol;
  }
  if (http != 200) {
    jobDetailCode_ = http;
    LOG_ERR(kTag, "discovery page returned %d url=%.80s", http, absolute);
    return jjwxc::Status::Server;
  }
  if (col.overflow) {
    LOG_ERR(kTag, "discovery page exceeded %u bytes", (unsigned)kDiscoverPageRawCap);
    return jjwxc::Status::TooLarge;
  }
  if (col.len == 0) return jjwxc::Status::EmptyResult;
  page[col.len] = '\0';

  size_t textLen = col.len;
  const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
  if (enc == txt_encoding::Encoding::Gbk) {
    const auto conv = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
    if (conv.rawLength == 0 || conv.utf8Length == 0) {
      LOG_ERR(kTag, "discovery page would not transcode (%u raw bytes)", (unsigned)col.len);
      return jjwxc::Status::Protocol;
    }
    textLen = conv.utf8Length;
    page[textLen] = '\0';
  }

  const size_t before = discover_.size();
  jjwxc::DiscoverPage pager;
  const size_t got = jjwxc::parseDiscoverList(page, textLen, &onDiscoverEntry, this, &pager, requestedPage);
  discoverPage_ = pager.current;
  discoverMax_ = pager.max;
  discoverNextUrl_.assign(pager.nextUrl);
  LOG_INF(kTag, "discovery %s page %u: %u rows (%u new), pager max=%u next=%.60s (%u bytes, gbk=%d)", discoverTitle_.c_str(),
          (unsigned)pager.current, (unsigned)got, (unsigned)(discover_.size() - before), (unsigned)pager.max,
          pager.nextUrl, (unsigned)col.len, (int)(enc == txt_encoding::Encoding::Gbk));

  if (got == 0) {
    discoverNextUrl_.clear();
    if (!append) return jjwxc::Status::EmptyResult;
  }
  if (discover_.size() >= kMaxRows) discoverNextUrl_.clear();
  return jjwxc::Status::Ok;
}

bool JjwxcActivity::onDiscoverEntry(void* ctx, const jjwxc::DiscoverEntry& entry, size_t) {
  auto* self = static_cast<JjwxcActivity*>(ctx);
  if (self->jobCancel_.load()) return false;
  if (!self->jobReplacedTarget_) {
    self->discover_.clear();
    self->jobReplacedTarget_ = true;
  }
  for (const auto& existing : self->discover_) {
    if (strcmp(existing.novelId, entry.novelId) == 0) return true;
  }
  if (self->discover_.size() < kMaxRows) self->discover_.push_back(entry);
  return true;
}

// ---- advanced search -------------------------------------------------------

void JjwxcActivity::promptAdvKeyword() {
  const auto handler = [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    // Write the field BEFORE enter(): enter() calls rebuildRows(), and the row's
    // subtitle is composed from this field. The old order entered first and wrote
    // the value second, so the page kept showing the previous keyword until some
    // unrelated redraw happened -- the reported "typing does not update the row".
    if (!result.isCancelled) advKeyword_ = std::get<KeyboardResult>(result.data).text;
    enter(State::AdvancedSearch);
    // The mode row is gone; the keyword row is now index 0.
    selected_ = 0;
    ensureVisible();
    requestUpdate();
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_ADV_KEYWORD), advKeyword_, 24,
                                                      InputType::Text);
}

void JjwxcActivity::promptAdvInclude() {
  const auto handler = [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    // See promptAdvKeyword(): assign before enter() so the rebuilt row shows it.
    if (!result.isCancelled) advInclude_ = std::get<KeyboardResult>(result.data).text;
    enter(State::AdvancedSearch);
    selected_ = 1;
    ensureVisible();
    requestUpdate();
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_ADV_INCLUDE), advInclude_, 40,
                                                      InputType::Text);
}

void JjwxcActivity::promptAdvExclude() {
  const auto handler = [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    // See promptAdvKeyword(): assign before enter() so the rebuilt row shows it.
    if (!result.isCancelled) advExclude_ = std::get<KeyboardResult>(result.data).text;
    enter(State::AdvancedSearch);
    selected_ = 2;
    ensureVisible();
    requestUpdate();
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_ADV_EXCLUDE), advExclude_, 40,
                                                      InputType::Text);
}

void JjwxcActivity::cycleAdvFinish() {
  advFinish_ = (advFinish_ + 1) % 3;
  rebuildRows();
  // Row order (advanced mode): keyword 0, include 1, exclude 2, status 3, sort 4,
  // run 5. The mode row that used to shift all of these is gone.
  selected_ = 3;
  ensureVisible();
  requestUpdate();
}

void JjwxcActivity::cycleAdvSort() {
  advSort_ = (advSort_ + 1) % static_cast<int>(kAdvSortCount);
  rebuildRows();
  // See cycleAdvFinish() for the row order.
  selected_ = 4;
  ensureVisible();
  requestUpdate();
}

void JjwxcActivity::startAdvancedSearch() {
  if (searchMode_ != 2) {
    keyword_ = advKeyword_;
    searchType_ = searchMode_ == 1 ? 2 : 1;
    if (keyword_.empty()) {
      showMessage(tr(STR_JJWXC_ADV_KEYWORD), State::AdvancedSearch);
      return;
    }
    runSearch();
    return;
  }
  discoverTitle_ = tr(STR_JJWXC_ADV_SEARCH);
  discoverListOrigin_ = State::AdvancedSearch;
  jobTagMissing_.clear();
  withNetwork(
      [this] {
        jobWhat_ = tr(STR_JJWXC_ADV_SEARCH);
        jobNextState_ = State::DiscoverList;
        returnState_ = State::AdvancedSearch;
        jobAppendTarget_ = false;
        jobReplacedTarget_ = false;
        discoverNextUrl_.clear();
        launchJob(
            [this] {
              jobStatus_ = runAdvancedSearch();
            },
            tr(STR_JJWXC_LOADING));
      },
      State::AdvancedSearch);
}

jjwxc::Status JjwxcActivity::runAdvancedSearch() {
  if (!ensureWapSession()) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  if (scratch_ == nullptr || scratchCap_ < kDiscoverPageRawCap + 64 * 1024) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }

  TagResolveCtx tags;
  tags.include = advInclude_.c_str();
  tags.exclude = advExclude_.c_str();
  const uint8_t includeTokens = static_cast<uint8_t>(countTokens(advInclude_.c_str()));
  const uint8_t excludeTokens = static_cast<uint8_t>(countTokens(advExclude_.c_str()));

  if (includeTokens > 0 || excludeTokens > 0) {
    auto* page = reinterpret_cast<char*>(scratch_);
    BufferCollector col{page, kDiscoverPageRawCap, 0, false};
    jjwxc::WapOptions opt;
    jjwxc::WapResponse res;
    const int http = wap_->get("https://m.jjwxc.net/assort", opt, &collectBuffer, &col, res);
    if (http < 0) {
      jobDetailCode_ = http;
      LOG_ERR(kTag, "tag dictionary transport error %d", http);
      return jjwxc::Status::Network;
    }
    if (http != 200) {
      jobDetailCode_ = http;
      LOG_ERR(kTag, "tag dictionary returned %d", http);
      return jjwxc::Status::Server;
    }
    if (col.overflow || col.len == 0) return col.overflow ? jjwxc::Status::TooLarge : jjwxc::Status::EmptyResult;
    page[col.len] = '\0';
    size_t textLen = col.len;
    const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
    if (enc == txt_encoding::Encoding::Gbk) {
      const auto conv = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
      if (conv.rawLength == 0 || conv.utf8Length == 0) return jjwxc::Status::Protocol;
      textLen = conv.utf8Length;
      page[textLen] = '\0';
    }
    const size_t tagCount = jjwxc::parseAssortTags(page, textLen, resolveTagSink, &tags);
    LOG_INF(kTag, "assort tag dictionary: %u tags, include=%u/%u exclude=%u/%u", (unsigned)tagCount,
            (unsigned)__builtin_popcount(tags.includeFound), (unsigned)includeTokens,
            (unsigned)__builtin_popcount(tags.excludeFound), (unsigned)excludeTokens);
    if (includeTokens > 0) {
      const uint8_t want = static_cast<uint8_t>((1u << includeTokens) - 1u);
      if ((tags.includeFound & want) != want) {
        firstMissing(advInclude_.c_str(), tags.includeFound, includeTokens, tags.missing, sizeof(tags.missing));
        jobTagMissing_.assign(tags.missing);
        return jjwxc::Status::Protocol;
      }
    }
    if (excludeTokens > 0) {
      const uint8_t want = static_cast<uint8_t>((1u << excludeTokens) - 1u);
      if ((tags.excludeFound & want) != want) {
        firstMissing(advExclude_.c_str(), tags.excludeFound, excludeTokens, tags.missing, sizeof(tags.missing));
        jobTagMissing_.assign(tags.missing);
        return jjwxc::Status::Protocol;
      }
    }
  }

  char keyword[96];
  keyword[0] = '\0';
  if (!advKeyword_.empty() &&
      jjwxc::wapUrlEncode(advKeyword_.c_str(), advKeyword_.size(), keyword, sizeof(keyword)) == 0) {
    LOG_ERR(kTag, "advanced keyword would not fit the encoded cap");
    return jjwxc::Status::Protocol;
  }

  char url[320];
  const int n = snprintf(url, sizeof(url),
                         "%s/assort?searchkeywords=%s&bq=%s&removebq=%s&collectiontypes=ands&isfinish=%d"
                         "&sortType=%d&page=1",
                         jjwxc::kWapHost, keyword, tags.includeIds, tags.excludeIds, advFinish_,
                         kAdvSorts[advSort_].value);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return jjwxc::Status::TooLarge;
  discoverRequestUrl_.assign(url);
  return fetchDiscoverPage(discoverRequestUrl_, /*append=*/false, 1);
}

// ---- favourites ------------------------------------------------------------

void JjwxcActivity::loadFavoriteClasses(const State after) {
  if (!haveSession()) {
    showMessage(tr(STR_JJWXC_NEED_LOGIN), State::Menu);
    return;
  }
  const State cancelTo = after == State::FavoritePicker ? State::BookDetail : State::Menu;
  withNetwork(
      [this, after, cancelTo] {
        jobWhat_ = tr(STR_JJWXC_FAVORITES);
        jobNextState_ = after;
        returnState_ = cancelTo;
        jobReplacedTarget_ = false;
        launchJob(
            [this] {
              jobStatus_ = fetchFavoriteClasses();
            },
            tr(STR_JJWXC_LOADING));
      },
      cancelTo);
}

jjwxc::Status JjwxcActivity::fetchFavoriteClasses() {
  if (!ensureWapSession()) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  if (scratch_ == nullptr || scratchCap_ < kWapPageRawCap + 32 * 1024) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  wap_->setNowUnix(cookieClock());

  auto* page = reinterpret_cast<char*>(scratch_);
  char url[128];
  snprintf(url, sizeof(url), "%s/my/favlist", jjwxc::kWapHost);
  BufferCollector col{page, kWapPageRawCap, 0, false};
  jjwxc::WapOptions opt;
  jjwxc::WapResponse res;
  const int http = wap_->get(url, opt, &collectBuffer, &col, res);
  if (http < 0) {
    jobDetailCode_ = http;
    LOG_ERR(kTag, "favourite classes request failed (%d)", http);
    return jjwxc::Status::Network;
  }
  if (res.sawRedirect) {
    LOG_ERR(kTag, "favourite classes redirected to %.80s -- the session is dead", res.location);
    return jjwxc::Status::NeedLogin;
  }
  if (http != 200) {
    jobDetailCode_ = http;
    LOG_ERR(kTag, "favourite classes returned %d", http);
    return jjwxc::Status::Server;
  }
  if (col.overflow || col.len == 0) {
    return col.overflow ? jjwxc::Status::TooLarge : jjwxc::Status::EmptyResult;
  }
  page[col.len] = '\0';
  size_t textLen = col.len;
  const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
  if (enc == txt_encoding::Encoding::Gbk) {
    const auto conv = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
    if (conv.rawLength == 0 || conv.utf8Length == 0) return jjwxc::Status::Protocol;
    textLen = conv.utf8Length;
    page[textLen] = '\0';
  }
  const size_t count = jjwxc::parseFavoriteClasses(page, textLen, &onFavoriteClass, this);
  LOG_INF(kTag, "favourite classes: %u folders (%u bytes, gbk=%d)", (unsigned)count, (unsigned)col.len,
          (int)(enc == txt_encoding::Encoding::Gbk));
  return count == 0 ? jjwxc::Status::EmptyResult : jjwxc::Status::Ok;
}

bool JjwxcActivity::onFavoriteClass(void* ctx, const jjwxc::FavoriteClass& item, size_t) {
  auto* self = static_cast<JjwxcActivity*>(ctx);
  if (self->jobCancel_.load()) return false;
  if (!self->jobReplacedTarget_) {
    self->favClasses_.clear();
    self->jobReplacedTarget_ = true;
  }
  for (const auto& existing : self->favClasses_) {
    if (strcmp(existing.id, item.id) == 0) return true;
  }
  if (self->favClasses_.size() < 64) self->favClasses_.push_back(item);
  return true;
}

void JjwxcActivity::openFavoriteFolder(const std::string& classId) {
  if (classId == "__all") {
    shelfClassId_.clear();
    shelfTitle_.clear();
    loadShelf();
    return;
  }
  for (const auto& c : favClasses_) {
    if (classId == c.id) {
      shelfClassId_ = c.id;
      shelfTitle_ = c.name;
      loadShelf();
      return;
    }
  }
}

void JjwxcActivity::promptFavoritePick() {
  if (!haveSession()) {
    showMessage(tr(STR_JJWXC_NEED_LOGIN), State::BookDetail);
    return;
  }
  if (favClasses_.empty()) {
    loadFavoriteClasses(State::FavoritePicker);
    return;
  }
  enter(State::FavoritePicker);
}

void JjwxcActivity::addToFavoriteClass(const std::string& classId, const std::string& className) {
  const bool preferMove = favoriteKnown_ && isFavorite_;
  jobFavoriteClassName_ = className;
  char notice[128];
  snprintf(notice, sizeof(notice), tr(STR_JJWXC_FAV_ADDED_FMT), className.c_str());
  withNetwork(
      [this, classId, preferMove, notice = std::string(notice)] {
        jobWhat_ = tr(STR_JJWXC_FAVORITES);
        jobNextState_ = State::BookDetail;
        returnState_ = State::BookDetail;
        jobNotice_ = notice;
        jobFavoriteAfter_ = 1;
        launchJob(
            [this, classId, preferMove] {
              jobStatus_ = runFavoriteSetClass(classId, preferMove);
            },
            tr(STR_JJWXC_LOADING));
      },
      State::BookDetail);
}

void JjwxcActivity::promptRemoveFavorite() {
  if (!favoriteKnown_ || !isFavorite_) return;
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_JJWXC_FAV_REMOVE_CONFIRM),
                                              novel_.title[0] ? novel_.title : novelId_.c_str());
  if (!confirmation) {
    LOG_ERR(kTag, "OOM: favourite removal confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    char body[128];
    if (jjwxc::buildFavoriteDeleteBody(novel_.novelId, body, sizeof(body)) == 0) {
      showMessage(tr(STR_JJWXC_ERR_PROTOCOL), State::BookDetail);
      return;
    }
    jobFavoriteAfter_ = 0;
    jobFavoriteClassName_.clear();
    launchFavoriteMutation("/app.jjwxc/Pc/Favorite/deleteNovelFavorite", body, tr(STR_JJWXC_FAV_REMOVED),
                           State::BookDetail, /*reloadClasses=*/false);
  });
}

void JjwxcActivity::showFavoriteClassActions() {
  if (selected_ < 0 || static_cast<size_t>(selected_) >= rowIds_.size()) return;
  const std::string id(rowIds_[static_cast<size_t>(selected_)].c_str());
  std::string name;
  for (const auto& c : favClasses_) {
    if (id == c.id) name = c.name;
  }
  if (name.empty()) return;
  const char* options[] = {tr(STR_JJWXC_FAV_RENAME), tr(STR_JJWXC_FAV_DELETE)};
  optionPopup_.show(tr(STR_JJWXC_FAV_MANAGE), name.c_str(), options, 2, 0, [this, id, name](const int index) {
    if (index == 0) promptRenameFavoriteClass(id, name);
    else if (index == 1) promptDeleteFavoriteClass(id, name);
  });
  requestUpdate();
}

void JjwxcActivity::promptNewFavoriteClass() {
  const auto handler = [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      enter(State::Favorites);
      return;
    }
    const std::string name = std::get<KeyboardResult>(result.data).text;
    if (name.empty()) {
      enter(State::Favorites);
      return;
    }
    char body[256];
    if (jjwxc::buildFavoriteClassCreateBody(name.c_str(), body, sizeof(body)) == 0) {
      showMessage(tr(STR_JJWXC_ERR_PROTOCOL), State::Favorites);
      return;
    }
    char notice[128];
    snprintf(notice, sizeof(notice), tr(STR_JJWXC_FAV_CREATED_FMT), name.c_str());
    launchFavoriteMutation("/app.jjwxc/Wap/FavoriteClass/addFavoriteClassname", body, notice, State::Favorites,
                           /*reloadClasses=*/true);
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_FAV_NAME), "", 16, InputType::Text);
}

void JjwxcActivity::promptRenameFavoriteClass(const std::string& classId, const std::string& name) {
  const auto handler = [this, classId](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      enter(State::FavoriteManage);
      return;
    }
    const std::string newName = std::get<KeyboardResult>(result.data).text;
    if (newName.empty()) {
      enter(State::FavoriteManage);
      return;
    }
    char body[256];
    if (jjwxc::buildFavoriteClassRenameBody(classId.c_str(), newName.c_str(), body, sizeof(body)) == 0) {
      showMessage(tr(STR_JJWXC_ERR_PROTOCOL), State::FavoriteManage);
      return;
    }
    launchFavoriteMutation("/app.jjwxc/Wap/FavoriteClass/editFavoriteClassname", body, tr(STR_JJWXC_FAV_RENAMED),
                           State::FavoriteManage, /*reloadClasses=*/true);
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_FAV_NAME), name, 16, InputType::Text);
}

void JjwxcActivity::promptDeleteFavoriteClass(const std::string& classId, const std::string& name) {
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_JJWXC_FAV_DELETE_CONFIRM), name.c_str());
  if (!confirmation) {
    LOG_ERR(kTag, "OOM: folder delete confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this, classId](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    char body[128];
    if (jjwxc::buildFavoriteClassDeleteBody(classId.c_str(), body, sizeof(body)) == 0) {
      showMessage(tr(STR_JJWXC_ERR_PROTOCOL), State::FavoriteManage);
      return;
    }
    launchFavoriteMutation("/app.jjwxc/Wap/FavoriteClass/delByClassid", body, tr(STR_JJWXC_FAV_DELETED),
                           State::FavoriteManage, /*reloadClasses=*/true);
  });
}

jjwxc::Status JjwxcActivity::fetchFavoriteStatus() {
  if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < 32 * 1024) return jjwxc::Status::Ok;
  wap_->setNowUnix(cookieClock());
  char body[64];
  if (jjwxc::buildFavoriteStatusBody(novelId_.c_str(), body, sizeof(body)) == 0) return jjwxc::Status::Ok;
  char url[160];
  snprintf(url, sizeof(url), "%s/app.jjwxc/Wap/Favorite/favoriteStatus", jjwxc::kWapHost);
  auto* page = reinterpret_cast<char*>(scratch_);
  BufferCollector col{page, 32 * 1024, 0, false};
  jjwxc::WapOptions opt;
  opt.contentType = jjwxc::kWapFormContentType;
  opt.referer = "https://m.jjwxc.net/book2/";
  jjwxc::WapResponse res;
  const int http = wap_->post(url, body, strlen(body), opt, &collectBuffer, &col, res);
  if (http < 0 || res.sawRedirect || http != 200 || col.overflow || col.len == 0) {
    LOG_INF(kTag, "favourite status unknown (http=%d redirect=%d)", http, (int)res.sawRedirect);
    return jjwxc::Status::Ok;
  }
  page[col.len] = '\0';
  jjwxc::FavoriteStatusInfo info;
  if (jjwxc::parseFavoriteStatusResponse(page, col.len, info) && info.known) {
    favoriteKnown_ = true;
    isFavorite_ = info.isFavorite;
    favoriteClassId_ = info.classId;
    favoriteClassName_ = info.className;
    LOG_INF(kTag, "favourite status: %s class=%s", info.isFavorite ? "favourited" : "not favourited",
            info.classId);
  } else {
    LOG_INF(kTag, "favourite status response not recognised (%u bytes)", (unsigned)col.len);
  }
  return jjwxc::Status::Ok;
}

jjwxc::Status JjwxcActivity::runFavoriteSetClass(const std::string& classId, const bool preferMove) {
  char body[256];
  const bool firstMove =
      preferMove && jjwxc::buildFavoriteMoveBody(novelId_.c_str(), classId.c_str(), body, sizeof(body)) > 0;
  if (!firstMove && jjwxc::buildFavoriteAddBody(novelId_.c_str(), classId.c_str(), body, sizeof(body)) == 0) {
    return jjwxc::Status::Protocol;
  }
  jjwxc::Status st = postFavorite(firstMove ? "/app.jjwxc/Wap/Favorite/editNovelFavoriteClass"
                                            : "/app.jjwxc/Wap/Favorite/addNovelFavorite",
                                  body);
  if (!firstMove && st != jjwxc::Status::Ok && jobDetailCode_ == 1059) {
    // Already favourited: the site's add endpoint refuses it with 1059. Re-file it
    // with the class editor instead of surfacing a bogus "article does not exist".
    char moveBody[256];
    if (jjwxc::buildFavoriteMoveBody(novelId_.c_str(), classId.c_str(), moveBody, sizeof(moveBody)) == 0) {
      return jjwxc::Status::Protocol;
    }
    LOG_INF(kTag, "add returned 1059; retrying as a class change");
    jobDetailCode_ = 0;
    jobServerMessage_[0] = '\0';
    st = postFavorite("/app.jjwxc/Wap/Favorite/editNovelFavoriteClass", moveBody);
  }
  return st;
}

jjwxc::Status JjwxcActivity::postFavorite(const char* path, const std::string& body) {
  if (!haveSession()) return jjwxc::Status::NeedLogin;
  if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < 32 * 1024) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  if (body.empty()) return jjwxc::Status::Protocol;
  wap_->setNowUnix(cookieClock());
  char url[192];
  snprintf(url, sizeof(url), "%s%s", jjwxc::kWapHost, path);
  auto* page = reinterpret_cast<char*>(scratch_);
  BufferCollector col{page, 32 * 1024, 0, false};
  jjwxc::WapOptions opt;
  opt.contentType = jjwxc::kWapFormContentType;
  opt.referer = "https://m.jjwxc.net/my/favlist";
  jjwxc::WapResponse res;
  const int http = wap_->post(url, body.data(), body.size(), opt, &collectBuffer, &col, res);
  if (http < 0) {
    jobDetailCode_ = http;
    LOG_ERR(kTag, "favourite mutation transport error %d", http);
    return jjwxc::Status::Network;
  }
  if (res.sawRedirect) {
    LOG_ERR(kTag, "favourite mutation redirected -- the session is dead");
    return jjwxc::Status::NeedLogin;
  }
  if (http != 200) {
    jobDetailCode_ = http;
    LOG_ERR(kTag, "favourite mutation returned %d", http);
    return jjwxc::Status::Server;
  }
  if (col.overflow || col.len == 0) {
    return col.overflow ? jjwxc::Status::TooLarge : jjwxc::Status::Protocol;
  }
  page[col.len] = '\0';
  jjwxc::FavoriteResult parsed;
  if (!jjwxc::parseFavoriteResult(page, col.len, parsed)) {
    LOG_ERR(kTag, "favourite mutation answer did not parse (%u bytes)", (unsigned)col.len);
    return jjwxc::Status::Protocol;
  }
  if (!parsed.ok) {
    // The endpoint always answers HTTP 200; the verdict is in the JSON.
    jobDetailCode_ = parsed.code;
    snprintf(jobServerMessage_, sizeof(jobServerMessage_), "%s", parsed.message);
    LOG_ERR(kTag, "favourite mutation refused (code=%d, message='%s')", parsed.code, parsed.message);
    return jjwxc::Status::Server;
  }
  return jjwxc::Status::Ok;
}

void JjwxcActivity::launchFavoriteMutation(const char* path, const std::string& body, const std::string& notice,
                                           const State after, const bool reloadClasses) {
  if (!haveSession()) {
    showMessage(tr(STR_JJWXC_NEED_LOGIN), after);
    return;
  }
  withNetwork(
      [this, path = std::string(path), body, notice, after, reloadClasses] {
        jobWhat_ = tr(STR_JJWXC_FAVORITES);
        jobNextState_ = after;
        returnState_ = after;
        jobNotice_ = notice;
        jobReloadFavorites_ = reloadClasses;
        jobReplacedTarget_ = false;
        launchJob(
            [this, path, body, reloadClasses] {
              jobStatus_ = postFavorite(path.c_str(), body);
              if (jobStatus_ == jjwxc::Status::Ok && reloadClasses) {
                jobStatus_ = fetchFavoriteClasses();
              }
            },
            tr(STR_JJWXC_LOADING));
      },
      after);
}

// ---- paragraph comment totals (reader sidecar) ------------------------------

namespace {
struct CommentCountCtx {
  JjwxcCommentSidecar::Count* out = nullptr;
  size_t cap = 0;
  size_t count = 0;
};

struct CommentBodyCtx {
  JjwxcCommentSidecar::Bodies::Item* out = nullptr;
  size_t cap = 0;
  size_t count = 0;
};
}  // namespace

bool JjwxcActivity::onCommentCount(void* ctx, const jjwxc::ParagraphComment& item, size_t) {
  auto* c = static_cast<CommentCountCtx*>(ctx);
  if (c->out == nullptr || c->count >= c->cap) return false;
  const unsigned long id = strtoul(item.paragraphId, nullptr, 10);
  if (id == 0 || item.total == 0) return true;
  c->out[c->count].paragraphId = static_cast<uint32_t>(id);
  c->out[c->count].total = static_cast<uint16_t>(item.total > 65535 ? 65535 : item.total);
  ++c->count;
  return true;
}

void JjwxcActivity::fetchAndStoreCommentCounts(const uint32_t chapterId, const bool includeBodies) {
  if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < 64 * 1024) return;
  char sid[256];
  char token[400];
  token[0] = '\0';
  if (jar_->get("sid", sid, sizeof(sid), cookieClock()) != 0) {
    char b64[400];
    if (jjwxc::Base64::encode(reinterpret_cast<const uint8_t*>(sid), strlen(sid), b64, sizeof(b64)) > 0 &&
        jjwxc::wapUrlEncode(b64, strlen(b64), token, sizeof(token)) == 0) {
      token[0] = '\0';
    }
  }
  char url[768];
  const int n = snprintf(url, sizeof(url),
                         "https://www.jjwxc.net/app.jjwxc/Pc/comment/getNovelParagraphCommentNum"
                         "?versionCode=489&novelid=%s&chapterid=%lu%s%s",
                         novelId_.c_str(), static_cast<unsigned long>(chapterId), token[0] ? "&token=" : "", token);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return;
  auto* page = reinterpret_cast<char*>(scratch_);
  BufferCollector col{page, 64 * 1024, 0, false};
  jjwxc::WapOptions opt;
  opt.referer = "https://m.jjwxc.net/book2/";
  jjwxc::WapResponse res;
  const int http = wap_->get(url, opt, &collectBuffer, &col, res);
  if (http != 200 || res.sawRedirect || col.overflow || col.len == 0) {
    LOG_INF(kTag, "comment counts unavailable for chapter %lu (http=%d)", (unsigned long)chapterId, http);
    return;
  }
  page[col.len] = '\0';
  // 128 entries is far past any real chapter's commented-paragraph count, and
  // keeps the worker stack free for the TLS handshake that follows.
  constexpr size_t kMaxChapterCounts = 128;
  JjwxcCommentSidecar::Count counts[kMaxChapterCounts];
  CommentCountCtx ctx{counts, kMaxChapterCounts, 0};
  jjwxc::parseParagraphComments(page, col.len, &JjwxcActivity::onCommentCount, &ctx);
  // Drop the old sidecar first: after a repackage the old paragraph ids may no
  // longer match this body, and a stale bubble is worse than no bubble.
  const std::string path = JjwxcStore::commentCountsPath(novelId_.c_str(), chapterId);
  Storage.remove(path.c_str());
  // An empty table is still written. Without it a chapter with no comments is
  // indistinguishable from one never checked, and the whole-book comment pass
  // would re-request its counts on every run.
  if (JjwxcStore::saveCommentCounts(novelId_.c_str(), chapterId, counts, ctx.count)) {
    LOG_INF(kTag, "comment counts: chapter %lu %u paragraphs", (unsigned long)chapterId, (unsigned)ctx.count);
  } else {
    LOG_ERR(kTag, "comment sidecar write failed for chapter %lu", (unsigned long)chapterId);
  }
  if (includeBodies && ctx.count > 0) {
    prefetchCommentBodies(chapterId, counts, ctx.count, millis() + 75000, static_cast<size_t>(-1));
  }
}

bool JjwxcActivity::onPrefetchComment(void* ctx, const jjwxc::CommentItem& item, size_t) {
  auto* c = static_cast<CommentBodyCtx*>(ctx);
  if (c->out == nullptr || c->count >= c->cap) return false;
  JjwxcCommentSidecar::Bodies::Item& out = c->out[c->count];
  snprintf(out.author, sizeof(out.author), "%s", item.author);
  snprintf(out.date, sizeof(out.date), "%s", item.date);
  snprintf(out.ip, sizeof(out.ip), "%s", item.ip);
  snprintf(out.body, sizeof(out.body), "%s", item.body);
  snprintf(out.commentId, sizeof(out.commentId), "%s", item.commentId);
  out.agreeNum = item.agreeNum;
  out.disagreeNum = item.disagreeNum;
  out.replyTotal = item.replyTotal;
  out.readerAgree = item.readerAgree;
  out.isAuthor = item.isAuthor;
  ++c->count;
  return true;
}

// The first page of each commented paragraph is enough for offline reading;
// "more comments" stays an explicit network action. A single-chapter run passes
// a 75 s deadline; the whole-book pass passes its own longer deadline and
// request budget, so this never runs past the caller's commitment.
JjwxcActivity::CommentPrefetchResult JjwxcActivity::prefetchCommentBodies(
    const uint32_t chapterId, const JjwxcCommentSidecar::Count* counts, const size_t count, const uint32_t deadlineMs,
    const size_t requestBudget) {
  CommentPrefetchResult result;
  if (counts == nullptr || count == 0) return result;
  if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < 128 * 1024) return result;
  constexpr size_t kPageSize = 20;
  auto itemsBuf = memory::makePsramByteBufferNoThrow(kPageSize * sizeof(JjwxcCommentSidecar::Bodies::Item));
  if (!itemsBuf) {
    LOG_ERR(kTag, "no PSRAM for comment prefetch");
    return result;
  }
  auto* items = reinterpret_cast<JjwxcCommentSidecar::Bodies::Item*>(itemsBuf.get());

  char sid[256];
  char token[400];
  token[0] = '\0';
  if (jar_->get("sid", sid, sizeof(sid), cookieClock()) != 0) {
    char b64[400];
    if (jjwxc::Base64::encode(reinterpret_cast<const uint8_t*>(sid), strlen(sid), b64, sizeof(b64)) > 0 &&
        jjwxc::wapUrlEncode(b64, strlen(b64), token, sizeof(token)) == 0) {
      token[0] = '\0';
    }
  }

  for (size_t i = 0; i < count; ++i) {
    if (jobCancel_.load()) break;
    const uint32_t paragraphId = counts[i].paragraphId;
    if (paragraphId == 0 || counts[i].total == 0) continue;
    const std::string bodyPath = JjwxcStore::commentBodiesPath(novelId_.c_str(), chapterId, paragraphId);
    if (Storage.exists(bodyPath.c_str())) {
      ++result.skipped;
      continue;
    }
    if (static_cast<int32_t>(millis() - deadlineMs) >= 0 || result.requests >= requestBudget) {
      LOG_INF(kTag, "comment prefetch budget reached (%u/%u)", (unsigned)i, (unsigned)count);
      result.budgetReached = true;
      break;
    }

    char url[768];
    const int n = snprintf(url, sizeof(url),
                           "https://www.jjwxc.net/app.jjwxc/Pc/comment/getCommentList"
                           "?versionCode=489&novelId=%s&chapterId=%lu&paragraph_id=%lu&offset=0&limit=%u%s%s",
                           novelId_.c_str(), static_cast<unsigned long>(chapterId),
                           static_cast<unsigned long>(paragraphId), static_cast<unsigned>(kPageSize),
                           token[0] ? "&token=" : "", token);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) {
      ++result.failed;
      continue;
    }
    ++result.requests;
    auto* page = reinterpret_cast<char*>(scratch_);
    BufferCollector col{page, 64 * 1024, 0, false};
    jjwxc::WapOptions opt;
    opt.referer = "https://m.jjwxc.net/book2/";
    jjwxc::WapResponse res;
    const int http = wap_->get(url, opt, &collectBuffer, &col, res);
    if (http != 200 || res.sawRedirect || col.overflow || col.len == 0) {
      ++result.failed;
      LOG_INF(kTag, "comment prefetch: para=%lu http=%d", (unsigned long)paragraphId, http);
      delay(250);
      continue;
    }
    page[col.len] = '\0';
    size_t textLen = col.len;
    const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
    if (enc == txt_encoding::Encoding::Gbk) {
      const auto conv = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
      if (conv.rawLength == 0 || conv.utf8Length == 0) {
        ++result.failed;
        continue;
      }
      textLen = conv.utf8Length;
      page[textLen] = '\0';
    }

    CommentBodyCtx ctx{items, kPageSize, 0};
    jjwxc::parseCommentList(page, textLen, &JjwxcActivity::onPrefetchComment, &ctx);
    uint32_t total = 0;
    const char* totalKey = strstr(page, "\"commentTotal\"");
    if (totalKey != nullptr) {
      const char* p = strchr(totalKey, ':');
      if (p != nullptr) {
        ++p;
        while (*p == ' ' || *p == '"') ++p;
        while (*p >= '0' && *p <= '9') total = total * 10 + static_cast<uint32_t>(*p++ - '0');
      }
    }
    if (ctx.count > 0 &&
        JjwxcStore::saveCommentBodies(novelId_.c_str(), chapterId, paragraphId, items, ctx.count,
                                      total > ctx.count ? total : static_cast<uint32_t>(ctx.count))) {
      ++result.fetched;
    } else {
      ++result.failed;
    }
    delay(300);
  }
  LOG_INF(kTag, "comment prefetch: chapter %lu fetched=%u skipped=%u failed=%u requests=%u",
          (unsigned long)chapterId, static_cast<unsigned>(result.fetched), static_cast<unsigned>(result.skipped),
          static_cast<unsigned>(result.failed), static_cast<unsigned>(result.requests));
  return result;
}

// Whole-book pass. This is deliberately an explicit action rather than part of
// "cache whole book": a long novel has thousands of commented paragraphs, and
// making that many requests silently would turn an 18-minute cache into hours.
// The pass is resumable because every saved JJD1 is skipped.
void JjwxcActivity::prefetchAllCommentBodies() {
  if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < 128 * 1024) {
    jobStatus_ = jjwxc::Status::NeedLogin;
    return;
  }
  constexpr uint32_t kRunBudgetMs = 15 * 60 * 1000;
  constexpr size_t kMaxRunRequests = 600;
  const uint32_t deadline = millis() + kRunBudgetMs;
  JjwxcStore::TocReader toc;
  if (!toc.open(novelId_.c_str())) {
    jobNotice_ = tr(STR_JJWXC_NO_CATALOGUE);
    return;
  }

  // The reader stores up to kMaxEntries and loadCommentCounts reports the file's
  // full count even when maxOut is smaller, so this buffer must be able to hold
  // every entry it is told about.
  constexpr size_t kMaxChapterCounts = JjwxcCommentSidecar::kMaxEntries;
  JjwxcCommentSidecar::Count counts[kMaxChapterCounts];
  size_t chapters = 0;
  size_t bodiesFetched = 0;
  size_t bodiesSkipped = 0;
  size_t failures = 0;
  size_t requests = 0;
  bool budgetReached = false;
  for (uint32_t i = 0; i < toc.count(); ++i) {
    if (jobCancel_.load()) break;
    JjwxcStore::TocRecord rec;
    if (!toc.read(i, rec)) break;
    if (!JjwxcStore::isReadable(rec.state)) continue;
    ++chapters;

    size_t count = 0;
    if (!JjwxcStore::loadCommentCounts(novelId_.c_str(), rec.chapterId, counts, kMaxChapterCounts, count)) {
      if (static_cast<int32_t>(millis() - deadline) >= 0 || requests >= kMaxRunRequests) {
        budgetReached = true;
        break;
      }
      fetchAndStoreCommentCounts(rec.chapterId, /*includeBodies=*/false);
      ++requests;
      // A chapter with no comments now has an empty JJC1 file, so it is not
      // re-requested next time. A failed request leaves no file and is retried.
      if (!JjwxcStore::loadCommentCounts(novelId_.c_str(), rec.chapterId, counts, kMaxChapterCounts, count)) {
        ++failures;
        continue;
      }
    }
    if (count == 0) continue;
    const CommentPrefetchResult r =
        prefetchCommentBodies(rec.chapterId, counts, count, deadline, kMaxRunRequests - requests);
    requests += r.requests;
    bodiesFetched += r.fetched;
    bodiesSkipped += r.skipped;
    failures += r.failed;
    if (r.budgetReached) {
      budgetReached = true;
      break;
    }
  }
  toc.close();

  char notice[192];
  snprintf(notice, sizeof(notice), tr(STR_JJWXC_COMMENT_PREFETCH_DONE_FMT), static_cast<unsigned long>(bodiesFetched),
           static_cast<unsigned long>(bodiesSkipped), static_cast<unsigned long>(failures),
           static_cast<unsigned long>(requests));
  jobNotice_ = notice;
  if (budgetReached) {
    jobNotice_ += ' ';
    jobNotice_ += tr(STR_JJWXC_COMMENT_PREFETCH_AGAIN);
  }
  LOG_INF(kTag, "comment prefetch whole: chapters=%u bodies=%u skipped=%u failed=%u requests=%u budget=%d",
          static_cast<unsigned>(chapters), static_cast<unsigned>(bodiesFetched),
          static_cast<unsigned>(bodiesSkipped), static_cast<unsigned>(failures), static_cast<unsigned>(requests),
          budgetReached ? 1 : 0);
}

// ---- paragraph comments ----------------------------------------------------

bool JjwxcActivity::commentRequestAllowed(const State returnTo) {
  const uint32_t now = millis();
  if (lastCommentRequestMs_ != 0 && static_cast<uint32_t>(now - lastCommentRequestMs_) < 1500) {
    showMessage(tr(STR_JJWXC_COMMENTS_WAIT), returnTo);
    return false;
  }
  lastCommentRequestMs_ = now;
  return true;
}

void JjwxcActivity::promptComments(const uint32_t ordinal) {
  if (ordinal >= chapters_.size()) return;
  if (!commentRequestAllowed(State::ChapterList)) return;
  commentsOrdinal_ = ordinal;
  commentsParagraphId_ = 0;
  withNetwork(
      [this, ordinal] {
        jobWhat_ = tr(STR_JJWXC_COMMENTS);
        jobNextState_ = State::ParagraphComments;
        returnState_ = State::ChapterList;
        jobReplacedTarget_ = false;
        launchJob(
            [this, ordinal] {
              jobStatus_ = fetchParagraphComments(ordinal);
            },
            tr(STR_JJWXC_LOADING));
      },
      State::ChapterList);
}

jjwxc::Status JjwxcActivity::fetchParagraphComments(const uint32_t ordinal) {
  if (ordinal >= chapters_.size()) return jjwxc::Status::Protocol;
  if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < 64 * 1024) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  char sid[256];
  char token[400];
  token[0] = '\0';
  if (haveSession() && jar_->get("sid", sid, sizeof(sid), cookieClock()) != 0) {
    char b64[400];
    if (jjwxc::Base64::encode(reinterpret_cast<const uint8_t*>(sid), strlen(sid), b64, sizeof(b64)) > 0 &&
        jjwxc::wapUrlEncode(b64, strlen(b64), token, sizeof(token)) == 0) {
      token[0] = '\0';
    }
  }
  char url[768];
  const int n = snprintf(url, sizeof(url),
                         "https://www.jjwxc.net/app.jjwxc/Pc/comment/getNovelParagraphCommentNum"
                         "?versionCode=489&novelid=%s&chapterid=%lu%s%s",
                         novelId_.c_str(), static_cast<unsigned long>(chapters_[ordinal].chapterId),
                         token[0] ? "&token=" : "", token);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return jjwxc::Status::TooLarge;
  auto* page = reinterpret_cast<char*>(scratch_);
  BufferCollector col{page, 64 * 1024, 0, false};
  jjwxc::WapOptions opt;
  opt.referer = "https://m.jjwxc.net/book2/";
  jjwxc::WapResponse res;
  const int http = wap_->get(url, opt, &collectBuffer, &col, res);
  if (http < 0) {
    jobDetailCode_ = http;
    return jjwxc::Status::Network;
  }
  if (http != 200) {
    jobDetailCode_ = http;
    return jjwxc::Status::Server;
  }
  if (col.overflow || col.len == 0) return col.overflow ? jjwxc::Status::TooLarge : jjwxc::Status::EmptyResult;
  page[col.len] = '\0';
  size_t textLen = col.len;
  const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
  if (enc == txt_encoding::Encoding::Gbk) {
    const auto conv = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
    if (conv.rawLength == 0 || conv.utf8Length == 0) return jjwxc::Status::Protocol;
    textLen = conv.utf8Length;
    page[textLen] = '\0';
  }
  const size_t count = jjwxc::parseParagraphComments(page, textLen, &onParagraphComment, this);
  LOG_INF(kTag, "paragraph comments: chapter=%lu %u paragraphs", (unsigned long)chapters_[ordinal].chapterId,
          (unsigned)count);
  return count == 0 ? jjwxc::Status::EmptyResult : jjwxc::Status::Ok;
}

bool JjwxcActivity::onParagraphComment(void* ctx, const jjwxc::ParagraphComment& item, size_t) {
  auto* self = static_cast<JjwxcActivity*>(ctx);
  if (self->jobCancel_.load()) return false;
  if (!self->jobReplacedTarget_) {
    self->paragraphs_.clear();
    self->jobReplacedTarget_ = true;
  }
  if (self->paragraphs_.size() < kMaxRows) self->paragraphs_.push_back(item);
  return true;
}

void JjwxcActivity::loadMoreComments() {
  if (!commentRequestAllowed(State::CommentList)) return;
  withNetwork(
      [this] {
        jobWhat_ = tr(STR_JJWXC_COMMENTS);
        jobNextState_ = State::CommentList;
        returnState_ = State::CommentList;
        launchJob(
            [this] {
              jobStatus_ = fetchCommentList(commentsOrdinal_, commentsParagraphId_,
                                            static_cast<uint32_t>(comments_.size()), /*append=*/true);
            },
            tr(STR_JJWXC_LOADING));
      },
      State::CommentList);
}

void JjwxcActivity::openCommentList(const uint32_t paragraphId) {
  if (commentsOrdinal_ >= chapters_.size()) return;
  if (!commentRequestAllowed(State::ParagraphComments)) return;
  commentsParagraphId_ = paragraphId;
  commentOffset_ = 0;
  // The reader and the app now share one panel: it owns the reply view, the
  // like/reply chips and the composer, and the app keeps this paragraph list
  // underneath it. That replaced the old app-side CommentList job, which had no
  // way to act on a comment and would have been a second UI to keep in sync.
  const bool started = startActivityForResultWith<JjwxcCommentActivity>(
      [this](const ActivityResult&) {
        waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
        waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        requestUpdate();
      },
      novelId_, chapters_[commentsOrdinal_].chapterId, paragraphId,
      static_cast<uint16_t>(paragraphId > 0xFFFF ? 0xFFFF : paragraphId));
  if (!started) {
    LOG_ERR(kTag, "OOM: comment activity");
    showMessage(tr(STR_JJWXC_ERR_MEMORY), State::ParagraphComments);
  }
}

jjwxc::Status JjwxcActivity::fetchCommentList(const uint32_t ordinal, const uint32_t paragraphId,
                                              const uint32_t offset, const bool append) {
  if (ordinal >= chapters_.size()) return jjwxc::Status::Protocol;
  if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < 64 * 1024) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  char sid[256];
  char token[400];
  token[0] = '\0';
  if (haveSession() && jar_->get("sid", sid, sizeof(sid), cookieClock()) != 0) {
    char b64[400];
    if (jjwxc::Base64::encode(reinterpret_cast<const uint8_t*>(sid), strlen(sid), b64, sizeof(b64)) > 0 &&
        jjwxc::wapUrlEncode(b64, strlen(b64), token, sizeof(token)) == 0) {
      token[0] = '\0';
    }
  }
  char url[768];
  const int n = snprintf(url, sizeof(url),
                         "https://www.jjwxc.net/app.jjwxc/Pc/comment/getCommentList"
                         "?versionCode=489&novelId=%s&chapterId=%lu&paragraph_id=%lu&offset=%lu&limit=20%s%s",
                         novelId_.c_str(), static_cast<unsigned long>(chapters_[ordinal].chapterId),
                         static_cast<unsigned long>(paragraphId), static_cast<unsigned long>(offset),
                         token[0] ? "&token=" : "", token);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return jjwxc::Status::TooLarge;
  auto* page = reinterpret_cast<char*>(scratch_);
  BufferCollector col{page, 64 * 1024, 0, false};
  jjwxc::WapOptions opt;
  opt.referer = "https://m.jjwxc.net/book2/";
  jjwxc::WapResponse res;
  const int http = wap_->get(url, opt, &collectBuffer, &col, res);
  if (http < 0) {
    jobDetailCode_ = http;
    return jjwxc::Status::Network;
  }
  if (http != 200) {
    jobDetailCode_ = http;
    return jjwxc::Status::Server;
  }
  if (col.overflow || col.len == 0) return col.overflow ? jjwxc::Status::TooLarge : jjwxc::Status::EmptyResult;
  page[col.len] = '\0';
  size_t textLen = col.len;
  const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
  if (enc == txt_encoding::Encoding::Gbk) {
    const auto conv = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
    if (conv.rawLength == 0 || conv.utf8Length == 0) return jjwxc::Status::Protocol;
    textLen = conv.utf8Length;
    page[textLen] = '\0';
  }
  if (!append) {
    comments_.clear();
  }
  const size_t count = jjwxc::parseCommentList(page, textLen, &onCommentItem, this);
  commentTotal_ = static_cast<uint32_t>(comments_.size());
  {
    // commentTotal lives outside the array; read it with the same tolerant
    // scanner by asking the module's parser through a one-item probe.
    const char* totalKey = strstr(page, "\"commentTotal\"");
    if (totalKey != nullptr) {
      const char* p = strchr(totalKey, ':');
      if (p != nullptr) {
        ++p;
        while (*p == ' ' || *p == '"') ++p;
        uint32_t total = 0;
        while (*p >= '0' && *p <= '9') total = total * 10 + static_cast<uint32_t>(*p++ - '0');
        if (total > 0) commentTotal_ = total;
      }
    }
  }
  if (count == 0 && !append) return jjwxc::Status::EmptyResult;
  LOG_INF(kTag, "comment list: paragraph=%lu %u shown / %u total", (unsigned long)paragraphId, (unsigned)comments_.size(),
          (unsigned)commentTotal_);
  return jjwxc::Status::Ok;
}

bool JjwxcActivity::onCommentItem(void* ctx, const jjwxc::CommentItem& item, size_t) {
  auto* self = static_cast<JjwxcActivity*>(ctx);
  if (self->jobCancel_.load()) return false;
  if (self->comments_.size() < kMaxRows) self->comments_.push_back(item);
  return true;
}

// ---- purchase --------------------------------------------------------------

void JjwxcActivity::promptPurchase(const uint32_t ordinal) {
  if (!haveSession()) {
    showMessage(tr(STR_JJWXC_NEED_LOGIN), State::ChapterList);
    return;
  }
  if (ordinal >= chapters_.size()) return;
  pendingPurchaseOrdinal_ = static_cast<int>(ordinal);
  pendingPrice_ = 0;
  withNetwork(
      [this] {
        jobWhat_ = tr(STR_JJWXC_BUY_CHAPTER);
        jobNextState_ = State::ChapterList;
        returnState_ = State::ChapterList;
        jobPurchaseConfirmPending_ = false;
        jobPurchaseAlready_ = false;
        launchJob(
            [this] {
              jobStatus_ = fetchChapterPrice(static_cast<uint32_t>(pendingPurchaseOrdinal_));
              if (jobStatus_ == jjwxc::Status::Ok && !jobPurchaseAlready_) jobPurchaseConfirmPending_ = true;
            },
            tr(STR_JJWXC_LOADING));
      },
      State::ChapterList);
}

jjwxc::Status JjwxcActivity::fetchChapterPrice(const uint32_t ordinal) {
  if (ordinal >= chapters_.size()) return jjwxc::Status::Protocol;
  if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < kWapPageRawCap + 32 * 1024) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  wap_->setNowUnix(cookieClock());
  char url[128];
  snprintf(url, sizeof(url), "%s/buy/%s/%lu", jjwxc::kWapHost, novelId_.c_str(),
           static_cast<unsigned long>(chapters_[ordinal].chapterId));
  auto* page = reinterpret_cast<char*>(scratch_);
  BufferCollector col{page, kWapPageRawCap, 0, false};
  jjwxc::WapOptions opt;
  jjwxc::WapResponse res;
  const int http = wap_->get(url, opt, &collectBuffer, &col, res);
  if (http < 0) {
    jobDetailCode_ = http;
    return jjwxc::Status::Network;
  }
  if (res.sawRedirect) return jjwxc::Status::NeedLogin;
  if (http != 200) {
    jobDetailCode_ = http;
    return jjwxc::Status::Server;
  }
  if (col.overflow || col.len == 0) return col.overflow ? jjwxc::Status::TooLarge : jjwxc::Status::EmptyResult;
  page[col.len] = '\0';
  size_t textLen = col.len;
  const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
  if (enc == txt_encoding::Encoding::Gbk) {
    const auto conv = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
    if (conv.rawLength == 0 || conv.utf8Length == 0) return jjwxc::Status::Protocol;
    textLen = conv.utf8Length;
    page[textLen] = '\0';
  }
  char chapterId[16];
  snprintf(chapterId, sizeof(chapterId), "%lu", static_cast<unsigned long>(chapters_[ordinal].chapterId));
  jjwxc::ChapterPrice price;
  if (!jjwxc::parseChapterPrice(page, textLen, chapterId, price)) {
    // Not in either list: the catalogue may be stale, or the chapter is free.
    return jjwxc::Status::EmptyResult;
  }
  if (!price.found && price.purchased) {
    jobPurchaseAlready_ = true;
    return jjwxc::Status::Ok;
  }
  if (!price.found || price.points == 0) return jjwxc::Status::EmptyResult;
  // The price page is the authority on ownership. Record a known-unpurchased
  // chapter so the reader's row can show VIP¥ even if the user cancels the
  // confirmation; a later successful purchase overwrites this with Missing and
  // then Cached.
  {
    JjwxcStore::TocReader toc;
    if (toc.openForUpdate(novelId_.c_str())) {
      JjwxcStore::TocRecord rec;
      if (toc.read(static_cast<uint32_t>(ordinal), rec)) {
        rec.state = static_cast<uint8_t>(JjwxcStore::ChapterState::Unpurchased);
        toc.update(static_cast<uint32_t>(ordinal), rec);
        if (static_cast<size_t>(ordinal) < chapterStates_.size()) {
          chapterStates_[static_cast<size_t>(ordinal)] = rec.state;
        }
      }
      toc.close();
    }
  }
  pendingPrice_ = price.points;
  LOG_INF(kTag, "purchase quote: chapter=%s points=%u", chapterId, (unsigned)price.points);
  return jjwxc::Status::Ok;
}

void JjwxcActivity::promptPurchaseConfirm() {
  if (pendingPurchaseOrdinal_ < 0 || static_cast<size_t>(pendingPurchaseOrdinal_) >= chapters_.size()) return;
  // Frame 13: a real bill instead of the generic two-line confirmation.
  const std::string work = info_.novelName[0] ? std::string(info_.novelName) : std::string(novel_.title);
  char chapterLabel[160];
  const auto& chapter = chapters_[static_cast<size_t>(pendingPurchaseOrdinal_)];
  std::snprintf(chapterLabel, sizeof(chapterLabel), "%s %lu · %s", tr(STR_CHAPTER),
                static_cast<unsigned long>(pendingPurchaseOrdinal_ + 1), chapter.name);
  auto confirmation = makeUniqueNoThrow<JjwxcPurchaseConfirmActivity>(
      renderer, mappedInput, work, std::string(chapterLabel), pendingPrice_);
  if (!confirmation) {
    LOG_ERR(kTag, "OOM: purchase confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      if (deepLinkReturnAction_ == DeepLinkAction::PurchaseChapter) {
        deepLinkReturnAction_ = DeepLinkAction::None;
        finishDeepLink(DeepLinkAction::PurchaseChapter, /*completed=*/false);
        return;
      }
      enter(State::ChapterList);
      return;
    }
    // A remembered password skips the keyboard; the price confirmation above is
    // still shown every time so a touch cannot spend points silently.
    char saved[7];
    if (jjwxcpay::load(saved)) {
      payPassword_.assign(saved, 6);
      memset(saved, 0, sizeof(saved));
      purchaseUsedSavedPassword_ = true;
      startPurchase();
      return;
    }
    purchaseUsedSavedPassword_ = false;
    promptPayPassword();
  });
}

void JjwxcActivity::promptPayPassword() {
  const auto handler = [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      if (deepLinkReturnAction_ == DeepLinkAction::PurchaseChapter) {
        deepLinkReturnAction_ = DeepLinkAction::None;
        finishDeepLink(DeepLinkAction::PurchaseChapter, /*completed=*/false);
        return;
      }
      enter(State::ChapterList);
      return;
    }
    const std::string code = std::get<KeyboardResult>(result.data).text;
    bool valid = code.size() == 6;
    for (const char c : code) {
      if (c < '0' || c > '9') valid = false;
    }
    if (!valid) {
      showMessage(tr(STR_JJWXC_PAY_PASSWORD_INVALID), State::ChapterList);
      return;
    }
    payPassword_ = code;
    startPurchase();
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_PAY_PASSWORD), "", 6, InputType::Password);
}

void JjwxcActivity::startPurchase() {
  if (payPassword_.empty() || pendingPurchaseOrdinal_ < 0) return;
  // The password leaves this object here: the worker gets a copy, and the member
  // is wiped before any input can reach it again.
  const std::string password = payPassword_;
  payPassword_.clear();
  purchaseAttemptInFlight_ = true;
  pendingPaySave_ = false;
  jobPurchaseWrongPassword_ = false;
  memset(lastPurchasePassword_, 0, sizeof(lastPurchasePassword_));
  const int ordinal = pendingPurchaseOrdinal_;
  withNetwork(
      [this, password, ordinal] {
        jobWhat_ = tr(STR_JJWXC_BUY_CHAPTER);
        jobNextState_ = State::ChapterList;
        returnState_ = State::ChapterList;
        launchJob(
            [this, password] {
              jobStatus_ = runPurchase(password);
            },
            tr(STR_JJWXC_LOADING));
      },
      State::ChapterList);
}

jjwxc::Status JjwxcActivity::runPurchase(const std::string& password) {
  const int ordinal = pendingPurchaseOrdinal_;
  if (ordinal < 0 || static_cast<size_t>(ordinal) >= chapters_.size()) return jjwxc::Status::Protocol;
  if (!haveSession()) return jjwxc::Status::NeedLogin;
  if (!ensureWapSession() || scratch_ == nullptr || scratchCap_ < 64 * 1024) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  char chapterId[16];
  snprintf(chapterId, sizeof(chapterId), "%lu", static_cast<unsigned long>(chapters_[ordinal].chapterId));
  char body[512];
  const size_t bodyLen = jjwxc::buildBuyBody(novelId_.c_str(), chapterId, password.c_str(), body, sizeof(body));
  if (bodyLen == 0) return jjwxc::Status::Protocol;
  wap_->setNowUnix(cookieClock());
  auto* page = reinterpret_cast<char*>(scratch_);
  BufferCollector col{page, 32 * 1024, 0, false};
  jjwxc::WapOptions opt;
  opt.contentType = jjwxc::kWapFormContentType;
  opt.referer = "https://m.jjwxc.net/buyvip";
  jjwxc::WapResponse res;
  const int http = wap_->post("https://m.jjwxc.net/buy/buy_vip", body, bodyLen, opt, &collectBuffer, &col, res);
  if (http < 0) {
    jobDetailCode_ = http;
    return jjwxc::Status::Network;
  }
  if (res.sawRedirect) return jjwxc::Status::NeedLogin;
  if (http != 200) {
    jobDetailCode_ = http;
    return jjwxc::Status::Server;
  }
  if (col.overflow || col.len == 0) return col.overflow ? jjwxc::Status::TooLarge : jjwxc::Status::Protocol;
  page[col.len] = '\0';
  size_t textLen = col.len;
  const auto enc = txt_encoding::detect(reinterpret_cast<const uint8_t*>(page), col.len, true);
  if (enc == txt_encoding::Encoding::Gbk) {
    const auto conv = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(page), col.len, scratchCap_, true);
    if (conv.rawLength == 0 || conv.utf8Length == 0) return jjwxc::Status::Protocol;
    textLen = conv.utf8Length;
    page[textLen] = '\0';
  }
  const jjwxc::PurchaseResult result = jjwxc::parsePurchaseResult(page, textLen);
  if (result.outcome != jjwxc::PurchaseOutcome::Success) {
    jobPurchaseWrongPassword_ = (result.outcome == jjwxc::PurchaseOutcome::WrongPassword);
    snprintf(jobServerMessage_, sizeof(jobServerMessage_), "%s", result.message);
    LOG_ERR(kTag, "purchase refused: outcome=%d (chapter=%s)", (int)result.outcome, chapterId);
    return jjwxc::Status::Server;
  }
  // Bought: the chapter is no longer refused. Mark it Missing so the next
  // cache/read fetches the real body instead of serving the placeholder.
  JjwxcStore::TocReader toc;
  if (toc.openForUpdate(novelId_.c_str())) {
    JjwxcStore::TocRecord rec;
    if (toc.read(static_cast<uint32_t>(ordinal), rec)) {
      rec.state = static_cast<uint8_t>(JjwxcStore::ChapterState::Missing);
      rec.remoteSize = 0;
      toc.update(static_cast<uint32_t>(ordinal), rec);
      if (static_cast<size_t>(ordinal) < chapterStates_.size()) {
        chapterStates_[static_cast<size_t>(ordinal)] = rec.state;
      }
    }
    toc.close();
  }
  if (!purchaseUsedSavedPassword_) {
    // Handed to the UI task by pollJob(); saving here would read NVS from the
    // PSRAM-stack worker, which is exactly the cache-disabled panic this app
    // already hit elsewhere.
    snprintf(lastPurchasePassword_, sizeof(lastPurchasePassword_), "%s", password.c_str());
    pendingPaySave_ = true;
  }
  jobPurchased_ = true;
  char notice[96];
  snprintf(notice, sizeof(notice), tr(STR_JJWXC_BOUGHT_FMT), static_cast<unsigned long>(result.points));
  jobNotice_ = notice;
  return jjwxc::Status::Ok;
}

// ---- downloaded-book maintenance -------------------------------------------

void JjwxcActivity::refreshCurrentChapter() {
  const std::string path = JjwxcStore::finalBookPath(novel_);
  const std::string cacheDir = bookCachePath(path);
  uint8_t bytes[10] = {};
  int len = 0;
  if (!cacheDir.empty()) {
    HalFile file;
    if (Storage.openFileForRead(kTag, cacheDir + "/progress.bin", file)) {
      len = static_cast<int>(file.read(bytes, sizeof(bytes)));
      file.close();
    }
  }
  if (len != 4 && len != 6 && len != 10) {
    showMessage(tr(STR_JJWXC_NO_PROGRESS), State::BookDetail);
    return;
  }
  const uint32_t spine = static_cast<uint32_t>(bytes[0] | (bytes[1] << 8));
  JjwxcStore::SpineEntry entry;
  uint32_t ordinal = 0;
  if (!JjwxcStore::readSpineEntry(novelId_.c_str(), spine, entry) ||
      !JjwxcStore::findOrdinalForChapter(novelId_.c_str(), entry.chapterId, ordinal)) {
    showMessage(tr(STR_JJWXC_NO_PROGRESS), State::BookDetail);
    return;
  }
  // Refresh, not Incremental: the point is to replace the body on the card.
  startCaching(ordinal, ordinal, /*openAfter=*/false, CacheMode::Refresh);
}

void JjwxcActivity::promptDeleteNovelCache() {
  // In-page dialog instead of ConfirmationActivity. Deleting from the detail
  // screen used to cost four full-screen transitions (confirm, busy, success,
  // library) for an operation that only unlinks files; the dialog is drawn over
  // the retained page and the job below gets an overlay on the same page.
  // Cancel is the default so a stray Confirm key cannot delete anything.
  const char* options[2] = {tr(STR_CANCEL), tr(STR_DELETE)};
  const char* headline = novel_.title[0] ? novel_.title : novelId_.c_str();
  optionPopup_.show(tr(STR_JJWXC_DELETE_CACHE_CONFIRM), headline, options, 2, 0, [this](const int index) {
    if (index != 1) {
      requestUpdate();
      return;
    }
    // Deleting a local cache is pure SD I/O. Wrapping it in withNetwork() made
    // the app ask for Wi-Fi (and bring it up) before deleting files that are
    // already on the card -- pointless, slow, and misleading when offline.
    jobWhat_ = tr(STR_JJWXC_DELETE_CACHE);
    jobNextState_ = State::Library;
    returnState_ = State::Library;
    jobDeletedNovel_ = true;
    inlineJob_ = true;
    if (!launchJob(
            [this] {
              jobStatus_ = runDeleteNovel();
            },
            tr(STR_JJWXC_DELETE_CACHE))) {
      jobDeletedNovel_ = false;
      inlineJob_ = false;
    }
  });
}

jjwxc::Status JjwxcActivity::runDeleteNovel() {
  const std::string path = JjwxcStore::finalBookPath(novel_);
  if (!path.empty()) {
    clearBookCache(path);
    Storage.remove(path.c_str());
  }
  if (!JjwxcStore::removeNovel(novelId_.c_str())) {
    jobFailKind_ = FailKind::StorageWrite;
    return jjwxc::Status::Protocol;
  }
  LOG_INF(kTag, "local cache deleted: %s", novelId_.c_str());
  return jjwxc::Status::Ok;
}

void JjwxcActivity::promptPrefetchCommentBodies() {
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput,
                                                              tr(STR_JJWXC_COMMENT_PREFETCH),
                                                              tr(STR_JJWXC_COMMENT_PREFETCH_HINT));
  if (!confirmation) {
    LOG_ERR(kTag, "OOM: comment prefetch confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    withNetwork(
        [this] {
          jobWhat_ = tr(STR_JJWXC_COMMENT_PREFETCH);
          jobNextState_ = State::BookDetail;
          returnState_ = State::BookDetail;
          jobNotice_.clear();
          launchJob([this] { prefetchAllCommentBodies(); }, tr(STR_JJWXC_COMMENT_PREFETCH));
        },
        State::BookDetail);
  });
}

// ---- reading progress ------------------------------------------------------

bool JjwxcActivity::syncReadingProgress() {
  if (!haveSession() || !ensureWapSession() || scratch_ == nullptr || scratchCap_ < 64 * 1024) return false;
  wap_->setNowUnix(cookieClock());

  char sid[256];
  if (jar_->get("sid", sid, sizeof(sid), cookieClock()) == 0) return false;
  char readerId[16];
  if (!jjwxc::readerIdFromSid(sid, readerId, sizeof(readerId))) return false;

  // The reader writes [spineIndex16, page16, totalPages16, visibleOffset32] into
  // the EPUB cache's progress.bin; the spine map turns the index into a chapter.
  const std::string bookPath = JjwxcStore::finalBookPath(novel_);
  const std::string cacheDir = bookCachePath(bookPath);
  uint8_t progressBytes[10] = {};
  int progressLen = 0;
  uint32_t spineIndex = 0;
  uint32_t offset = 0;
  if (!cacheDir.empty()) {
    HalFile file;
    if (Storage.openFileForRead(kTag, cacheDir + "/progress.bin", file)) {
      progressLen = static_cast<int>(file.read(progressBytes, sizeof(progressBytes)));
      file.close();
      if (progressLen == 4 || progressLen == 6 || progressLen == 10) {
        spineIndex = static_cast<uint32_t>(progressBytes[0] | (progressBytes[1] << 8));
        if (progressLen == 10) {
          offset = static_cast<uint32_t>(progressBytes[6]) | (static_cast<uint32_t>(progressBytes[7]) << 8) |
                   (static_cast<uint32_t>(progressBytes[8]) << 16) |
                   (static_cast<uint32_t>(progressBytes[9]) << 24);
        }
      } else {
        progressLen = 0;
      }
    }
  }
  JjwxcStore::SpineEntry spineEntry;
  const bool haveLocalChapter =
      progressLen != 0 && JjwxcStore::readSpineEntry(novelId_.c_str(), spineIndex, spineEntry);
  char localChapter[16] = "0";
  if (haveLocalChapter) snprintf(localChapter, sizeof(localChapter), "%lu", (unsigned long)spineEntry.chapterId);

  auto* page = reinterpret_cast<char*>(scratch_);
  const auto post = [&](const char* url, const char* body, const size_t bodyLen, const size_t cap) -> int {
    BufferCollector col{page, cap, 0, false};
    jjwxc::WapOptions opt;
    opt.contentType = jjwxc::kWapFormContentType;
    jjwxc::WapResponse res;
    const int http = wap_->post(url, body, bodyLen, opt, &collectBuffer, &col, res);
    if (http < 0 || res.sawRedirect || http != 200 || col.overflow) return -1;
    page[col.len] = '\0';
    return static_cast<int>(col.len);
  };

  // 1) Cloud -> local: a record written by another device is offered as a jump.
  char body[512];
  size_t bodyLen = jjwxc::buildProgressReadBody(novelId_.c_str(), localChapter, sid, body, sizeof(body));
  if (bodyLen > 0 && !jobCancel_.load()) {
    const int len = post("https://m.jjwxc.net/app.jjwxc/Wap/ReadingProgress/getReadingProgress", body, bodyLen,
                         8192);
    if (len > 0) {
      jjwxc::ReadingProgress parsed;
      if (jjwxc::parseReadingProgress(page, static_cast<size_t>(len), parsed) && parsed.valid) {
        cloudProgress_ = parsed;
        cloudHasProgress_ = !haveLocalChapter || strcmp(parsed.chapterId, localChapter) != 0;
        uint32_t ordinal = 0;
        const unsigned long remoteChapter = strtoul(parsed.chapterId, nullptr, 10);
        cloudOrdinalValid_ = cloudHasProgress_ &&
                             JjwxcStore::findOrdinalForChapter(novelId_.c_str(), static_cast<uint32_t>(remoteChapter),
                                                               ordinal);
        if (cloudOrdinalValid_) cloudOrdinal_ = ordinal;
        LOG_INF(kTag, "cloud progress: chapter=%s position=%lu source=%s jump=%d", parsed.chapterId,
                (unsigned long)parsed.position, parsed.source, cloudOrdinalValid_ ? 1 : 0);
      }
    }
  }

  // 2) Local -> cloud, but only when it changed since the last push. Without
  // this guard every book open would rewrite the same position, and an old
  // device position would keep overwriting newer phone reading.
  if (progressLen != 0 && haveLocalChapter && !jobCancel_.load()) {
    uint8_t synced[10] = {};
    int syncedLen = 0;
    HalFile syncedFile;
    if (Storage.openFileForRead(kTag, JjwxcStore::syncedProgressPath(novelId_.c_str()), syncedFile)) {
      syncedLen = static_cast<int>(syncedFile.read(synced, sizeof(synced)));
      syncedFile.close();
    }
    if (syncedLen != progressLen || memcmp(synced, progressBytes, static_cast<size_t>(progressLen)) != 0) {
      char chapterStr[16];
      snprintf(chapterStr, sizeof(chapterStr), "%lu", (unsigned long)spineEntry.chapterId);
      char writeBody[1024];
      const size_t writeLen =
          jjwxc::buildProgressWriteBody(novelId_.c_str(), readerId, chapterStr, offset, "", static_cast<uint32_t>(cookieClock()),
                                        sid, writeBody, sizeof(writeBody));
      if (writeLen > 0) {
        const int len = post("https://android.jjwxc.net/readingProgressAndroid/readingProgress", writeBody, writeLen,
                             4096);
        if (len > 0 && jjwxc::readingProgressResponseOk(page, static_cast<size_t>(len))) {
          HalFile out;
          if (Storage.openFileForWrite(kTag, JjwxcStore::syncedProgressPath(novelId_.c_str()), out)) {
            out.write(progressBytes, static_cast<size_t>(progressLen));
            out.flush();
          }
          LOG_INF(kTag, "reading progress pushed: chapter=%s position=%lu", chapterStr, (unsigned long)offset);
        } else {
          LOG_INF(kTag, "reading progress push was not acknowledged");
        }
      }
    }
  }
  return true;
}

void JjwxcActivity::jumpToCloudProgress() {
  if (!cloudHasProgress_ || !cloudOrdinalValid_) return;
  const uint32_t ordinal = cloudOrdinal_;
  const std::string path = JjwxcStore::finalBookPath(novel_);

  JjwxcStore::TocRecord rec;
  bool haveRec = false;
  {
    JjwxcStore::TocReader toc;
    if (toc.open(novelId_.c_str())) {
      haveRec = toc.read(ordinal, rec);
      toc.close();
    }
  }
  if (!haveRec) {
    showMessage(tr(STR_JJWXC_CLOUD_JUMP_FAILED), State::BookDetail);
    return;
  }
  if (rec.state == static_cast<uint8_t>(JjwxcStore::ChapterState::Unpurchased) ||
      rec.state == static_cast<uint8_t>(JjwxcStore::ChapterState::Locked)) {
    showMessage(tr(STR_JJWXC_ERR_PURCHASE), State::BookDetail);
    return;
  }
  uint32_t spine = 0;
  if (JjwxcStore::findSpineForOrdinal(novelId_.c_str(), ordinal, spine) && Storage.exists(path.c_str())) {
    writeReaderSpineTarget(path, spine);
    handOffToReader(path);
    return;
  }
  // Not packaged yet (or the EPUB is gone): cache exactly that chapter and open
  // it, which repackages the book and targets the same spine.
  cacheSingleChapter(ordinal, /*openAfter=*/true);
}

// Nothing calls this since the App API search was removed: WAP login yields a cookie
// jar, not a token, and fetchSearch() refuses without one. It is kept as the receiving
// end for the WAP search parser (see promptSearchKeyword for the page shape), together
// with search_/searchTotal_/keyword_ and the State::SearchResults renderer, so wiring
// search back up is a fetch function and not a feature rebuild.
bool JjwxcActivity::onSearchEntry(void* ctx, const jjwxc::SearchItem& item, size_t) {
  auto* self = static_cast<JjwxcActivity*>(ctx);
  if (self->jobCancel_.load()) return false;
  if (!self->jobReplacedTarget_) {
    self->search_.clear();
    self->jobReplacedTarget_ = true;
  }
  if (self->search_.size() < kMaxRows) self->search_.push_back(item);
  return true;
}

void JjwxcActivity::openBook(const std::string& novelId) {
  // Copied out before the network step: `novelId` usually aliases an element of
  // rowIds_, which a rebuild can invalidate while the Wi-Fi activity is up.
  novelId_ = novelId;
  // Where Back and a failed fetch return to. Captured before any state change so
  // it names the list the user actually tapped: shelf, search results, or a
  // channel/ranking list. Everything else (a deep link, in practice) falls back
  // to the shelf, which is what the old ternary did for every non-search state.
  bookDetailOrigin_ = (state_ == State::SearchResults)  ? State::SearchResults
                      : (state_ == State::DiscoverList) ? State::DiscoverList
                      : (state_ == State::Library)      ? State::Library
                                                        : State::Shelf;
  cloudHasProgress_ = false;
  cloudOrdinalValid_ = false;
  cloudOrdinal_ = 0;
  favoriteKnown_ = false;
  isFavorite_ = false;
  favoriteClassId_.clear();
  favoriteClassName_.clear();
  if (bookDetailOrigin_ == State::Shelf) {
    for (const auto& entry : shelf_) {
      if (novelId == entry.novelId) {
        favoriteKnown_ = true;
        isFavorite_ = true;
        break;
      }
    }
  }
  info_ = jjwxc::NovelInfo{};
  // Cleared with info_, not after the fetch: a book whose metadata request fails must
  // show no blurb rather than the previous book's.
  intro_.clear();
  novel_ = JjwxcStore::NovelRecord{};
  snprintf(novel_.novelId, sizeof(novel_.novelId), "%s", novelId.c_str());
  // Read the persisted record first: it is what makes the cached-chapter count and
  // the "open local copy" row correct before any network step, and what
  // finalBookPath() is derived from.
  JjwxcStore::loadNovel(novelId.c_str(), novel_);
  snprintf(novel_.novelId, sizeof(novel_.novelId), "%s", novelId.c_str());
  tocSummary_ = JjwxcStore::TocSummary{};
  chapters_.clear();
  chapterStates_.clear();
  chapterWindow_ = 0;
  chapterRowStart_ = 0;
  rangeFrom_ = -1;
  rangeTo_ = -1;
  infoLineCount_ = -1;
  detailLayoutValid_ = false;
  // Offline with this novel already on the card, the detail page still has a title,
  // an author and every cached chapter to offer. The fallback buried in the job below
  // cannot deliver that: withNetwork() stops at the Wi-Fi page and never launches the
  // job, so a downloaded book was unreachable without a network. Same fast path
  // loadChapters() uses, and a job for the same reason -- walking toc.bin must not
  // happen inside an input handler.
  if (!transport_.networkReady() && Storage.exists(JjwxcStore::tocPath(novelId.c_str()).c_str())) {
    jobWhat_ = tr(STR_JJWXC_BOOK_INFO);
    jobNextState_ = State::BookDetail;
    returnState_ = bookDetailOrigin_;
    launchJob(
        [this] {
          if (!loadTocIntoMemory()) jobStatus_ = jjwxc::Status::EmptyResult;
        },
        tr(STR_JJWXC_LOADING));
    return;
  }
  withNetwork(
      [this] {
        jobWhat_ = tr(STR_JJWXC_BOOK_INFO);
        jobNextState_ = State::BookDetail;
        returnState_ = bookDetailOrigin_;
        launchJob(
            [this] {
              jjwxc::Scratch scratch{scratch_, scratchCap_};
              // Anonymous and free-hosted: novelbasicinfo needs no credential, which is
              // why a book reached from the search results gets exactly the metadata a
              // shelf book does -- the same call, the same host, no sid.
              jobStatus_ = jjwxc::fetchNovelInfo(transport_, cred_, novelId_.c_str(), scratch, info_,
                                                 &JjwxcActivity::onNovelIntro, this);
              if (jobStatus_ == jjwxc::Status::Ok) {
                // The tagline leads the blurb when the service sent both. They are
                // independent fields, not long and short forms of one another (measured:
                // 13 characters vs 928 for the same book), and the tagline is the
                // sentence the site itself puts above the fold.
                if (!intro_.empty() && info_.introShort[0] != '\0') {
                  const size_t tagLen = strlen(info_.introShort);
                  jjwxc::PsramString lead;
                  lead.reserve(tagLen + 2 + intro_.size());
                  lead.append(info_.introShort, tagLen);
                  lead.append("\n\n", 2);
                  lead.append(intro_);
                  intro_.swap(lead);
                }
                // The detail page draws a cover, so make sure one exists before the page
                // is entered rather than on the frame that wants it. Best effort: it is
                // after the status check and never changes jobStatus_, because a book
                // with no picture is a book, not an error.
                if (!jobCancel_.load()) {
                  if (info_.coverUrl[0] != '\0') {
                    snprintf(novel_.coverUrl, sizeof(novel_.coverUrl), "%s", info_.coverUrl);
                  }
                  if (!ensureCoverThumb()) LOG_INF(kTag, "no cover thumbnail for %s", novelId_.c_str());
                }
              }
              // Cross-device progress is best effort and must never change the
              // outcome of opening the book: a stale session or an offline card
              // simply means no cloud row appears.
              if (haveSession() && !jobCancel_.load()) syncReadingProgress();
              if (haveSession() && !favoriteKnown_ && !jobCancel_.load()) fetchFavoriteStatus();
              // A card that already holds this novel stays usable without a network:
              // the detail page loses the fields only novelbasicinfo provides, but the
              // catalogue, the chapter states and the cached chapters are all local, so
              // browsing and repackaging still work. Without this the "Downloaded" list
              // would be a dead end offline.
              if (jobStatus_ != jjwxc::Status::Ok && loadTocIntoMemory()) {
                LOG_INF(kTag, "novelbasicinfo unavailable; using the cached catalogue");
                jobStatus_ = jjwxc::Status::Ok;
              }
              // A chapter-targeted deep link resolves its chapterId against the
              // in-memory catalogue, so load it in this worker before pollJob()
              // starts the cache/purchase.
              if (jobStatus_ == jjwxc::Status::Ok && chapters_.empty() &&
                  (deepLinkAction_ == DeepLinkAction::CacheChapter ||
                   deepLinkAction_ == DeepLinkAction::PurchaseChapter)) {
                if (!loadTocIntoMemory()) jobStatus_ = jjwxc::Status::EmptyResult;
              }
            },
            tr(STR_JJWXC_LOADING));
      },
      bookDetailOrigin_, /*returnDeepLinkOnCancel=*/deepLinkReturnAction_ != DeepLinkAction::None);
}

bool JjwxcActivity::loadTocIntoMemory() {
  JjwxcStore::TocReader toc;
  if (!toc.open(novelId_.c_str())) return false;
  chapters_.clear();
  chapterStates_.clear();
  const size_t cap = std::min<size_t>(toc.count(), kMaxChapters);
  chapters_.reserve(cap);
  chapterStates_.reserve(cap);
  JjwxcStore::TocRecord rec;
  for (size_t i = 0; i < cap; ++i) {
    if (!toc.read(static_cast<uint32_t>(i), rec)) break;
    jjwxc::ChapterEntry entry{};
    entry.chapterId = rec.chapterId;
    snprintf(entry.name, sizeof(entry.name), "%s", rec.title);
    entry.size = rec.remoteSize;
    entry.isVip = rec.isVip;
    chapters_.push_back(entry);
    chapterStates_.push_back(rec.state);
  }
  toc.close();
  uint32_t highest = 0;
  if (!JjwxcStore::readTocSummary(novelId_.c_str(), tocSummary_, highest)) tocSummary_.count = 0;
  return !chapters_.empty();
}

// Fetches the catalogue and rewrites toc.bin in the same pass, merging the cache
// state of the previous index in. `after` is the screen the user asked for: the
// range presets or the annotated chapter list.
void JjwxcActivity::loadChapters(const State after, const bool repackageAfter) {
  // A newly loaded catalogue starts at chapter 1. Keeping the old window would
  // land the user in the middle of a list they have not seen yet.
  chapterWindow_ = 0;
  chapterRowStart_ = 0;
  jobWhat_ = tr(STR_JJWXC_CHAPTERS);
  jobNextState_ = after;
  returnState_ = State::BookDetail;
  if (repackageAfter) {
    const JjwxcStore::NovelRecord keyRec = buildNovelRecord(tocSummary_, tocSummary_.count);
    const std::string keyPath = JjwxcStore::finalBookPath(keyRec);
    if (keyPath.empty() || !JjwxcStore::ensureBookContentKey(keyPath, contentKey_)) {
      LOG_ERR(kTag, "no content key for chapter-list repackage");
      showMessage(tr(STR_JJWXC_ERR_STORAGE), State::BookDetail);
      deepLinkReturnAction_ = DeepLinkAction::None;
      return;
    }
    contentKeyPath_ = keyPath + ".key";
  }
  // Offline with a catalogue already on the card: browse it instead of
  // demanding a network from an ordinary in-app chapter-list tap. The explicit
  // reader "update chapter list" is different: it must reach the server (and
  // asks for Wi-Fi); silently reading the cached toc made the command look
  // like it worked while the nav kept its old suffixes.
  if (!repackageAfter && !transport_.networkReady() &&
      Storage.exists(JjwxcStore::tocPath(novelId_.c_str()).c_str())) {
    launchJob(
        [this] {
          if (!loadTocIntoMemory()) jobStatus_ = jjwxc::Status::EmptyResult;
        },
        tr(STR_JJWXC_LOADING));
    return;
  }
  withNetwork([this, after, repackageAfter] {
    chapters_.clear();
    chapterStates_.clear();
    chapters_.reserve(std::min<size_t>(kMaxChapters, info_.chapterCount ? info_.chapterCount : 64));
    chapterStates_.reserve(std::min<size_t>(kMaxChapters, info_.chapterCount ? info_.chapterCount : 64));
    chapterTotal_ = 0;
    chapterVipStart_ = 0;
    launchJob(
        [this, after, repackageAfter] {
          jjwxc::Scratch scratch{scratch_, scratchCap_};
          JjwxcStore::TocMerger merger;
          if (!merger.begin(novelId_.c_str())) {
            jobStatus_ = jjwxc::Status::Protocol;
            jobFailKind_ = FailKind::StorageWrite;
            return;
          }
          ChapterListCtx ctx{this, &merger};
          // kMaxTocEntries, not kMaxChapters: the merger writes each record straight
          // to SD and keeps none of them, so the on-card index can and should cover
          // the whole catalogue. Capping the stream at kMaxChapters used to truncate
          // toc.bin silently -- CapSink reports a hit cap as a normal end of stream,
          // so a 4000-chapter novel came back Ok with 1500 records and "whole book"
          // quietly meant "first 1500". onChapterEntry still bounds the in-memory
          // mirror at kMaxChapters, which is the constraint that actually costs RAM.
          jobStatus_ = jjwxc::fetchChapterList(transport_, cred_, novelId_.c_str(), &JjwxcActivity::onChapterEntry,
                                               &ctx, kMaxTocEntries, scratch, chapterTotal_, chapterVipStart_);
          const bool cancelled = jobCancel_.load();
          if (jobStatus_ != jjwxc::Status::Ok || cancelled || chapters_.empty()) {
            // Never replace a good index with a partial one: abort() drops
            // toc.bin.part and leaves the previous toc.bin untouched.
            merger.abort();
            if (jobStatus_ == jjwxc::Status::Ok) jobStatus_ = jjwxc::Status::EmptyResult;
            loadTocIntoMemory();
            return;
          }
          JjwxcStore::TocSummary summary;
          if (!merger.finish(summary)) {
            jobStatus_ = jjwxc::Status::Protocol;
            jobFailKind_ = FailKind::StorageWrite;
            return;
          }
          tocSummary_ = summary;
          // Persist the metadata the packager and the "Downloaded" list need while
          // offline. A failure here does not invalidate the catalogue that was
          // just written, so it is logged rather than surfaced.
          novel_ = buildNovelRecord(summary, summary.count);
          if (!JjwxcStore::saveNovel(novel_)) {
            LOG_ERR(kTag, "saveNovel failed for %s", novelId_.c_str());
          }
          if (repackageAfter) {
            // Always repackage on the explicit reader "update chapter list"
            // action, even when the count is unchanged: this is what removes
            // legacy nav suffixes and fixes an old partial nav. Placeholders
            // make new chapters visible without downloading a single body. The
            // scratch buffer held the catalogue response; the fetch is over, so
            // packaging can reuse it (the same rule as cacheChapterRange).
            const std::string path = JjwxcStore::finalBookPath(novel_);
            const JjwxcEpub::Result packaged =
                JjwxcEpub::packageBook(novel_, scratch_, kZipBufferBytes, contentKey_, contentKeyPath_,
                                       &onPackageCancelled, this, nullptr, nullptr);
            if (packaged == JjwxcEpub::Result::Cancelled) {
              jobStatus_ = jjwxc::Status::Cancelled;
              return;
            }
            if (packaged != JjwxcEpub::Result::Ok) {
              jobStatus_ = jjwxc::Status::Protocol;
              jobFailKind_ = FailKind::StorageWrite;
              return;
            }
            invalidateReaderCache(path);
            if (!library::markLibraryIndexDirty()) LOG_ERR(kTag, "cannot mark the library index dirty");
            LOG_INF(kTag, "repackaged chapter list placeholders: %lu chapters into %s",
                    static_cast<unsigned long>(summary.count), path.c_str());
          }
          if (rangeFrom_ < 0 || rangeTo_ < 0) {
            // Default the custom range to the next 20 chapters, which is what a
            // reader usually wants; the presets do not use it.
            rangeFrom_ = 0;
            rangeTo_ = static_cast<int>(std::min<uint32_t>(summary.count, 20)) - 1;
          }
        },
        tr(STR_JJWXC_LOADING));
  },
      State::BookDetail, /*returnDeepLinkOnCancel=*/repackageAfter);
}

bool JjwxcActivity::onChapterEntry(void* ctx, const jjwxc::ChapterEntry& e, size_t) {
  auto* c = static_cast<ChapterListCtx*>(ctx);
  JjwxcActivity* self = c->self;
  if (self->jobCancel_.load()) return false;

  JjwxcStore::TocRecord record;
  if (!c->merger->add(e, record)) return false;

  // The in-memory mirrors are capped, toc.bin is not: a 4000-chapter novel gets a
  // complete index on the card and a truncated but still usable list on screen.
  if (self->chapters_.size() < kMaxChapters) {
    self->chapters_.push_back(e);
    self->chapterStates_.push_back(record.state);
  }
  return true;
}

bool JjwxcActivity::onChapterText(void* ctx, const char* utf8, size_t len) {
  auto* c = static_cast<ChapterSinkCtx*>(ctx);
  if (!c->writer->feed(utf8, len)) {
    c->failed = true;
    return false;
  }
  return true;
}

bool JjwxcActivity::onNovelEntry(void* ctx, const JjwxcStore::NovelRecord& rec, size_t) {
  auto* self = static_cast<JjwxcActivity*>(ctx);
  if (self->books_.size() < kMaxLocalBooks) self->books_.push_back(rec);
  return true;
}

// Receives `novelIntro` from the SAX parser and turns it into the prose the detail
// page draws. Runs on the worker task; the UI thread reads intro_ only after
// jobDone_, which is the same ordering info_ itself relies on.
bool JjwxcActivity::onNovelIntro(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<JjwxcActivity*>(ctx);
  if (self->jobCancel_.load()) return false;
  self->intro_.clear();
  if (value == nullptr || len == 0) return true;

  // Bounded, and cut on a code-point boundary: the cap is what stops a pathological
  // reply from asking PSRAM for megabytes, and a blurb that ends in half a character
  // would render as a box.
  //
  // The allocation itself is PSRAM (jjwxc::PsramString) and cannot be null-checked --
  // under -fno-exceptions a failed std::string allocation aborts rather than reporting
  // -- so the cap is the only defence and 8 KB against ~3.6 MB of free PSRAM is a wide
  // margin. This is the same exposure every row string in this app already has.
  size_t n = len < kMaxIntroBytes ? len : kMaxIntroBytes;
  while (n > 0 && n < len && (static_cast<unsigned char>(value[n]) & 0xC0) == 0x80) --n;
  self->intro_.assign(value, n);

  // The service escapes the blurb's markup: the line breaks are the literal seven
  // characters `&lt;br/&gt;` (measured: 40 of them in a 928-character intro, and no
  // newline anywhere in it). Both repairs run in place and never grow the buffer,
  // and both are already in the image for the WAP chapter path, so unescaping the
  // blurb costs no Flash:
  //   wapDecodeEntities  &lt;br/&gt; -> <br/>, &amp;/&nbsp;/&#NN; -> themselves
  //   wapHtmlToText      <br/> -> '\n', every other tag dropped, 3+ newlines folded
  //                      to 2, trailing space before a break dropped, ends trimmed
  char* buf = &self->intro_[0];
  size_t out = jjwxc::wapDecodeEntities(buf, self->intro_.size());
  out = jjwxc::wapHtmlToText(buf, out);
  self->intro_.resize(out);  // never grows, so this cannot allocate
  LOG_INF(kTag, "blurb: %u raw -> %u bytes of prose", (unsigned)n, (unsigned)out);
  return true;
}

bool JjwxcActivity::onPackageCancelled(void* ctx) { return static_cast<JjwxcActivity*>(ctx)->jobCancel_.load(); }

void JjwxcActivity::onPackageProgress(void* ctx) {
  auto* self = static_cast<JjwxcActivity*>(ctx);
  self->dlDone_ = self->dlDone_.load() + 1;
}

// ---------------------------------------------------------------------------
// Caching
// ---------------------------------------------------------------------------

JjwxcStore::NovelRecord JjwxcActivity::buildNovelRecord(const JjwxcStore::TocSummary& sum,
                                                        const uint32_t remoteCount) const {
  JjwxcStore::NovelRecord rec = novel_;
  snprintf(rec.novelId, sizeof(rec.novelId), "%s", novelId_.c_str());
  // The server's own fields win, but an offline run keeps whatever was persisted
  // last time rather than blanking the title out of the finished EPUB's name.
  if (info_.novelName[0]) snprintf(rec.title, sizeof(rec.title), "%s", info_.novelName);
  if (info_.authorName[0]) snprintf(rec.author, sizeof(rec.author), "%s", info_.authorName);
  if (info_.coverUrl[0]) snprintf(rec.coverUrl, sizeof(rec.coverUrl), "%s", info_.coverUrl);
  if (rec.title[0] == '\0') snprintf(rec.title, sizeof(rec.title), "%s", novelId_.c_str());
  rec.remoteChapterCount = remoteCount ? remoteCount : chapterTotal_;
  rec.cachedChapterCount = sum.readable;
  rec.packagedChapterCount = sum.packaged;
  rec.savedAt = static_cast<uint32_t>(time(nullptr));
  return rec;
}

bool JjwxcActivity::writeChapterStub(const std::string& path, const JjwxcStore::TocRecord& rec, const bool locked) {
  // The stub is composed from THIS record's own title plus a fixed localised
  // note. There is no code path that copies a fetched body into it, which is the
  // structural guarantee against one chapter's text appearing in another: the
  // writer that received the rejected response was aborted and its part file
  // removed before this is called.
  return JjwxcEpub::writePlaceholder(path, rec.title,
                                     locked ? tr(STR_JJWXC_CHAPTER_LOCKED) : tr(STR_JJWXC_CHAPTER_UNPURCHASED));
}

// Downloads NovelInfo::coverUrl into cover.source.jpg. Best effort: a book
// without a cover still packages, still opens and still reaches the shelf, it
// just has no thumbnail.
bool JjwxcActivity::downloadCover() {
  if (novel_.coverUrl[0] == '\0') return false;
  const std::string finalPath = JjwxcStore::coverSourcePath(novelId_.c_str());
  const std::string partPath = finalPath + ".part";

  char url[256];
  // The firmware's HTTP client is wolfSSL-backed and cannot speak plain HTTP
  // (JjwxcProtocol.h:22-24), so an http:// cover URL is upgraded rather than
  // failing outright. A host with no TLS listener simply fails here and the book
  // ships without a cover.
  if (strncmp(novel_.coverUrl, "http://", 7) == 0) {
    snprintf(url, sizeof(url), "https://%s", novel_.coverUrl + 7);
  } else {
    snprintf(url, sizeof(url), "%s", novel_.coverUrl);
  }

  // No Referer here. The image CDN allow-lists jjwxc.net referers but rejects any
  // that carry a query string, and kReferer is exactly that
  // (`http://android.jjwxc.net?v=381`), which made every cover a 403. Measured
  // against i5-static.jjwxc.net: no Referer 200, `http://android.jjwxc.net` 200,
  // `http://android.jjwxc.net?v=381` 403. The API hosts want kReferer, so this is
  // deliberately narrower than the rest of the app.
  const jjwxc::RequestHeader headers[] = {{"User-Agent", jjwxc::kUserAgent}};
  for (int redirect = 0; redirect < 3; ++redirect) {
    if (jobCancel_.load()) return false;
    if (Storage.exists(partPath.c_str())) Storage.remove(partPath.c_str());
    HalFile file;
    if (!Storage.openFileForWrite(kTag, partPath, file)) {
      LOG_ERR(kTag, "cannot open %s", partPath.c_str());
      return false;
    }
    CoverSinkCtx body{&file, 0, false};
    CoverHeaderCtx headerCtx;
    const int status = transport_.request("GET", url, headers, 1, nullptr, 0, &JjwxcActivity::onCoverBody, &body,
                                          &JjwxcActivity::onCoverHeader, &headerCtx);
    file.flush();
    // Explicit close before the remove/rename below (rule #5).
    file.close();

    if (status == 200 && !body.failed && body.bytes > 0) {
      // The decoder is chosen from the entry's extension (lib/Epub/Epub.cpp:62),
      // so a payload that is not an image must not be packaged under one.
      HalFile check;
      uint8_t magic[3] = {};
      const bool isImage = Storage.openFileForRead(kTag, partPath, check) &&
                           check.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic)) &&
                           ((magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) ||
                            (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N'));
      if (isImage) {
        if (WeReadStore::atomicReplace(partPath, finalPath)) {
          LOG_INF(kTag, "cover saved: %u bytes", (unsigned)body.bytes);
          return true;
        }
      } else {
        LOG_ERR(kTag, "cover payload is not an image (%u bytes)", (unsigned)body.bytes);
      }
      Storage.remove(partPath.c_str());
      return false;
    }
    const bool redirectStatus = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    Storage.remove(partPath.c_str());
    if (!redirectStatus || strncmp(headerCtx.location, "http", 4) != 0) {
      LOG_ERR(kTag, "cover download failed: status=%d url=%.80s", status, url);
      return false;
    }
    snprintf(url, sizeof(url), "%s", headerCtx.location);
  }
  LOG_ERR(kTag, "cover download gave up after redirects");
  return false;
}

bool JjwxcActivity::onCoverBody(void* ctx, const uint8_t* data, size_t len) {
  auto* c = static_cast<CoverSinkCtx*>(ctx);
  if (c->bytes + len > kMaxCoverBytes) {
    LOG_ERR(kTag, "cover exceeds %u bytes", (unsigned)kMaxCoverBytes);
    c->failed = true;
    return false;
  }
  if (c->file->write(data, len) != len) {
    c->failed = true;
    return false;
  }
  c->bytes += len;
  return true;
}

void JjwxcActivity::onCoverHeader(void* ctx, const char* name, size_t nameLen, const char* value, size_t) {
  auto* c = static_cast<CoverHeaderCtx*>(ctx);
  // Header names are not case-normalised by the transport (JjwxcTransport.h:26).
  if (nameLen == 8 && strncasecmp(name, "location", 8) == 0 && value) {
    snprintf(c->location, sizeof(c->location), "%s", value);
  }
}

// cover.source.jpg -> cover.thumb.bmp, at the size the detail page lays out.
//
// This is WeRead's cover pipeline, not a new one: the same converter pair its
// Operation::convertCoverSource() uses (WeReadClient.cpp:3427-3430), the same
// `.part` + atomicReplace write, and the same `Bitmap` + GfxRenderer::drawBitmap the
// themes and both reading apps already draw cached covers with. Both converters are
// linked into the image for the EPUB thumbnail path regardless, so the Flash cost is
// the few dozen instructions below.
//
// It runs on the worker task: JPEGDEC decodes a 300x420 source in MCU rows and the
// write is a stream, but neither belongs in an input handler.
bool JjwxcActivity::buildCoverThumb() {
  const std::string source = JjwxcStore::coverSourcePath(novelId_.c_str());
  const std::string thumb = JjwxcStore::coverThumbPath(novelId_.c_str());
  const std::string part = thumb + ".part";
  if (Storage.exists(part.c_str())) Storage.remove(part.c_str());

  HalFile magicProbe;
  if (!Storage.openFileForRead(kTag, source, magicProbe)) {
    LOG_ERR(kTag, "cannot open the cover source %s", source.c_str());
    return false;
  }
  // Chosen on the bytes, not the extension: downloadCover() accepts either magic and
  // names the file .jpg whichever it got. Epub's converter picks the same way.
  uint8_t magic[3] = {};
  const bool read = magicProbe.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic));
  magicProbe.close();
  const bool isJpeg = read && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
  const bool isPng = read && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N';
  if (!isJpeg && !isPng) {
    LOG_ERR(kTag, "cover source is neither JPEG nor PNG");
    return false;
  }

  HalFile input;
  HalFile output;
  if (!Storage.openFileForRead(kTag, source, input) || !Storage.openFileForWrite(kTag, part, output)) {
    if (input.isOpen()) input.close();
    if (output.isOpen()) output.close();
    Storage.remove(part.c_str());
    LOG_ERR(kTag, "cannot open the cover thumbnail for writing");
    return false;
  }
  const bool converted = isPng ? PngToBmpConverter::pngFileToBmpStreamWithSize(input, output, kCoverThumbWidth,
                                                                               kCoverThumbHeight)
                               : JpegToBmpConverter::jpegFileToBmpStreamWithSize(input, output, kCoverThumbWidth,
                                                                                 kCoverThumbHeight);
  // Explicit close before the rename (AGENTS.md rule #5).
  input.close();
  output.flush();
  output.close();
  if (!converted) {
    Storage.remove(part.c_str());
    LOG_ERR(kTag, "cover thumbnail conversion failed (%s)", isPng ? "png" : "jpeg");
    return false;
  }
  if (!WeReadStore::atomicReplace(part, thumb)) {
    Storage.remove(part.c_str());
    LOG_ERR(kTag, "cannot put the cover thumbnail in place");
    return false;
  }
  LOG_INF(kTag, "cover thumbnail built: %dx%d", kCoverThumbWidth, kCoverThumbHeight);
  return true;
}

bool JjwxcActivity::ensureCoverThumb() {
  const std::string thumb = JjwxcStore::coverThumbPath(novelId_.c_str());
  const std::string source = JjwxcStore::coverSourcePath(novelId_.c_str());
  if (jobCancel_.load()) return false;

  bool freshSource = false;
  if (!Storage.exists(source.c_str())) {
    if (novel_.coverUrl[0] == '\0') return false;  // nothing to fetch it from
    if (!JjwxcStore::ensureNovelRoot(novelId_.c_str())) return false;
    if (!downloadCover()) {
      // Keep a thumbnail built from an earlier source if there is one: a stale
      // picture beats no picture only when nothing fresher is available, and here
      // nothing is.
      return Storage.exists(thumb.c_str());
    }
    freshSource = true;
  }
  if (!freshSource && Storage.exists(thumb.c_str())) return true;
  // A new source invalidates the thumbnail built from the old one. Covers do change
  // (the author replaces the art), and the wrong cover is worse than none.
  if (Storage.exists(thumb.c_str()) && !Storage.remove(thumb.c_str())) {
    LOG_ERR(kTag, "cannot remove the stale cover thumbnail");
    return false;
  }
  return buildCoverThumb();
}

void JjwxcActivity::loadCoverThumb() {
  releaseCover();
  const std::string thumb = JjwxcStore::coverThumbPath(novelId_.c_str());
  if (!Storage.exists(thumb.c_str())) return;
  if (!Storage.openFileForRead(kTag, thumb, coverFile_)) {
    LOG_ERR(kTag, "cannot open %s", thumb.c_str());
    return;
  }
  auto bitmap = makeUniqueNoThrow<Bitmap>(coverFile_);
  if (!bitmap) {
    LOG_ERR(kTag, "OOM: cover bitmap");
    coverFile_.close();
    return;
  }
  // parseHeaders() is what validates the file; a thumbnail left half-written by an
  // interrupted run is rejected here and the page simply draws no picture.
  if (bitmap->parseHeaders() != BmpReaderError::Ok || bitmap->getWidth() <= 0 || bitmap->getHeight() <= 0) {
    LOG_ERR(kTag, "cover thumbnail is not a readable BMP");
    bitmap.reset();
    coverFile_.close();
    return;
  }
  coverBitmap_ = std::move(bitmap);
}

void JjwxcActivity::releaseCover() {
  // The bitmap holds a pointer to the file, so it goes first.
  coverBitmap_.reset();
  if (coverFile_.isOpen()) coverFile_.close();
}

void JjwxcActivity::invalidateReaderCache(const std::string& path) {
  // book.bin and section.bin are keyed on the path alone and carry no source size
  // or modification time (docs/file-formats.md#book-bin), so a repackaged EPUB at
  // the same path would otherwise be served from the previous pagination.
  const std::string cacheDir = bookCachePath(path);
  uint8_t saved[16] = {};
  int savedLen = 0;
  // progress.bin is [spineIndex, pageNumber, chapterTotalPageCount,
  // visibleTextOffset] (EpubReaderActivity.cpp:393-414). An append-only run
  // changes none of those for the spine entries that already existed, so the
  // position can be carried across; anything else shifts pages and starts fresh.
  if (appendOnlyRun_ && !cacheDir.empty()) {
    HalFile progress;
    if (Storage.openFileForRead(kTag, cacheDir + "/progress.bin", progress)) {
      const int got = progress.read(saved, sizeof(saved));
      if (got == 4 || got == 6 || got == 10) savedLen = got;
    }
  }
  if (!clearBookCache(path)) LOG_ERR(kTag, "cannot clear the reader cache for %s", path.c_str());
  if (savedLen <= 0 || !Storage.ensureDirectoryExists(cacheDir.c_str())) return;
  HalFile restored;
  if (!Storage.openFileForWrite(kTag, cacheDir + "/progress.bin", restored)) {
    LOG_ERR(kTag, "cannot restore reading progress for %s", path.c_str());
    return;
  }
  restored.write(saved, static_cast<size_t>(savedLen));
  restored.flush();
  LOG_INF(kTag, "carried %d bytes of reading progress across the repackage", savedLen);
}

// Points the reader at one chapter. progress.bin is
// [spineIndex lo, spineIndex hi, pageNumber lo, pageNumber hi] and a 4-byte file is
// accepted as-is (EpubReaderActivity.cpp:393-405), so writing page 0 of the target
// spine entry is enough. Written after invalidateReaderCache(), which would
// otherwise delete it along with the rest of the cache.
void JjwxcActivity::writeReaderSpineTarget(const std::string& path, const uint32_t spineIndex) {
  const std::string cacheDir = bookCachePath(path);
  if (cacheDir.empty()) {
    LOG_ERR(kTag, "no cache directory for %s; cannot target a chapter", path.c_str());
    return;
  }
  if (!Storage.ensureDirectoryExists(cacheDir.c_str())) {
    LOG_ERR(kTag, "cannot create %s", cacheDir.c_str());
    return;
  }
  const uint16_t clamped = static_cast<uint16_t>(std::min<uint32_t>(spineIndex, 0xFFFFu));
  const uint8_t data[4] = {static_cast<uint8_t>(clamped & 0xFF), static_cast<uint8_t>(clamped >> 8), 0, 0};
  HalFile file;
  if (!Storage.openFileForWrite(kTag, cacheDir + "/progress.bin", file)) {
    LOG_ERR(kTag, "cannot write the reading target for %s", path.c_str());
    return;
  }
  file.write(data, sizeof(data));
  file.flush();
  LOG_INF(kTag, "reader will open %s at spine %u", path.c_str(), (unsigned)clamped);
}

std::string JjwxcActivity::cacheSummary() const {
  char buf[kInfoLineBytes];
  const unsigned long readable = tocSummary_.readable;
  const unsigned long total = tocSummary_.count;
  const unsigned long blocked = tocSummary_.unpurchased + tocSummary_.locked;
  const unsigned long added = jobNewlyCached_.load();
  const unsigned long skipped = dlSkipped_.load();
  // renderMessage() wraps greedily and has no notion of a newline, so the parts are
  // joined into one sentence rather than laid out on separate lines.
  int n = snprintf(buf, sizeof(buf), "%s %lu/%lu", tr(STR_JJWXC_SUM_CACHED), readable, total);
  // Built first: the buffer it lands in is not `buf`, so the append below cannot
  // overwrite a source it is still reading.
  char stopped[64];
  snprintf(stopped, sizeof(stopped), tr(STR_JJWXC_SUM_STOPPED_FMT), static_cast<unsigned long>(jobStoppedOrdinal_ + 1));
  const auto append = [&buf, &n](const char* label, unsigned long value) {
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) return;
    n += snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " · %s %lu", label, value);
  };
  if (blocked > 0) append(tr(STR_JJWXC_SUM_UNPURCHASED), blocked);
  if (added > 0) append(tr(STR_JJWXC_SUM_ADDED), added);
  if (skipped > 0) append(tr(STR_JJWXC_SUM_SKIPPED), skipped);
  if (jobStoppedAtPurchase_ && n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
    // Its own sentence rather than a "label value" pair, because the ordinal belongs
    // inside the phrase in every language this ships in.
    n += snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " · %s", stopped);
  }
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
    // Truncated by the buffer rather than by the renderer: say the one number that
    // matters instead of showing a half sentence.
    snprintf(buf, sizeof(buf), "%s %lu/%lu", tr(STR_JJWXC_SUM_CACHED), readable, total);
  }
  return std::string(buf);
}

bool JjwxcActivity::chapterLooksVip(const JjwxcStore::TocRecord& rec) const {
  if (rec.isVip != 0) return true;
  // Fallback for a catalogue that omits the per-chapter flag. `vipChapterId` is
  // the novel's first VIP chapter (JjwxcProtocol.h:106), so every chapter at or
  // above it is paid and every one below it is free: this cannot turn a free
  // chapter into a paid one. Two sources because only one of them is populated on
  // an offline run.
  const uint32_t vipStart = info_.vipChapterId != 0 ? info_.vipChapterId : chapterVipStart_;
  return vipStart != 0 && rec.chapterId >= vipStart;
}

jjwxc::TokenLiveness JjwxcActivity::tokenLiveness() {
  if (tokenProbed_) return tokenLiveness_;
  tokenProbed_ = true;
  probeStatus_ = jjwxc::Status::Ok;
  if (!haveSession()) {
    tokenLiveness_ = jjwxc::TokenLiveness::Dead;
    probeStatus_ = jjwxc::Status::NeedLogin;
    LOG_ERR(kTag, "token liveness: no credential held");
    return tokenLiveness_;
  }
  const jjwxc::Scratch scratch{scratch_, scratchCap_};
  const jjwxc::Status s = jjwxc::probeTokenAlive(transport_, cred_, scratch);
  if (s == jjwxc::Status::Ok) {
    tokenLiveness_ = jjwxc::TokenLiveness::Alive;
    LOG_INF(kTag, "token liveness probe: session accepted");
    return tokenLiveness_;
  }
  if (s == jjwxc::Status::NeedLogin) {
    tokenLiveness_ = jjwxc::TokenLiveness::Dead;
    probeStatus_ = jjwxc::Status::NeedLogin;
    LOG_ERR(kTag, "token liveness probe: session rejected");
    return tokenLiveness_;
  }
  // No verdict. Neither guess is safe: "not purchased" would stamp a wrong
  // placeholder that TocMerger then remembers until an explicit refresh, and
  // "session over" would send the user to re-login with a token that works.
  tokenLiveness_ = jjwxc::TokenLiveness::Inconclusive;
  probeStatus_ = s;
  LOG_ERR(kTag, "token liveness probe inconclusive: %s", jjwxc::statusText(s));
  return tokenLiveness_;
}

// The cache job. Fetches only what is missing, then rebuilds the single EPUB from
// everything on the card.
void JjwxcActivity::cacheChapterRange(uint32_t fromOrdinal, uint32_t toOrdinal) {
  jobStatus_ = jjwxc::Status::Ok;
  jobPackageFailed_ = false;
  jobRangeUnreadable_ = false;
  jobDetailCode_ = 0;
  // One probe per job, never one per chapter.
  tokenProbed_ = false;
  tokenLiveness_ = jjwxc::TokenLiveness::Alive;
  probeStatus_ = jjwxc::Status::Ok;
  dlPath_.clear();

  const auto storageFail = [this] {
    jobStatus_ = jjwxc::Status::Protocol;
    jobFailKind_ = FailKind::StorageWrite;
  };

  if (!JjwxcStore::ensureNovelRoot(novelId_.c_str())) {
    storageFail();
    return;
  }
  if (!JjwxcStore::reclaimNovelTempFiles(novelId_.c_str())) {
    LOG_ERR(kTag, "temp file reclamation incomplete before caching");
  }

  JjwxcStore::TocSummary before;
  uint32_t highestPackaged = 0;
  if (!JjwxcStore::readTocSummary(novelId_.c_str(), before, highestPackaged) || before.count == 0) {
    jobStatus_ = jjwxc::Status::EmptyResult;
    return;
  }
  if (fromOrdinal >= before.count) {
    jobStatus_ = jjwxc::Status::EmptyResult;
    return;
  }
  if (toOrdinal >= before.count) toOrdinal = before.count - 1;
  // Appending after the last chapter already in the book leaves every existing
  // spine index and page count untouched, so the reader's position survives the
  // cache invalidation a repackage needs. A refresh, or filling a gap in the
  // middle, moves pages and therefore does not.
  //
  // `before.packaged == 0` is deliberately NOT treated as append-only. There is no
  // existing book to append to in that case, so the only progress.bin that could be
  // carried over is a stale one left behind by a deleted or renamed artefact -- and
  // restoring it would drop the reader at a spine index this newly built book does
  // not have. Requiring a real predecessor makes the judgement strictly safer and
  // costs nothing: a genuine first package has no progress.bin to save anyway.
  appendOnlyRun_ = cacheMode_ == CacheMode::Incremental && before.packaged > 0 && fromOrdinal > highestPackaged;

  JjwxcStore::TocReader toc;
  if (!toc.openForUpdate(novelId_.c_str())) {
    storageFail();
    return;
  }

  jjwxc::Scratch scratch{scratch_, scratchCap_};
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  uint32_t written = 0;
  uint32_t stubs = 0;
  uint32_t skipped = 0;
  // Spine index the reader must open at when this run was asked to hand over one
  // particular chapter: the number of packaged records ahead of it, which is the
  // same predicate JjwxcEpub uses to build the spine. -1 while the target is not
  // in the range or has turned out to be unreadable.
  int targetSpine = -1;
  uint32_t packagedBefore = 0;
  // Spine indices are absolute over the whole book, so the running count has to
  // start at the packaged records *ahead of* the range, not at zero. Only paid for
  // when this run actually aims the reader at one chapter.
  if (jobTargetOrdinal_ >= 0) {
    JjwxcStore::TocRecord ahead;
    for (uint32_t i = 0; i < fromOrdinal; ++i) {
      if (!toc.read(i, ahead)) break;
      if (JjwxcStore::isPackaged(ahead.state) &&
          Storage.exists(JjwxcStore::chapterPath(novelId_.c_str(), ahead.chapterId).c_str())) {
        ++packagedBefore;
      }
    }
  }
  // Chapters in the requested range that ended up with a real body, whether
  // fetched now or already on the card. Zero of them means the range produced
  // nothing but placeholders, which is what stops a "cache and read this chapter"
  // tap from opening a book that does not contain it.
  uint32_t readableInRange = 0;
  // Consecutive refusals carrying a code this library does not know. Bounded by
  // kUnknownRefusalLimit; a known refusal or a success resets it.
  uint32_t unknownRefusalStreak = 0;

  for (uint32_t i = fromOrdinal; i <= toOrdinal; ++i) {
    if (jobCancel_.load()) {
      jobStatus_ = jjwxc::Status::Cancelled;
      break;
    }
    // Every chapter starts at "requesting the body"; the VIP path advances this
    // to font/decode and the loop advances it to write below.
    dlStage_ = 0;
    JjwxcStore::TocRecord rec;
    if (!toc.read(i, rec)) {
      toc.close();
      storageFail();
      return;
    }
    const std::string path = JjwxcStore::chapterPath(novelId_.c_str(), rec.chapterId);
    const bool haveFile = Storage.exists(path.c_str());
    const auto state = static_cast<JjwxcStore::ChapterState>(rec.state);
    const bool wantsTarget = (i == fromOrdinal) && jobTargetOrdinal_ >= 0 && i == static_cast<uint32_t>(jobTargetOrdinal_);

    // RefreshCached: the user asked to refresh what is already readable. Leave
    // missing/unpurchased/locked chapters exactly as they are -- they were not
    // cached, and re-requesting them is what the other refresh modes are for.
    if (cacheMode_ == CacheMode::RefreshCached &&
        (state != JjwxcStore::ChapterState::Cached || !haveFile)) {
      ++skipped;
      if (JjwxcStore::isPackaged(rec.state)) ++packagedBefore;
      dlSkipped_ = skipped;
      dlDone_ = i - fromOrdinal + 1;
      continue;
    }
    // Incremental: a chapter whose file is on the card and whose remote size has
    // not changed is never refetched. This is the whole point of the chapter-level
    // cache, and the reason there is no "the artefact exists, so pull everything
    // again" path.
    if (cacheMode_ != CacheMode::Refresh && cacheMode_ != CacheMode::RefreshCached &&
        state == JjwxcStore::ChapterState::Cached && haveFile) {
      ++readableInRange;
      unknownRefusalStreak = 0;
      // The EPUB contains every toc chapter (packaged body or placeholder), so a
      // spine index is simply the toc ordinal. Counting only packaged chapters
      // here made "read this chapter" land on the first chapter.
      if (wantsTarget) targetSpine = static_cast<int>(i);
      ++packagedBefore;
      dlDone_ = i - fromOrdinal + 1;
      continue;
    }
    if (cacheMode_ == CacheMode::RetryBlocked && state != JjwxcStore::ChapterState::Unpurchased &&
        state != JjwxcStore::ChapterState::Locked) {
      // "Retry the chapters that were refused" must not re-download the ones that
      // were not, and must not touch a chapter that has never been attempted: that
      // is what the other two modes are for.
      ++skipped;
      if (JjwxcStore::isPackaged(rec.state)) ++packagedBefore;
      dlSkipped_ = skipped;
      dlDone_ = i - fromOrdinal + 1;
      continue;
    }
    if (state == JjwxcStore::ChapterState::Unpurchased || state == JjwxcStore::ChapterState::Locked) {
      // Already known to be unreadable: keep the stub, do not ask the server
      // again. Only a refresh or an explicit retry re-requests these.
      if (cacheMode_ == CacheMode::Incremental) {
        if (!haveFile) {
          const bool locked = state == JjwxcStore::ChapterState::Locked;
          if (!writeChapterStub(path, rec, locked)) {
            toc.close();
            storageFail();
            return;
          }
          ++stubs;
        }
        // A stub is packaged but not readable, so it still occupies a spine slot.
        ++packagedBefore;
        ++skipped;
        dlSkipped_ = skipped;
        dlDone_ = i - fromOrdinal + 1;
        continue;
      }
    }

    // One writer per chapter, on its own part file. fetchChapter streams the body
    // while the response is still being parsed, so a rejected chapter can emit
    // bytes before its `code` is known (JjwxcProtocol.h:373-378); aborting the
    // writer discards all of them and the stub below is composed from this
    // record alone. Nothing is ever rolled back into a shared file.
    JjwxcEpub::ChapterWriter writer;
    ChapterSinkCtx sink{&writer, false};
    jjwxc::ChapterMeta meta;
    // Two sources, chosen by the catalogue's own isVip flag. Free chapters stay on
    // app-cdn's chapterContent, which serves them anonymously as plaintext JSON with no
    // watermark at all (verified: PUA=0, zero-width=0, no `code` key) -- so cred_ being
    // empty is not a problem there, it is what selects the plaintext branch. Only VIP
    // bodies need the sid, and only they carry the AES envelope, the CSS displacement
    // and the per-session font watermark.
    //
    // This flag is also the refusal branch's answer to "could that request have spent
    // money", so it is named for the path it selects rather than for the chapter.
    const bool servedFromWap = rec.isVip != 0;
    jjwxc::Status s = jjwxc::Status::Network;
    // One retry for a transport-level read failure. Measured on the device: a
    // 200 response whose body read aborted (`SecureClient read failed: -397`,
    // `response incomplete`) killed a whole-book run at chapter 60; the same
    // chapter succeeded on the next manual run. Retrying the chapter once with
    // a fresh session keeps a one-off keep-alive/server hiccup from throwing
    // away the sixty chapters already cached. A dead radio still fails both
    // attempts, so this cannot turn a persistent outage into a long loop.
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (attempt > 0) {
        writer.abort();
        transport_.resetSession();
        LOG_INF(kTag, "chapter %lu network error; reconnecting and retrying once",
                static_cast<unsigned long>(rec.chapterId));
        vTaskDelay(pdMS_TO_TICKS(1200));
        if (jobCancel_.load()) {
          s = jjwxc::Status::Cancelled;
          break;
        }
      }
      if (!writer.begin(path, rec.title)) {
        toc.close();
        storageFail();
        return;
      }
      sink = ChapterSinkCtx{&writer, false};
      meta = jjwxc::ChapterMeta{};
      s = servedFromWap
              ? fetchWapVipChapter(novelId_.c_str(), rec.chapterId, meta, &JjwxcActivity::onChapterText, &sink)
              : jjwxc::fetchChapter(transport_, cred_, novelId_.c_str(), rec.chapterId, scratch, meta,
                                    &JjwxcActivity::onChapterText, &sink, nullptr, nullptr);
      if (s != jjwxc::Status::Network || jobCancel_.load()) break;
    }
    if (s == jjwxc::Status::Ok && !sink.failed && meta.chapterId != 0 && meta.chapterId != rec.chapterId) {
      // The reply says it served a different chapter than the one asked for. Discard the
      // body rather than write it: this is the check that would have caught the
      // scrolled-tap routing bug, which otherwise wrote an early free chapter's prose
      // into the book under a VIP chapter's title with no error anywhere.
      //
      // A run-level failure, not a per-chapter skip: a mismatch means our own selection
      // logic is wrong, so every remaining chapter would be wrong the same way, and
      // stubbing them would hide that behind a book full of placeholders.
      writer.abort();
      LOG_ERR(kTag, "chapter id mismatch: requested %lu, the reply served %lu",
              static_cast<unsigned long>(rec.chapterId), static_cast<unsigned long>(meta.chapterId));
      jobStatus_ = jjwxc::Status::Protocol;
      jobFailKind_ = FailKind::Protocol;
      toc.close();
      return;
    }
    if (s == jjwxc::Status::Ok && !sink.failed) {
      // Cross-check that costs nothing and makes a routing regression visible in
      // the log instead of in the book: the body's own chapter name must match the
      // catalogue entry it was requested for. Deliberately NOT a gate: an author
      // renaming a chapter after the catalogue was cached is legitimate, and refusing
      // the body would make that chapter permanently uncachable.
      if (meta.name[0] != '\0' && strcmp(meta.name, rec.title) != 0) {
        LOG_ERR(kTag, "chapter %lu body reports a different title: list='%s' body='%s'",
                static_cast<unsigned long>(rec.chapterId), rec.title, meta.name);
      }
      uint32_t bytes = 0;
      dlStage_ = 3;
      if (!writer.finish(bytes)) {
        toc.close();
        storageFail();
        return;
      }
      rec.state = static_cast<uint8_t>(JjwxcStore::ChapterState::Cached);
      rec.localBytes = bytes;
      rec.savedAt = now;
      if (meta.size != 0) rec.remoteSize = meta.size;
      if (!toc.update(i, rec)) {
        toc.close();
        storageFail();
        return;
      }
      if (i < chapterStates_.size()) chapterStates_[i] = rec.state;
      // One extra request per cached chapter feeds the reader's offline bubbles.
      // It is deliberately outside the status/error path: a comment failure must
      // never turn a successfully cached chapter into a failed one.
      // A single-chapter run also fetches the first page of every commented
      // paragraph so the reader's comment panel is offline on first open. A
      // range/whole-book run only fetches the counts: 100+ requests per chapter
      // would dominate an already long run.
      if (haveSession() && !jobCancel_.load()) {
        fetchAndStoreCommentCounts(rec.chapterId, fromOrdinal == toOrdinal);
      }
      ++written;
      ++readableInRange;
      unknownRefusalStreak = 0;
      if (wantsTarget) targetSpine = static_cast<int>(i);
      ++packagedBefore;
      dlBytes_ = dlBytes_.load() + bytes;
      // Polite pacing. JJWXC publishes no rate limit, but sustained machine-speed
      // fetching is exactly what its risk control looks for. Skipped chapters are
      // not requests and are not paced.
      if (i != toOrdinal) vTaskDelay(pdMS_TO_TICKS(kChapterDelayMs));
    } else {
      writer.abort();

      // Decide between "this chapter is not readable by this account" (skip it,
      // leave a placeholder, carry on) and "this run cannot continue" (stop now and
      // say why). Getting it wrong in the first direction costs the user a book
      // full of lies; in the second, it costs them the whole download.
      //
      // The bug this replaces: a chapter the account cannot read was only skipped
      // when the status was PurchaseRequired, i.e. code 70200 -- which belongs to
      // the `buy` endpoint and has never been seen from chapterContent. Every
      // refusal the service actually sends took the abort path instead, so a
      // whole-book cache died at the first unreadable chapter. Measured live on
      // novelId=<novelId> with a valid token, the refusals were 1013 (auto-
      // subscription failed, balance too low) and the STRING "1010" (already
      // subscribed / order in flight); statusFromCode() knew neither, so both fell
      // into its `default` arm and reached the screen as "晋江服务端错误". On
      // novelId=<novelId> the same situation arrives as 1004, which chapterContent
      // also uses for a dead token -- hence the probe.
      //
      // The rule itself is a pure function in the library so the host tests can pin
      // it; only the probe and the unknown-code streak are stateful.
      const bool vip = chapterLooksVip(rec);
      const jjwxc::TokenLiveness liveness =
          (meta.businessCode == 1004 && vip) ? tokenLiveness() : jjwxc::TokenLiveness::Alive;
      const jjwxc::RefusalVerdict verdict =
          jjwxc::classifyChapterRefusal(s, meta.businessCode, vip, liveness);
      jobDetailCode_ = meta.businessCode;

      const bool skipChapter = (verdict == jjwxc::RefusalVerdict::Unpurchased ||
                                verdict == jjwxc::RefusalVerdict::UnknownRefusal);
      if (verdict == jjwxc::RefusalVerdict::UnknownRefusal) {
        // An unknown code is treated as per-chapter because the last two surprises
        // (1013, 1010) were exactly that -- but bounded, so a rate limit or a
        // client rejection that happens to wear an unknown number cannot be stamped
        // into every remaining chapter of the book.
        if (++unknownRefusalStreak >= kUnknownRefusalLimit) {
          LOG_ERR(kTag, "%d consecutive chapters refused with unknown code %ld; treating it as a run-level failure",
                  (int)unknownRefusalStreak, meta.businessCode);
          jobStatus_ = s;
          break;
        }
      } else {
        unknownRefusalStreak = 0;
      }

      if (skipChapter) {
        // A chapter the catalogue already called author-locked keeps that state:
        // only a refresh re-asks the server about one, and the answer must not
        // silently reclassify it as a purchase problem.
        const bool locked = (state == JjwxcStore::ChapterState::Locked);
        if (!writeChapterStub(path, rec, locked)) {
          toc.close();
          storageFail();
          return;
        }
        rec.state = static_cast<uint8_t>(locked ? JjwxcStore::ChapterState::Locked
                                                : JjwxcStore::ChapterState::Unpurchased);
        rec.savedAt = now;
        rec.localBytes = 0;
        if (!toc.update(i, rec)) {
          toc.close();
          storageFail();
          return;
        }
        if (i < chapterStates_.size()) chapterStates_[i] = rec.state;
        // A stub is packaged but NOT readable: counting it as written is what made
        // a book of 114 real chapters and 72 stubs announce itself as fully cached.
        ++stubs;
        ++skipped;
        ++packagedBefore;
        dlSkipped_ = skipped;
        LOG_INF(kTag, "chapter %lu refused (code=%ld, %s); placeholder written, continuing",
                static_cast<unsigned long>(rec.chapterId), meta.businessCode, jjwxc::statusText(s));
        // Whether this refusal ends the run is a property of the PATH the chapter was
        // requested from, not of any user preference:
        //
        //   * WAP (/vip/<nid>/<cid>) answers an unpurchased VIP chapter with
        //     302 -> /buy/<nid>/<cid>. It declines to serve it and points at the
        //     purchase page; it cannot buy anything. So the placeholder written above
        //     is the whole story and the run carries on. Ending the run here instead
        //     made a selectively purchased book uncachable -- measured on a
        //     197-chapter book:
        //
        //       VIP chapter 20 is not purchased (https://m.jjwxc.net/buy/<novelId>/20)
        //       chapter 20 refused (code=0, 需要购买); placeholder written, continuing
        //       cached 0 new (20 readable, 1 stubs, 176 skipped), 21 in book
        //
        //     Twenty free chapters in, one stub, and 176 chapters never asked about --
        //     including the ones the account HAD bought -- which also made "重试未购买章"
        //     useless past the first gap, since it hit the same break.
        //   * chapterContent (the App API) runs the account's own server-side auto-
        //     subscription: requesting the chapter IS the purchase attempt (measured:
        //     code 1013 "余额不足，自动购买失败" and the string "1010" already
        //     subscribed). A purchase refusal on that path means the service just
        //     tried to charge the account, so the run stops rather than fire the same
        //     attempt at every remaining chapter. It is only reached for a chapter the
        //     catalogue called free, i.e. when that catalogue's isVip flag is stale.
        //
        // refusalEndsRun() is a pure function in the library so the host tests pin this
        // rule instead of a copy of it. The run still ends Ok: the stub above stands,
        // the book is packaged with everything already on the card, and the completion
        // summary says which chapter it stopped at.
        if (jjwxc::refusalEndsRun(verdict, servedFromWap)) {
          jobStoppedAtPurchase_ = true;
          jobStoppedOrdinal_ = i;
          skipped += toOrdinal - i;
          dlSkipped_ = skipped;
          dlDone_ = i - fromOrdinal + 1;
          LOG_INF(kTag, "purchase refusal on the App API path (code=%ld); ending the run at ordinal %lu",
                  meta.businessCode, static_cast<unsigned long>(i));
          break;
        }
      } else {
        // A dead session, an inconclusive probe, a client-level code, or a
        // transport/decrypt failure: not a property of this chapter. Stop here, keep
        // everything already cached, and let the user act on the real cause.
        const jjwxc::Status cause = (verdict == jjwxc::RefusalVerdict::Inconclusive) ? probeStatus_
                                    : (verdict == jjwxc::RefusalVerdict::NeedLogin)   ? jjwxc::Status::NeedLogin
                                                                                      : s;
        LOG_ERR(kTag, "chapter %lu failed: code=%ld %s (verdict=%d textBytes=%u)",
                static_cast<unsigned long>(rec.chapterId), meta.businessCode, jjwxc::statusText(cause),
                static_cast<int>(verdict), (unsigned)meta.textBytes);
        jobStatus_ = cause;
        break;
      }
    }
    dlDone_ = i - fromOrdinal + 1;
  }
  toc.close();

  // Nothing readable came out of the range. Recorded rather than acted on here:
  // the run may still be worth packaging (other chapters are already on the card),
  // but pollJob() must not then hand the reader a book that lacks the chapter the
  // user asked to read.
  jobRangeUnreadable_ = (readableInRange == 0);
  jobNewlyCached_ = written;
  jobTargetSpine_ = targetSpine;

  // The cover comes from a different host than the API, so it is fetched after
  // the last chapter rather than interleaved with them: one host switch instead of
  // two per chapter. Failure is not fatal -- a book without a cover still
  // packages, opens and reaches the shelf.
  bool coverChanged = false;
  if (!jobCancel_.load() && !Storage.exists(JjwxcStore::coverSourcePath(novelId_.c_str()).c_str())) {
    coverChanged = downloadCover();
    // The detail page draws a thumbnail, not the source JPEG, and this is the run that
    // just produced the source. Building it here costs one decode on a task that is
    // already off the UI thread, and saves the next visit to the page from having to.
    if (coverChanged) buildCoverThumb();
  }
  // The remaining phases are SD-only; holding a socket across them fragments the
  // heap (the same rule WeRead follows at Phase::PackageBook).
  transport_.resetSession();

  JjwxcStore::TocSummary after;
  uint32_t highest = 0;
  if (!JjwxcStore::readTocSummary(novelId_.c_str(), after, highest)) {
    after = before;
  }
  tocSummary_ = after;
  const JjwxcStore::NovelRecord rec = buildNovelRecord(after, after.count);
  dlPath_ = JjwxcStore::finalBookPath(rec);

  if (jobStatus_ != jjwxc::Status::Ok) {
    // Cancelled or failed: the chapters that reached the card stay cached and the
    // next run picks up from them. The metadata is still worth persisting so the
    // "Downloaded" list reports the right count.
    if (!JjwxcStore::saveNovel(rec)) LOG_ERR(kTag, "saveNovel failed for %s", rec.novelId);
    dlPath_.clear();
    return;
  }

  if (written == 0 && stubs == 0 && !coverChanged && Storage.exists(dlPath_.c_str())) {
    // Nothing changed and the artefact is already there: repackaging would only
    // cost SD bandwidth and throw away the reader's pagination for no gain. A new
    // stub counts as a change even though it added no readable chapter, because it
    // adds a spine entry.
    LOG_INF(kTag, "cache unchanged; keeping %s", dlPath_.c_str());
    if (!JjwxcStore::saveNovel(rec)) LOG_ERR(kTag, "saveNovel failed for %s", rec.novelId);
    return;
  }

  if (after.readable == 0) {
    // A book whose every entry would be a "[本章未购买]" stub is not a book: it
    // would take a shelf slot, a Recent entry and a thumbnail, and paging through
    // it would show nothing but refusals. Not an error status -- the summary the
    // message page is about to show explains it far better than "no content".
    LOG_ERR(kTag, "nothing readable to package: %lu packaged, %lu readable, %lu unpurchased",
            static_cast<unsigned long>(after.packaged), static_cast<unsigned long>(after.readable),
            static_cast<unsigned long>(after.unpurchased));
    dlPath_.clear();
    return;
  }

  dlPhase_ = static_cast<uint8_t>(JobPhase::Packaging);
  dlTotal_ = after.packaged;
  dlDone_ = 0;
  // The 4 KB copy buffer is a slice of the PSRAM scratch that held the last
  // response: packaging runs after the final fetchChapter, so the two never
  // overlap and no second buffer is allocated.
  const JjwxcEpub::Result packaged = JjwxcEpub::packageBook(rec, scratch_, kZipBufferBytes, contentKey_, contentKeyPath_,
                                                            &onPackageCancelled, this, &onPackageProgress, this);
  switch (packaged) {
    case JjwxcEpub::Result::Ok:
      break;
    case JjwxcEpub::Result::Cancelled:
      jobStatus_ = jjwxc::Status::Cancelled;
      dlPath_.clear();
      return;
    case JjwxcEpub::Result::Empty:
      jobStatus_ = jjwxc::Status::EmptyResult;
      dlPath_.clear();
      return;
    case JjwxcEpub::Result::Integrity:
    case JjwxcEpub::Result::StorageWrite:
      jobPackageFailed_ = true;
      jobStatus_ = jjwxc::Status::Protocol;
      jobFailKind_ = FailKind::StorageWrite;
      dlPath_.clear();
      return;
  }

  if (!JjwxcStore::saveNovel(rec)) {
    LOG_ERR(kTag, "saveNovel failed for %s", rec.novelId);
    storageFail();
    return;
  }
  invalidateReaderCache(dlPath_);
  // A new or changed book has to reach the shelf: LibraryBuilder only re-walks
  // the card when the dirty marker is set (LibraryBuilder.cpp:1046-1062). WeRead
  // never marked it, which is why its EPUBs only showed up once something else
  // happened to dirty the index.
  if (!library::markLibraryIndexDirty()) LOG_ERR(kTag, "cannot mark the library index dirty");
  LOG_INF(kTag, "cached %lu new (%lu readable, %lu stubs, %lu skipped), %lu in book: %s",
          static_cast<unsigned long>(written), static_cast<unsigned long>(after.readable),
          static_cast<unsigned long>(stubs), static_cast<unsigned long>(skipped),
          static_cast<unsigned long>(after.packaged), dlPath_.c_str());
}

// Launches a cache job for a closed range of 0-based toc ordinals.
void JjwxcActivity::startCaching(uint32_t fromOrdinal, uint32_t toOrdinal, const bool openAfter,
                                 const CacheMode mode) {
  if (tocSummary_.count == 0) {
    showMessage(tr(STR_JJWXC_NO_CATALOGUE), State::BookDetail);
    return;
  }
  if (fromOrdinal > toOrdinal) std::swap(fromOrdinal, toOrdinal);
  if (toOrdinal >= tocSummary_.count) toOrdinal = tocSummary_.count - 1;
  if (fromOrdinal > toOrdinal) {
    showMessage(tr(STR_JJWXC_RANGE_INVALID), State::DownloadRange);
    return;
  }
  // Consumed here rather than on leaving the range page. enter() only drops it when
  // the next state is not DownloadRange, so a confirmed refresh survived its own run:
  // the page title stayed "刷新缓存" afterwards and every later preset became an
  // unconfirmed full re-download, re-requesting every chapter the account had been
  // refused.
  refreshPending_ = false;
  // No token check here on purpose: scanning the range for a VIP chapter would
  // mean up to N SD reads inside an input handler. The job decides per chapter
  // instead -- an unpurchased VIP chapter becomes a placeholder and the run
  // continues, while a session the server rejects stops it with "sign in first"
  // (cacheChapterRange's refusal branch and jjwxc::probeTokenAlive).

  const State origin = state_ == State::ChapterList ? State::ChapterList : State::DownloadRange;
  withNetwork([this, fromOrdinal, toOrdinal, openAfter, mode, origin] {
    launchCacheJob(fromOrdinal, toOrdinal, openAfter, mode, origin);
  },
              origin);
}

void JjwxcActivity::launchCacheJob(const uint32_t fromOrdinal, const uint32_t toOrdinal, const bool openAfter,
                                   const CacheMode mode, const State origin) {
  // The EPUB is about to become device-encrypted. Prepare the raw key here, on
  // the UI task: bookkey::write goes through deviceSecret (NVS), and NVS from the
  // PSRAM-stack job would repeat the font-flash panic.
  const JjwxcStore::NovelRecord keyRec = buildNovelRecord(tocSummary_, tocSummary_.count);
  const std::string keyPath = JjwxcStore::finalBookPath(keyRec);
  if (!JjwxcStore::ensureBookContentKey(keyPath, contentKey_)) {
    showMessage(tr(STR_JJWXC_EPUB_FAILED), origin);
    return;
  }
  contentKeyPath_ = keyPath + ".key";

  dlTotal_ = toOrdinal - fromOrdinal + 1;
  dlDone_ = 0;
  dlBytes_ = 0;
  dlSkipped_ = 0;
  jobNewlyCached_ = 0;
  dlPhase_ = 0;
  dlStage_ = 0;
  lastProgressBucket_ = 0;
  lastProgressPhase_ = 0;
  lastProgressStage_ = 0;
  dlTitle_ = novel_.title[0] ? novel_.title : (info_.novelName[0] ? info_.novelName : novelId_);
  jobWhat_ = tr(STR_JJWXC_DOWNLOAD);
  jobNextState_ = origin;
  returnState_ = origin;
  busyText_ = tr(STR_JJWXC_DOWNLOADING);
  cacheMode_ = mode;
  jobWasCache_ = true;
  jobStoppedAtPurchase_ = false;
  jobStoppedOrdinal_ = 0;
  jobTargetSpine_ = -1;
  // Only a single-chapter run aims the reader at one place. A range opens wherever
  // the last session left off, which is what progress.bin is for.
  jobTargetOrdinal_ = (openAfter && fromOrdinal == toOrdinal) ? static_cast<int>(fromOrdinal) : -1;
  state_ = State::Downloading;
  infoLineCount_ = -1;
  detailLayoutValid_ = false;
  fullRefreshPending_ = true;
  requestUpdate();

  launchJob([this, fromOrdinal, toOrdinal] { cacheChapterRange(fromOrdinal, toOrdinal); },
            tr(STR_JJWXC_DOWNLOADING), openAfter);
}

void JjwxcActivity::cacheSingleChapter(const uint32_t ordinal, const bool openAfter) {
  // A single-chapter action is an explicit "fetch this chapter now": using
  // Incremental here made "缓存并阅读本章" a no-op on an already-cached chapter,
  // so edits made on the site never reached the card. Range/whole-book runs stay
  // incremental; only this explicit one-chapter entry refreshes.
  startCaching(ordinal, ordinal, openAfter, CacheMode::Refresh);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

int JjwxcActivity::listTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  return safe.y + metrics.headerHeight + metrics.verticalSpacing;
}

int JjwxcActivity::infoBlockHeight() const {
  // BookDetail's block is not a stack of equal lines any more -- it has a picture in
  // it -- so its height comes from the same cached layout the drawing uses.
  if (state_ == State::BookDetail) return detailLayout().height;
  if (state_ == State::AdvancedSearch) {
    // The unified search page's info band is the three mode chips, not text
    // lines. Its height comes from the same layout function that draws them and
    // that loop() hit-tests, so rowTop() (and therefore the row touch bands)
    // can never disagree with what is on screen.
    return searchChipLayout(nullptr, 0) + kInfoBlockGap;
  }
  if (state_ == State::Account && UITheme::getInstance().usesCpyLayout()) {
    // Frame 13's header half: the status card (2px ring + 24px padding + a
    // 56px mascot slot), the 账号 group label, and the double rule. This is
    // the sum of the constants renderCpyAccountHeader() draws with.
    constexpr int kCardH = 108;
    return 6 + kCardH + 14 + renderer.getLineHeight(UI_12_FONT_ID) + 6 + 5 + 6;
  }
  const int lines = infoLineCount();
  if (lines <= 0) return 0;
  return lines * renderer.getLineHeight(kSmallFont) + kInfoBlockGap;
}

int JjwxcActivity::searchChipLayout(Rect* out, const int maxOut) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  // CPY's design draws 64 px pills; everywhere else the chip grows with the UI
  // font but never drops below a 44 px touch target.
  int chipH = UITheme::getInstance().usesCpyLayout()
                  ? 64
                  : std::max(44, renderer.getLineHeight(kBodyFont) + 20);
  chipH = std::max(chipH, 24);
  const int left = safe.x + metrics.contentSidePadding;
  const int width = std::max(0, safe.width - 2 * metrics.contentSidePadding);
  // Rule #14: at least 6 px of visible space between adjacent controls.
  constexpr int kGap = 10;
  const int slot = width > 2 * kGap ? (width - 2 * kGap) / 3 : 0;
  const int y = listTop();
  for (int i = 0; i < 3; ++i) {
    const int x = left + i * (slot + kGap);
    // The last chip absorbs integer-division remainder, so the row of three ends
    // exactly on the content's right edge instead of leaving a stray gap.
    const int w = (i == 2) ? std::max(0, left + width - x) : slot;
    if (out != nullptr && i < maxOut) out[i] = Rect{x, y, w, chipH};
  }
  return chipH;
}

int JjwxcActivity::infoLineCount() const {
  if (infoLineCount_ < 0) {
    // Walks infoLine() once per state change instead of once per row per frame.
    // The probe buffer is deliberately tiny: infoLine() decides whether a line
    // exists from the source field, not from how much of it fitted.
    char probe[8];
    int count = 0;
    while (infoLine(count, probe, sizeof(probe))) ++count;
    infoLineCount_ = count;
  }
  return infoLineCount_;
}

bool JjwxcActivity::infoLine(const int index, char* buf, const size_t bufSize) const {
  if (buf != nullptr && bufSize > 0) buf[0] = '\0';
  if (index < 0) return false;

  char line[kInfoLineBytes];
  // Only ever holds a formatted integer; the text fields are passed straight to
  // compose() from NovelInfo's own fixed buffers.
  char value[24];
  line[0] = '\0';
  int n = 0;

  // "label  value", or an empty line when the field is absent, so a
  // novelbasicinfo response that omits a field never shows a dangling label.
  // Two spaces rather than punctuation: the separator has to read the same in
  // every language and cannot itself be a translated string.
  const auto compose = [&line](const StrId label, const char* text) {
    if (text == nullptr || text[0] == '\0') {
      line[0] = '\0';
      return;
    }
    snprintf(line, sizeof(line), "%s  %s", I18N.get(label), text);
  };
  // One walk answers both "how many lines are there" (infoLineCount, and so the
  // block height, and so rowTop()/visibleRows()) and "what is line i"
  // (renderInfoBlock). Drawing and layout therefore cannot drift apart, which is
  // what AGENTS.md rule #14 requires of anything the rows below are measured
  // against.
  const auto emit = [&n, &line, index, buf, bufSize]() {
    if (line[0] == '\0') return false;
    if (n++ != index) return false;
    if (buf != nullptr && bufSize > 0) snprintf(buf, bufSize, "%s", line);
    return true;
  };

  switch (state_) {
    case State::BookDetail:
      // Not here any more. The detail block's facts wrap (a ten-tag list is unreadable
      // clipped to one line), and a wrapped fact is not one line, so this state composes
      // its facts in composeDetailFact() and lays them out in buildDetailFacts() -- both
      // driven by detailLayout(), which is still the one geometry source the drawing and
      // the row hit-test share.
      return false;

    case State::DownloadRange:
      snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(tocSummary_.count));
      compose(StrId::STR_JJWXC_DETAIL_CHAPTERS, value);
      if (emit()) return true;
      // readable/total, never packaged/total: a "[本章未购买]" stub is in the book
      // but cannot be read, and reporting it as cached is what made a book of 114
      // real chapters and 72 stubs claim to be complete.
      snprintf(value, sizeof(value), "%lu/%lu", static_cast<unsigned long>(tocSummary_.readable),
               static_cast<unsigned long>(tocSummary_.count));
      compose(StrId::STR_JJWXC_DETAIL_CACHED, value);
      if (emit()) return true;
      if (tocSummary_.unpurchased + tocSummary_.locked > 0) {
        snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(tocSummary_.unpurchased + tocSummary_.locked));
        compose(StrId::STR_JJWXC_DETAIL_BLOCKED, value);
        if (emit()) return true;
      }
      return false;

    case State::ChapterList: {
      snprintf(value, sizeof(value), "%lu/%lu", static_cast<unsigned long>(tocSummary_.readable),
               static_cast<unsigned long>(tocSummary_.count));
      compose(StrId::STR_JJWXC_DETAIL_CACHED, value);
      if (emit()) return true;
      if (tocSummary_.unpurchased + tocSummary_.locked > 0) {
        snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(tocSummary_.unpurchased + tocSummary_.locked));
        compose(StrId::STR_JJWXC_DETAIL_BLOCKED, value);
        if (emit()) return true;
      }
      if (chapters_.empty()) return false;
      const int last = static_cast<int>(chapters_.size()) - 1;
      const bool hasRange = rangeFrom_ >= 0 && rangeTo_ >= 0;
      const int from = std::clamp(hasRange ? rangeFrom_ : 0, 0, last);
      const int to = std::clamp(hasRange ? rangeTo_ : 0, 0, last);
      // The endpoints live in this block now; the one-line header subtitle is
      // used for the action hint instead. Ordinals (+1), not chapterIds: every
      // other place a range is entered or shown -- the custom-range row, both
      // keyboard prompts and startCaching() itself -- speaks ordinals, and the two
      // diverge as soon as the server skips or renumbers a chapter.
      snprintf(line, sizeof(line), "%s  %d    %s  %d", tr(STR_JJWXC_FROM), from + 1, tr(STR_JJWXC_TO), to + 1);
      if (emit()) return true;
      return false;
    }

    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// The book-detail block: cover, wrapped facts beside it, wrapped blurb under it
// ---------------------------------------------------------------------------

const char* JjwxcActivity::blurbText() const { return intro_.empty() ? info_.introShort : intro_.c_str(); }

size_t JjwxcActivity::blurbLen() const { return intro_.empty() ? strlen(info_.introShort) : intro_.size(); }

bool JjwxcActivity::composeDetailFact(const int index, char* buf, const size_t bufSize,
                                     jjwxc::WrapField* field) const {
  if (field != nullptr) *field = jjwxc::WrapField::Plain;
  if (buf == nullptr || bufSize == 0 || index < 0) return false;
  buf[0] = '\0';

  char line[kInfoLineBytes];
  // Only ever holds a formatted integer; the text fields are passed straight to
  // compose() from NovelInfo's own fixed buffers.
  char value[24];
  line[0] = '\0';
  int n = 0;
  // How the fact being composed may be broken. Reported through `field` rather than
  // derived from `index` downstream: which facts exist depends on the response (a book
  // with nothing blocked has no "blocked" line), so the tags are not at a fixed index.
  jjwxc::WrapField want = jjwxc::WrapField::Plain;

  // "label  value", or an empty line when the field is absent, so a novelbasicinfo
  // response that omits a field never shows a dangling label. Two spaces rather than
  // punctuation: the separator has to read the same in every language and cannot itself
  // be a translated string.
  const auto compose = [&line, &want](const StrId label, const char* text,
                                      const jjwxc::WrapField f = jjwxc::WrapField::Plain) {
    want = f;
    if (text == nullptr || text[0] == '\0') {
      line[0] = '\0';
      return;
    }
    snprintf(line, sizeof(line), "%s  %s", I18N.get(label), text);
  };
  const auto emit = [&]() -> bool {
    if (line[0] == '\0') return false;
    if (n++ != index) return false;
    snprintf(buf, bufSize, "%s", line);
    if (field != nullptr) *field = want;
    return true;
  };

  // The CPY hero already shows the author above the band; repeating it as a
  // prose fact is the old text wall.
  if (!UITheme::getInstance().usesCpyLayout()) {
    compose(StrId::STR_JJWXC_DETAIL_AUTHOR, info_.authorName[0] ? info_.authorName : novel_.author);
    if (emit()) return true;
  }
  // A category path ("原创-纯爱-近代现代-爱情") clipped to one line loses its tail, so it
  // breaks at its dashes instead. See JjwxcTextWrap.h for the rule and the host test
  // that pins it.
  compose(StrId::STR_JJWXC_DETAIL_CLASS, info_.novelClass, jjwxc::WrapField::Category);
  if (emit()) return true;

  // Chapters / cached / blocked live in the data band above these lines, so
  // repeating them as prose would say the same three numbers twice.
  compose(StrId::STR_JJWXC_DETAIL_SIZE, info_.sizeText);
  if (emit()) return true;

  if (info_.vipChapterId != 0) {
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(info_.vipChapterId));
    compose(StrId::STR_JJWXC_DETAIL_VIP_FROM, value);
  } else if (info_.chapterCount != 0) {
    want = jjwxc::WrapField::Plain;
    snprintf(line, sizeof(line), "%s", tr(STR_JJWXC_DETAIL_ALL_FREE));
  }
  if (emit()) return true;

  compose(StrId::STR_JJWXC_DETAIL_LATEST, info_.lastChapterName);
  if (emit()) return true;
  compose(StrId::STR_JJWXC_DETAIL_UPDATED, info_.renewDate);
  if (emit()) return true;
  // A ten-tag list is exactly as unreadable clipped as the category path was, and for
  // the same reason: it is a list, not a sentence. Broken at its separators so no tag is
  // ever split in half.
  compose(StrId::STR_JJWXC_DETAIL_TAGS, info_.novelTags, jjwxc::WrapField::Tags);
  if (emit()) return true;
  // The blurb is deliberately NOT one of these facts. Clipping novelIntro to a single
  // 192-byte row is what made the detail page read as empty: the field is ~930
  // characters for a normal book and one line of it is a teaser with the end cut off. It
  // is wrapped over as many lines as fit under this block instead -- see detailLayout()
  // and renderBookDetail(), which share one geometry source.
  return false;
}

void JjwxcActivity::buildDetailFacts(const int wrapWidth) const {
  factText_.clear();
  factLines_.clear();
  const auto measure = [this](const char* s, const size_t n) { return measureTextWidth(s, n, kSmallFont); };
  char buf[kInfoLineBytes];
  for (int i = 0;; ++i) {
    jjwxc::WrapField field = jjwxc::WrapField::Plain;
    if (!composeDetailFact(i, buf, sizeof(buf), &field)) break;
    const size_t textLen = strlen(buf);
    if (textLen == 0) continue;
    if (field == jjwxc::WrapField::Plain || wrapWidth <= 0) {
      // One line, clipped to the column exactly as before this page could wrap.
      if (wrapWidth > 0) truncateToWidth(renderer, kSmallFont, buf, wrapWidth);
      const size_t keep = std::min(strlen(buf), static_cast<size_t>(0xFFFF));
      if (keep == 0) continue;
      const auto offset = static_cast<uint32_t>(factText_.size());
      factText_.append(buf, keep);
      factLines_.push_back({offset, static_cast<uint16_t>(keep)});
      continue;
    }
    // Wrapped: each line's own bytes are appended, minus the separators the break
    // consumed, so a continuation line never starts with a dangling space. The offsets
    // therefore index factText_, not the composed line -- which is what lets the drawing
    // loop copy a line out without composing or measuring anything.
    for (size_t pos = 0; pos < textLen;) {
      const size_t n = jjwxc::wrapLineLen(buf, textLen, pos, wrapWidth, field, measure);
      if (n == 0) break;
      size_t keep = std::min(n, textLen - pos);
      while (keep > 0 && (buf[pos + keep - 1] == ' ' || buf[pos + keep - 1] == '\t')) --keep;
      if (keep > 0 && keep <= 0xFFFF) {
        const auto offset = static_cast<uint32_t>(factText_.size());
        factText_.append(buf + pos, keep);
        factLines_.push_back({offset, static_cast<uint16_t>(keep)});
      }
      pos += n;
    }
  }
}

bool JjwxcActivity::detailFactLine(const int index, char* buf, const size_t bufSize) const {
  if (buf == nullptr || bufSize == 0) return false;
  buf[0] = '\0';
  if (index < 0 || static_cast<size_t>(index) >= factLines_.size()) return false;
  const auto& wl = factLines_[static_cast<size_t>(index)];
  if (wl.offset >= factText_.size()) return false;
  const size_t avail = factText_.size() - wl.offset;
  copyWrappedLine(buf, bufSize, factText_.data() + wl.offset, std::min<size_t>(wl.bytes, avail));
  return true;
}

int JjwxcActivity::measureTextWidth(const char* text, const size_t len, const int fontId) const {
  if (text == nullptr || len == 0) return 0;
  int width = 0;
  char glyph[8];
  for (size_t pos = 0; pos < len;) {
    const size_t n = jjwxc::utf8CharLen(text, len, pos);
    if (n == 0 || n >= sizeof(glyph)) break;  // not a UTF-8 lead: stop rather than guess
    memcpy(glyph, text + pos, n);
    glyph[n] = '\0';
    width += renderer.getTextAdvanceX(fontId, glyph, EpdFontFamily::REGULAR);
    pos += n;
  }
  return width;
}

size_t JjwxcActivity::measureWrappedLine(const char* text, const size_t textLen, const size_t start, const int fontId,
                                        const int maxWidth) const {
  // One greedy walk, shared by the counting pass and the drawing pass, so the block
  // height and what actually appears cannot disagree. The rules live in JjwxcTextWrap.h
  // (pure, host-tested); this only supplies the renderer's glyph advances.
  return jjwxc::wrapLineLen(text, textLen, start, maxWidth, jjwxc::WrapField::Plain,
                            [this, fontId](const char* s, const size_t n) { return measureTextWidth(s, n, fontId); });
}

const JjwxcActivity::DetailLayout& JjwxcActivity::detailLayout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int contentWidth = std::max(0, safe.width - 2 * metrics.contentSidePadding);
  const size_t blurbBytes = blurbLen();
  // Recomputed on a state change, on a content-width change, on the expand/collapse
  // toggle and when the blurb itself changed -- never per frame, because counting
  // wrapped lines measures every code point against the font.
  if (detailLayoutValid_ && detailLayout_.contentWidth == contentWidth &&
      detailLayout_.expanded == blurbExpanded_ && blurbLinesForLen_ == blurbBytes)
    return detailLayout_;

  DetailLayout L;
  L.contentWidth = contentWidth;
  L.expanded = blurbExpanded_;
  const int factLineH = renderer.getLineHeight(kSmallFont);
  const int blurbLineH = renderer.getLineHeight(kListFont);
  // Same bottom edge visibleRows() uses, so the block and the action rows below it are
  // measured against each other rather than against the raw panel.
  const int available = detailViewportHeight();
  // The rows are the point of the page: a blurb that pushed them off the panel would be
  // both unreadable and unreachable, since the hit-test below would report a viewport
  // with nothing in it. Collapsed, the block therefore gets only what is left once the
  // first kDetailRowsReserve rows are paid for; expanded, it gets the whole viewport and
  // the page scrolls instead (maxDetailScroll()).
  const int rowsReserve = blurbExpanded_
                              ? 0
                              : std::min(static_cast<int>(rows_.size()), kDetailRowsReserve) * rowHeight();
  // Everything the block may cost, kInfoBlockGap included, so that
  // L.height + rowsReserve <= available holds by construction. That inequality is the
  // whole guarantee the collapsed page rests on: rowTop() = listTop() + L.height, so the
  // reserved rows end at rowTop() + rowsReserve <= viewportBottom.
  const int budget = std::max(0, available - rowsReserve - kInfoBlockGap);

  // Cover slot. Reserved when a thumbnail is open OR when a cover URL is known -- the
  // second case draws the theme's placeholder, so an offline book and a book whose
  // download failed keep the same shape instead of collapsing the layout.
  const bool wantCover =
      coverBitmap_ != nullptr || info_.coverUrl[0] != '\0' || novel_.coverUrl[0] != '\0';
  if (wantCover && contentWidth > 0) {
    int coverW = std::min(kCoverThumbWidth, contentWidth * 40 / 100);
    // refreshed UI frame 03: a 150 px cover leaves room for the title block on the
    // right. The 200 px thumbnail is drawn scaled down into this slot.
    if (UITheme::getInstance().usesCpyLayout()) coverW = std::min(coverW, 150);
    int coverH = coverW * 7 / 5;  // the service's own 300x420 aspect
    // Never taller than what is left above the rows once the blurb's heading and its gap
    // are paid for.
    const int maxH = std::max(0, budget - blurbLineH - kBlurbGap);
    if (coverH > maxH) {
      coverH = maxH;
      coverW = coverH * 5 / 7;
    }
    if (coverW >= kCoverMinWidth && coverH >= kCoverMinWidth) {
      L.coverW = coverW;
      L.coverH = coverH;
    }
  }
  L.factX = L.coverW > 0 ? L.coverW + kCoverGap : 0;
  L.factWidth = std::max(0, contentWidth - L.factX);
  L.heroH = UITheme::getInstance().usesCpyLayout() && L.factWidth > 0 ? 112 : 0;
  // Design frame 11: three at-a-glance numbers sit above the fact lines. They are
  // always derivable from the toc (total / cached / blocked), so the band exists
  // even for an offline book whose novelbasicinfo never arrived.
  L.bandH = L.factWidth > 0 ? kDetailBandH : 0;
  const int factBudget = std::max(0, budget - L.heroH - (L.bandH > 0 ? L.bandH + kDetailBandGap : 0));

  // The facts, wrapped at the column width. Built before the height is settled because
  // the height IS the wrapped line count; nothing above depends on that count, so there
  // is no cycle to break.
  buildDetailFacts(L.factWidth);
  const int factTotal = static_cast<int>(factLines_.size());
  int factLines = factTotal;
  // A wrapped fact column can be taller than the whole panel -- sixteen lines for a book
  // with a long category path, a long latest-chapter name and ten tags -- and the action
  // rows still have to be on the first screen. Collapsed, the column is therefore cut
  // back to the budget and factCut says so; the expand row is what brings the rest back,
  // so nothing is lost, only deferred.
  if (!blurbExpanded_ && factLineH > 0 && factTotal * factLineH > factBudget) {
    factLines = std::max(0, factBudget / factLineH);
    L.factCut = factLines < factTotal;
  }
  L.factLines = factLines;

  // The top band is whichever of the picture and the fact column is taller; the two
  // sit side by side, so the shorter one simply ends early.
  int y = std::max(L.coverH, L.heroH + (L.bandH > 0 ? L.bandH + kDetailBandGap : 0) + factLines * factLineH);

  L.blurbX = 0;
  L.blurbWidth = contentWidth;
  blurbLines_.clear();
  blurbLinesForLen_ = blurbBytes;
  // CPY (frame 12): the blurb lives BELOW the pill band, so it never competes
  // with the actions for the first screen -- it is not part of this block's
  // height at all, and nothing about it is ever cut (the page scrolls).
  const bool cpy = UITheme::getInstance().usesCpyLayout();
  if (blurbBytes > 0 && L.blurbWidth > 0) {
    // Collapsed: only as many wrapped lines as still leave the reserved rows inside the
    // viewport, which can legitimately be zero. Expanded: all of them, and the page
    // scrolls. The heading is not free, so it comes out of the room first.
    const int room = budget - y - kBlurbGap - blurbLineH;
    const int linesThatFit = room > 0 && blurbLineH > 0 ? room / blurbLineH : 0;
    const int maxLines =
        cpy ? kMaxBlurbLines : std::clamp(blurbExpanded_ ? kMaxBlurbLines : linesThatFit, 0, kMaxBlurbLines);
    if (maxLines > 0) {
      if (!cpy) {
        y += kBlurbGap;
        L.blurbY = y;
        y += blurbLineH;  // the heading
      }
      const char* text = blurbText();
      size_t pos = 0;
      // Stops at the budget: one line short of the whole paragraph, which is enough to
      // know the text was cut without measuring a 900-character blurb that cannot be
      // shown collapsed anyway.
      while (pos < blurbBytes && static_cast<int>(blurbLines_.size()) < maxLines) {
        const size_t n = measureWrappedLine(text, blurbBytes, pos, kListFont, L.blurbWidth);
        if (n == 0) break;
        blurbLines_.push_back({static_cast<uint32_t>(pos), static_cast<uint16_t>(std::min<size_t>(n, 0xFFFF))});
        pos += n;
      }
      L.blurbLines = static_cast<int>(blurbLines_.size());
      L.blurbCut = !cpy && pos < blurbBytes;
      if (!cpy) y += L.blurbLines * blurbLineH;
    }
  }

  // Nothing at all to show -- no metadata, no cover, no blurb, which is what an
  // offline book that was never catalogued looks like -- must cost no height, exactly
  // as the old line-based block did, or the action rows would sit 12 px lower for no
  // reason.
  const int content = std::max(y, L.coverH);
  L.height = content > 0 ? content + kInfoBlockGap : 0;
  detailLayout_ = L;
  detailLayoutValid_ = true;
  return detailLayout_;
}

int JjwxcActivity::blockTop() const { return listTop() - (state_ == State::BookDetail ? detailScroll_ : 0); }

int JjwxcActivity::rowTop() const { return blockTop() + infoBlockHeight(); }

int JjwxcActivity::rowHeight() const {
  // CPY's list pages are all the design's one-line rows (frame 14's .lrow,
  // frame 13's .row: both 96px) -- every state that draws its rows at that
  // rhythm must report the height here, or the rows silently fall back to the
  // old two-line rhythm. AdvancedSearch keeps the rhythm through its own
  // renderSearchRows(). BookDetail's "rows" are frame 12's pill band, whose
  // height comes from the pill layout itself. The smaller kMaxRows-era cap is
  // unaffected -- this only changes how tall each of the same rows is drawn and
  // hit.
  if (UITheme::getInstance().usesCpyLayout()) {
    if (state_ == State::BookDetail) return cpyDetailPillLayout(0, nullptr, 0);
    if (state_ == State::ChapterList || state_ == State::Account || state_ == State::Menu ||
        state_ == State::Shelf || state_ == State::Library || state_ == State::SearchResults ||
        state_ == State::DiscoverList || state_ == State::CommentList || state_ == State::DownloadRange ||
        state_ == State::AdvancedSearch) {
      return 96;
    }
  }
  return renderer.getLineHeight(kListFont) + renderer.getLineHeight(kSmallFont) + 10;
}

int JjwxcActivity::detailViewportBottom() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  // The theme's safe area is already inside the panel; the panel height the renderer
  // reports is the hard bound (rule #8 -- never a literal panel size in an activity).
  return std::min(safe.y + safe.height - metrics.verticalSpacing, renderer.getScreenHeight());
}

int JjwxcActivity::detailViewportHeight() const { return std::max(0, detailViewportBottom() - listTop()); }

int JjwxcActivity::visibleRows() const {
  const int bottom = detailViewportBottom();
  const int h = rowHeight();
  return h > 0 ? std::max(1, (bottom - rowTop()) / h) : 1;
}

int JjwxcActivity::maxDetailScroll() const {
  if (state_ != State::BookDetail) return 0;
  const int blockH = infoBlockHeight();
  const int rowsH = static_cast<int>(rows_.size()) * rowHeight();
  // CPY (frame 12): the blurb sits below the pill band and is the page's tail,
  // so it is what the extra scroll range is for. The pill hit-test guards
  // against taps above listTop when the band has scrolled up; the block-height
  // cap below exists for the default layout's geometry assumptions.
  int below = 0;
  const bool cpy = UITheme::getInstance().usesCpyLayout();
  if (cpy && blurbLen() > 0 && detailLayout().blurbLines > 0) {
    const int blurbLineH = renderer.getLineHeight(kListFont);
    below = kBlurbGap + blurbLineH * (1 + detailLayout().blurbLines) + kCpyBlurbRuleBlock;
  }
  const int overflow = blockH + rowsH + below - detailViewportHeight();
  if (overflow <= 0) return 0;
  if (cpy) return overflow;
  // Never further than the block's own height. rowTop() = listTop() - detailScroll_ +
  // blockH then stays >= listTop(), which is what keeps renderRows()' clip, its scroll
  // hint and rowTouch()'s "a tap above `top` is not a row" on the geometry they were
  // written for: the rows can be pushed BELOW the viewport, never above it.
  return std::min(blockH, overflow);
}

void JjwxcActivity::clampDetailScroll() { detailScroll_ = std::clamp(detailScroll_, 0, maxDetailScroll()); }

int JjwxcActivity::detailPageSize() const {
  // A page of the content viewport, less one line of text so the reader keeps a line of
  // context across the turn. Never smaller than one row, or a page gesture would crawl.
  return std::max(rowHeight(), detailViewportHeight() - renderer.getLineHeight(kListFont));
}

bool JjwxcActivity::scrollDetailByPage(const int pages) {
  if (pages == 0) return false;
  const int maxScroll = maxDetailScroll();
  if (maxScroll <= 0) return false;
  const int next = std::clamp(detailScroll_ + pages * detailPageSize(), 0, maxScroll);
  if (next == detailScroll_) return false;  // already at that end: let the rows page
  detailScroll_ = next;
  requestUpdate();
  return true;
}

void JjwxcActivity::ensureDetailRowVisible() {
  const int h = rowHeight();
  if (h <= 0 || selected_ < 0 || static_cast<size_t>(selected_) >= rows_.size()) return;
  // scroll_ is 0 in this state, so row i sits at rowTop() + i * h.
  const int y = rowTop() + selected_ * h;
  const int bottom = detailViewportBottom();
  if (y < listTop()) {
    detailScroll_ -= listTop() - y;
  } else if (y + h > bottom) {
    detailScroll_ += y + h - bottom;
  }
  clampDetailScroll();
}

void JjwxcActivity::ensureVisible() {
  if (state_ == State::BookDetail) {
    // One scroll for the whole page. The rows are laid out from rowTop() in order and the
    // PAGE moves to follow the selection, so the row window stays at 0 and cannot
    // disagree with the block above it -- which is what two independent scrolls would do.
    scroll_ = 0;
    ensureDetailRowVisible();
    return;
  }
  const int vis = visibleRows();
  if (selected_ < scroll_) scroll_ = selected_;
  else if (selected_ >= scroll_ + vis) scroll_ = selected_ - vis + 1;
  if (scroll_ < 0) scroll_ = 0;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void JjwxcActivity::renderHeader(const char* title, const char* subtitle) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const Rect header{safe.x, safe.y, safe.width, metrics.headerHeight};
  GUI.drawHeader(renderer, header, title);
  if (subtitle && subtitle[0]) {
    renderer.drawText(kSmallFont, safe.x + metrics.contentSidePadding,
                      header.y + header.height - renderer.getLineHeight(kSmallFont), subtitle, true);
  }
}

void JjwxcActivity::renderRows() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int x = safe.x + metrics.contentSidePadding;
  const int h = rowHeight();
  const int viewportBottom = detailViewportBottom();
  // BookDetail scrolls its whole page, so an expanded blurb can push the row viewport
  // off the bottom entirely. There is then nothing to draw, nothing to hit-test and
  // nothing wrong: this is a state, not a fault, and both the empty-list message and the
  // geometry warning below would be noise (or worse, off-screen drawing).
  if (rowTop() >= viewportBottom) return;
  if (rows_.empty()) {
    renderer.drawCenteredText(kBodyFont, rowTop() + rowHeight(), tr(STR_JJWXC_EMPTY));
    return;
  }
  // The unified search page has its own row language (label/value rows plus the
  // solid "start search" pill) and must not fall through to the generic/CPY
  // renderers, which would paint the pill row as plain text.
  if (state_ == State::AdvancedSearch) {
    renderSearchRows();
    return;
  }
  if (UITheme::getInstance().usesCpyLayout() && state_ == State::ChapterList) {
    renderCpyChapterRows();
    return;
  }
  if (UITheme::getInstance().usesCpyLayout() && state_ == State::Account) {
    renderCpyPlainRows(true);
    return;
  }
  if (UITheme::getInstance().usesCpyLayout() && state_ == State::BookDetail) {
    renderCpyDetailPills();
    return;
  }
  if (UITheme::getInstance().usesCpyLayout()) {
    // The remaining app lists -- menu, shelf, library, search, range presets
    // -- take the same row language; short action menus ride in the card.
    renderCpyPlainRows(state_ == State::Menu || state_ == State::DownloadRange);
    return;
  }
  const int vis = visibleRows();
  const int last = std::min(scroll_ + vis, static_cast<int>(rows_.size()));

  // Clipped to the list viewport. The device log showed ~20,000 "GFX !! Outside range"
  // errors in under three seconds -- a row band drawn from y=1259 to y=3045 on a 1216-px
  // screen, at ~162 px spacing, which is rowHeight(). Every one of those is a wasted
  // per-pixel bounds check and a log line that buries the errors worth reading, and on an
  // e-ink device the frame that produced them took 1393 ms.
  //
  // The clip contains it; it does not explain it. Nothing in this function should be able
  // to produce those coordinates -- `last` is bounded by rows_.size() and `y` by vis -- so
  // one of rowTop(), rowHeight() or visibleRows() returned something impossible, and the
  // geometry dump below is what identifies which. ClipScope is the same tool three other
  // activities in this repo use for exactly this symptom.
  const GfxRenderer::ClipScope clip(renderer, safe.x, rowTop(), safe.width,
                                    viewportBottom > rowTop() ? viewportBottom - rowTop() : 0);
  // `vis` has a floor of 1, so a page that leaves room for LESS than one row (an expanded
  // blurb, or a fact column that had to be cut) satisfies rowTop() + vis*h > bottom while
  // being perfectly sane. Only the impossible cases are reported: a per-frame LOG_ERR that
  // fires on a legitimate state is exactly the log flood this clip exists to stop.
  const int roomBelow = viewportBottom - rowTop();
  if (scroll_ < 0 || vis <= 0 || (roomBelow >= h && rowTop() + vis * h > viewportBottom)) {
    LOG_ERR(kTag, "list geometry is off-screen: rowTop=%d rowH=%d vis=%d scroll=%d rows=%u last=%d bottom=%d",
            rowTop(), h, vis, scroll_, (unsigned)rows_.size(), last, viewportBottom);
  }

  for (int i = scroll_; i < last; ++i) {
    const int y = rowTop() + (i - scroll_) * h;
    const Rect rowRect{safe.x, y, safe.width, h};
    const bool selected = (i == selected_);
    bool foregroundBlack = true;
    if (selected) foregroundBlack = GUI.drawSelectionBackground(renderer, rowRect);

    const int textY = y + 5;
    renderer.drawText(kListFont, x, textY, rows_[i].title.c_str(), foregroundBlack,
                      selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (!rows_[i].subtitle.empty()) {
      renderer.drawText(kSmallFont, x, textY + renderer.getLineHeight(kListFont) + 2, rows_[i].subtitle.c_str(),
                        foregroundBlack);
    }
  }

  // Scroll hints, since a list can be far longer than one screen.
  if (scroll_ > 0) renderer.drawText(kSmallFont, x, rowTop() - renderer.getLineHeight(kSmallFont), "▲", true);
  if (last < static_cast<int>(rows_.size())) {
    char hint[48];
    snprintf(hint, sizeof(hint), "▼ %d/%zu", selected_ + 1, rows_.size());
    // Clamped into the viewport. When the list fills the screen, `last - scroll_ == vis`
    // and this lands one whole row BELOW the last visible row, i.e. off the bottom --
    // which is where a large share of those Outside range errors came from. It now sits
    // in the leftover band under the last row, or over that row's empty lower half when
    // there is no band, which is readable and on-screen either way.
    const int lineH = renderer.getLineHeight(kSmallFont);
    const int maxY = viewportBottom - lineH;
    int hintY = rowTop() + (last - scroll_) * h + 2;
    if (hintY > maxY) hintY = maxY;
    if (hintY < rowTop()) hintY = rowTop();
    renderer.drawText(kSmallFont, x, hintY, hint, true);
  }
}

// The unified search page's rows. Every row but the last is the app's usual
// label-left / value-right line with the theme's selection band; the last row
// ("开始搜索") is painted as a solid rounded pill across its band, so the action
// reads as a button rather than as one more form field.
//
// It is still an ordinary row in rows_/rowIds_: rowHeight() gives the pill band
// its height, rowTouch() resolves a tap anywhere in that band to `advrun`, the
// keyboard can select it, and onListConfirm() runs the search. Only the drawing
// differs -- there is no second hit-test to drift from the button (rule #14).
void JjwxcActivity::renderSearchRows() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int x = safe.x + metrics.contentSidePadding;
  const int right = safe.x + safe.width - metrics.contentSidePadding;
  const int top = rowTop();
  const int bottom = detailViewportBottom();
  const int h = rowHeight();
  const int n = static_cast<int>(rows_.size());
  if (n <= 0 || h <= 0 || top >= bottom) return;

  const int titleLine = renderer.getLineHeight(kListFont);
  const int smallLine = renderer.getLineHeight(kSmallFont);
  const GfxRenderer::ClipScope clip(renderer, safe.x, top, safe.width, bottom - top);
  const int last = std::min(scroll_ + visibleRows(), n);
  const int contentW = std::max(0, safe.width - 2 * metrics.contentSidePadding);

  for (int i = scroll_; i < last; ++i) {
    const int y = top + (i - scroll_) * h;
    const bool isRun = static_cast<size_t>(i) < rowIds_.size() && rowIds_[i] == "advrun";
    if (isRun) {
      // Solid primary button, centred in the row band. Width is a share of the
      // content column (capped so it does not become a full-width bar on a wide
      // panel); height is the design's 64 px pill on CPY and a font-derived pill
      // elsewhere, always inside the band with air above and below.
      int pillH = std::min(h - 14, UITheme::getInstance().usesCpyLayout()
                                      ? 64
                                      : std::max(44, renderer.getLineHeight(kBodyFont) + 20));
      if (pillH < 24) pillH = std::max(0, h - 2);
      const int pillW = std::min(420, contentW * 2 / 3);
      const int pillX = safe.x + (safe.width - pillW) / 2;
      const int pillY = y + (h - pillH) / 2;
      if (i == selected_) {
        // The pill is black whether focused or not, so focus needs its own
        // signal: a 2 px outline with a 4 px white gap. Drawn only; the touch
        // band stays the whole row, which is what rowTouch() already reports.
        renderer.drawRoundedRect(pillX - 5, pillY - 5, pillW + 10, pillH + 10, 2, (pillH + 10) / 2, true);
      }
      renderer.fillRoundedRect(pillX, pillY, pillW, pillH, pillH / 2, Color::Black);
      const char* label = tr(STR_JJWXC_ADV_RUN);
      const int textW = renderer.getTextWidth(kBodyFont, label, EpdFontFamily::BOLD);
      const int lineH = renderer.getLineHeight(kBodyFont);
      renderer.drawText(kBodyFont, pillX + (pillW - textW) / 2, pillY + (pillH - lineH) / 2, label, false,
                        EpdFontFamily::BOLD);
      continue;
    }

    const bool cursor = i == selected_;
    bool foregroundBlack = true;
    if (cursor) foregroundBlack = GUI.drawSelectionBackground(renderer, Rect{safe.x, y, safe.width, h});
    renderer.drawText(kListFont, x, y + (h - titleLine) / 2, rows_[i].title.c_str(), foregroundBlack,
                      cursor ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (!rows_[i].subtitle.empty()) {
      // Same cap as the CPY row language: half of the free width, so a long tag
      // list never reaches the title. The value is truncated on a code-point
      // boundary and drawn right-aligned.
      char val[128];
      snprintf(val, sizeof(val), "%s", rows_[i].subtitle.c_str());
      truncateToWidth(renderer, kSmallFont, val, std::max(24, (right - x) / 2));
      const int valW = renderer.getTextWidth(kSmallFont, val);
      renderer.drawText(kSmallFont, right - valW, y + (h - smallLine) / 2, val, foregroundBlack);
    }
    // Hairline under each form row; the pill draws its own shape and needs none.
    renderer.drawLine(x, y + h - 1, right, y + h - 1, true);
  }

  // Same fallback hint as the generic renderer, for a viewport too short to show
  // all rows (not expected on either panel, but the page must remain navigable).
  if (last < n) {
    char hint[48];
    snprintf(hint, sizeof(hint), "▼ %d/%d", selected_ + 1, n);
    const int lineH = renderer.getLineHeight(kSmallFont);
    int hintY = top + (last - scroll_) * h + 2;
    const int maxY = bottom - lineH;
    if (hintY > maxY) hintY = maxY;
    if (hintY < top) hintY = top;
    renderer.drawText(kSmallFont, x, hintY, hint, true);
  }
}

// Design frame 14's chapter rows: a monospace-looking number gutter, the title,
// and either the range marker (起/止) or the cache-state tag at the right end.
// The row language is the design's .lrow -- 96px, 32px side padding -- and the
// cursor borrows the shelf list's whole-row inversion. A black row is therefore
// one of two things, and the marker text tells them apart: an endpoint carries
// 起/止, a cursor does not. Drawn and hit on one geometry (rowHeight()/rowTop())
// exactly like the default renderer.
void JjwxcActivity::renderCpyChapterRows() const {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int top = rowTop();
  const int bottom = detailViewportBottom();
  const int h = rowHeight();
  const int sidePad = 32;  // the design's .lrow padding, not metrics: frame 14 is explicit
  const int x0 = safe.x + sidePad;
  const int rightX = safe.x + safe.width - sidePad;
  constexpr int kGutter = 56;  // .lrow .no width
  constexpr int kGap = 16;     // .lrow gap
  const int titleX = x0 + kGutter + kGap;
  const int titleLine = renderer.getLineHeight(kListFont);
  const int smallLine = renderer.getLineHeight(kSmallFont);

  const bool haveRange = rangeFrom_ >= 0 && rangeTo_ >= 0;
  const int lo = haveRange ? std::min(rangeFrom_, rangeTo_) : -1;
  const int hi = haveRange ? std::max(rangeFrom_, rangeTo_) : -1;

  const GfxRenderer::ClipScope clip(renderer, safe.x, top, safe.width, bottom > top ? bottom - top : 0);
  const int last = std::min(scroll_ + visibleRows(), static_cast<int>(rows_.size()));
  char buf[48];
  for (int i = scroll_; i < last; ++i) {
    const int y = top + (i - scroll_) * h;
    const bool isStart = haveRange && i == lo;
    const bool isEnd = haveRange && i == hi;
    const bool cursor = i == selected_;
    const bool inverted = isStart || isEnd || cursor;
    if (inverted) renderer.fillRect(safe.x, y, safe.width, h, true);

    // The number gutter is the row's ordinal, one-based and zero-padded, so the
    // column has a stable width (the design shows 019, not 19).
    snprintf(buf, sizeof(buf), "%03d", i + 1);
    renderer.drawText(kSmallFont, x0, y + (h - smallLine) / 2, buf, !inverted);

    renderer.drawText(kListFont, titleX, y + (h - titleLine) / 2, rows_[i].title.c_str(), !inverted);

    if (isStart || isEnd) {
      // Marker, not tag: the design puts 起/止 where the tag would sit. A
      // one-chapter range is both, and shows both.
      const char* marker = isStart && isEnd ? nullptr : (isStart ? tr(STR_JJWXC_RANGE_MARK_FROM) : tr(STR_JJWXC_RANGE_MARK_TO));
      if (marker) {
        const int w = renderer.getTextWidth(kListFont, marker);
        renderer.drawText(kListFont, rightX - w, y + (h - titleLine) / 2, marker, !inverted);
      } else {
        snprintf(buf, sizeof(buf), "%s·%s", tr(STR_JJWXC_RANGE_MARK_FROM), tr(STR_JJWXC_RANGE_MARK_TO));
        const int w = renderer.getTextWidth(kListFont, buf);
        renderer.drawText(kListFont, rightX - w, y + (h - titleLine) / 2, buf, !inverted);
      }
    } else {
      const uint8_t rawState = i < static_cast<int>(chapterStates_.size()) ? chapterStates_[i] : 0;
      const bool isVip = i < static_cast<int>(chapters_.size()) && chapters_[i].isVip;
      // One tag, as the design draws it -- VIP rides inside it rather than
      // getting a second chip, because on this page the only decision a VIP
      // flag feeds is whether an unpurchased row will stay dark.
      snprintf(buf, sizeof(buf), "%s%s", stateLabel(rawState), isVip ? " · VIP" : "");
      const int textW = renderer.getTextWidth(kSmallFont, buf);
      const int padX = 10;  // .lrow .tag padding: 2px 10px
      const int tagW = textW + padX * 2;
      const int tagH = smallLine + 8;
      const int tagX = rightX - tagW;
      const int tagY = y + (h - tagH) / 2;
      renderer.drawRoundedRect(tagX, tagY, tagW, tagH, 1, 8, !inverted);
      renderer.drawText(kSmallFont, tagX + padX, tagY + (tagH - smallLine) / 2, buf, !inverted);
    }

    // The design's hairline under every row (border-bottom: 1px). Invisible on
    // an inverted row either way, so no branch.
    renderer.drawLine(safe.x, y + h - 1, safe.x + safe.width, y + h - 1, true);
  }

  // Position readout, the shelf's language ("12/69") rather than the old "▼
  // n/m" hint. Clamped into the leftover band exactly like the default
  // renderer's hint: under the last row, or over its empty lower half.
  if (!rows_.empty()) {
    snprintf(buf, sizeof(buf), "%d / %d", selected_ + 1, static_cast<int>(rows_.size()));
    const int w = renderer.getTextWidth(kSmallFont, buf);
    const int maxY = bottom - smallLine;
    int hintY = top + (last - scroll_) * h + 4;
    if (hintY > maxY) hintY = maxY;
    if (hintY < top) hintY = top;
    renderer.drawText(kSmallFont, rightX - w, hintY, buf, true);
  }
}

// Design frame 13's header half: the status card -- mascot, state, one honest
// detail line -- then the 账号 group label and the design's double rule. The
// drawn segments and infoBlockHeight()'s sum are the same constants, so the
// rows below cannot drift off the panel (AGENTS.md rule #14).
void JjwxcActivity::renderCpyAccountHeader() const {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  constexpr int kSidePad = 32;
  constexpr int kCardH = 108;  // ring included
  constexpr int kPad = 24;
  const int x = safe.x + kSidePad;
  const int w = safe.width - kSidePad * 2;
  const int y = listTop() + 6;

  GUI.drawCardFrame(renderer, Rect{x, y, w, kCardH}, false);
  // No mascot here: the frozen duck list (design decision 9) allows the duck
  // only on the four main tabs, the progress bars, empty states, the sleep
  // screen, the splash and the standby face. The account card was a later
  // addition that broke that list, so the status text gets the full width.
  const int textX = x + kPad;
  const int titleLine = renderer.getLineHeight(kListFont);
  const int smallLine = renderer.getLineHeight(kSmallFont);
  const bool loggedIn = haveSession();
  char detail[64] = {};
  if (loggedIn) {
    // "Synced" is a claim. Only the loaded shelf can back it, so the count
    // appears once the shelf has actually been fetched this session; before
    // that the line stays honest about what is known.
    if (!shelf_.empty()) {
      snprintf(detail, sizeof(detail), tr(STR_JJWXC_SESSION_SHELF_FMT),
               static_cast<unsigned long>(shelf_.size()));
    } else {
      snprintf(detail, sizeof(detail), "%s", tr(STR_JJWXC_SESSION_OK));
    }
  }
  const char* status = loggedIn ? tr(STR_JJWXC_LOGGED_IN) : tr(STR_JJWXC_NEED_LOGIN);
  const int blockH = titleLine + (detail[0] ? 4 + smallLine : 0);
  const int ty = y + kPad + ((kCardH - kPad * 2) - blockH) / 2;
  renderer.drawText(kListFont, textX, ty, status, true, EpdFontFamily::BOLD);
  if (detail[0]) renderer.drawText(kSmallFont, textX, ty + titleLine + 4, detail, true);

  // .grp + .dbl: the group label, then two hairlines 4px apart.
  int gy = y + kCardH + 14;
  renderer.drawText(UI_12_FONT_ID, x, gy, tr(STR_JJWXC_ACCOUNT), true, EpdFontFamily::BOLD);
  gy += renderer.getLineHeight(UI_12_FONT_ID) + 6;
  renderer.drawLine(x, gy, x + w, gy, true);
  renderer.drawLine(x, gy + 4, x + w, gy + 4, true);
}

// Design frame 13's rows half: single-line 96px rows, the subtitle as the
// row's right-hand value, chevrons in the CPY two-line idiom, and the shelf
// list's whole-row inversion for the cursor. `withCard` wraps short action
// lists (account, menu, range presets) in the design's card; long browsing
// lists (shelf, library, search) stay plain so the card's side padding does
// not eat the value column.
void JjwxcActivity::renderCpyPlainRows(const bool withCard) const {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  constexpr int kSidePad = 32;
  constexpr int kPad = 24;  // .row padding: 0 24px, inside the ring
  const int top = rowTop();
  const int h = rowHeight();
  const int n = static_cast<int>(rows_.size());
  if (n <= 0) return;
  const int bottom = detailViewportBottom();

  int textX = safe.x + kSidePad;
  int rightX = safe.x + safe.width - kSidePad;
  int cardX = 0;
  int cardW = 0;
  if (withCard) {
    cardX = safe.x + kSidePad;
    cardW = safe.width - kSidePad * 2;
    // The card wraps the rows: 2px ring above the first and below the last.
    GUI.drawCardFrame(renderer, Rect{cardX, top - 4, cardW, n * h + 8}, false);
    textX = cardX + 2 + kPad;
    rightX = cardX + cardW - 2 - kPad;
  }
  const int titleLine = renderer.getLineHeight(kListFont);
  const int smallLine = renderer.getLineHeight(kSmallFont);
  // Book lists carry two facts per entry (title, then author/progress). Putting
  // them on separate lines is what keeps a long title from running under the
  // right-hand value: the old single-line row drew the title at full width and
  // the subtitle right-aligned on the same baseline, so the two necessarily
  // overlapped once the title was wider than the leftover column.
  const bool bookRows = !withCard && (state_ == State::Shelf || state_ == State::Library ||
                                      state_ == State::SearchResults || state_ == State::DiscoverList ||
                                      state_ == State::CommentList);
  const int textMax = std::max(24, rightX - 24 - textX);
  const int blockH = titleLine + 4 + smallLine;
  const GfxRenderer::ClipScope clip(renderer, safe.x, top, safe.width, std::max(0, bottom - top));

  const int last = std::min(scroll_ + visibleRows(), n);
  for (int i = scroll_; i < last; ++i) {
    const int y = top + (i - scroll_) * h;
    const bool cursor = i == selected_;
    if (cursor) {
      renderer.fillRect(withCard ? cardX + 2 : safe.x, y, withCard ? cardW - 4 : safe.width, h, true);
    }
    if (i > 0) renderer.drawLine(textX, y, rightX, y, true);
    if (bookRows) {
      const bool hasSub = !rows_[i].subtitle.empty();
      const int ty = hasSub ? y + (h - blockH) / 2 : y + (h - titleLine) / 2;
      char title[256];
      const auto titleStyle = cursor ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      copyMiddleEllipsis(renderer, kListFont, rows_[i].title.c_str(), title, sizeof(title), textMax, titleStyle);
      renderer.drawText(kListFont, textX, ty, title, !cursor, titleStyle);
      if (hasSub) {
        char val[256];
        snprintf(val, sizeof(val), "%s", rows_[i].subtitle.c_str());
        if (renderer.getTextWidth(kSmallFont, val) > textMax) {
          truncateToWidth(renderer, kSmallFont, val, textMax);
          appendEllipsis(renderer, kSmallFont, val, sizeof(val), textMax);
        }
        renderer.drawText(kSmallFont, textX, ty + titleLine + 4, val, !cursor);
      }
    } else {
      renderer.drawText(kListFont, textX, y + (h - titleLine) / 2, rows_[i].title.c_str(), !cursor,
                        cursor ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      if (!rows_[i].subtitle.empty()) {
        // Action menus keep the design's right-hand value; their labels are short
        // by construction, and the cap keeps a stored login name off the chevron.
        char val[64];
        snprintf(val, sizeof(val), "%s", rows_[i].subtitle.c_str());
        truncateToWidth(renderer, kSmallFont, val, std::max(24, (rightX - textX) / 2));
        const int valW = renderer.getTextWidth(kSmallFont, val);
        renderer.drawText(kSmallFont, rightX - 24 - valW, y + (h - smallLine) / 2, val, !cursor);
      }
    }
    const int cy = y + h / 2;
    renderer.drawLine(rightX - 6, cy - 7, rightX, cy, 2, !cursor);
    renderer.drawLine(rightX, cy, rightX - 6, cy + 7, 2, !cursor);
  }
}

// Frame 12's action pills: the same rows_ actions, left to right at the design's
// 64px pill height, wrapped onto another line when the page width runs out.
// Returns the band height -- what rowHeight() reports, so the collapsed layout's
// row reserve pays for the whole band -- and optionally fills the per-pill rects
// the drawing and the tap test share.
int JjwxcActivity::cpyDetailPillLayout(const int top, Rect* out, const int maxOut) const {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  constexpr int kSidePad = 32;
  constexpr int kGap = 10;
  constexpr int kPrimaryH = 96;
  constexpr int kSecondaryH = 80;
  constexpr int kSmallH = 58;
  constexpr int kSmallStride = 64;
  const int left = safe.x + kSidePad;
  const int right = safe.x + safe.width - kSidePad;
  const int width = std::max(1, right - left);
  const int n = static_cast<int>(rows_.size());
  if (n <= 0) return 0;

  int primary = 0;
  for (int i = 0; i < n; ++i) {
    if (rowIds_[i] == "open") {
      primary = i;
      break;
    }
  }

  int y = top + 8;
  if (out != nullptr && primary < maxOut) out[primary] = Rect{left, y, width, kPrimaryH};
  y += kPrimaryH + kGap;

  // The next three actions are the design's secondary band; the rest become
  // quiet two-column text rows. Every row still gets a rect and a hit target.
  const int secondaryW = (width - 2 * kGap) / 3;
  const int smallW = (width - kGap) / 2;
  int secondary = 0;
  int small = 0;
  for (int i = 0; i < n; ++i) {
    if (i == primary) continue;
    if (secondary < 3) {
      if (out != nullptr && i < maxOut) {
        out[i] = Rect{left + secondary * (secondaryW + kGap), y, secondaryW, kSecondaryH};
      }
      ++secondary;
    } else {
      if (out != nullptr && i < maxOut) {
        out[i] = Rect{left + (small % 2) * (smallW + kGap), y + kSecondaryH + kGap +
                                                               (small / 2) * kSmallStride,
                      smallW, kSmallH};
      }
      ++small;
    }
  }
  y += kSecondaryH + kGap;
  if (small > 0) y += ((small + 1) / 2) * kSmallStride;
  return y - top + 8;
}

void JjwxcActivity::renderCpyDetailPills() const {
  const int n = static_cast<int>(rows_.size());
  if (n <= 0) return;
  const int top = rowTop();
  if (top >= detailViewportBottom()) return;
  // BookDetail can carry up to twelve actions (open, cache, chapters, refresh,
  // cloud jump, favourite, purchase, comments, ...). The old cap of eight
  // silently dropped the tail -- including "open local book" on a fully set-up
  // book -- while the band still reserved their height, leaving blank rows that
  // could not be tapped. The layout already wraps, so the array is the only cap.
  constexpr int kMaxPills = 12;
  Rect rects[kMaxPills];
  cpyDetailPillLayout(top, rects, kMaxPills);
  const int count = std::min(n, kMaxPills);

  // The design's primary pill is the read action. That is the 打开本地 row when
  // the book has a readable artefact; otherwise the first action (缓存章节) is
  // what gets the reader there, so it carries the emphasis instead.
  int primary = 0;
  for (int i = 0; i < count; ++i) {
    if (rowIds_[i] == "open") {
      primary = i;
      break;
    }
  }

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int textLine = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int kRadius = 32;  // primary/secondary capsules
  // The band scrolls with the block, so its clip starts at whichever of the band
  // and the content viewport is lower -- a scrolled-up pill must not paint over
  // the header, the same rule the block's own clip follows.
  const int clipTop = std::max(top, listTop());
  const GfxRenderer::ClipScope clip(renderer, safe.x, clipTop, safe.width,
                                    std::max(0, detailViewportBottom() - clipTop));
  for (int i = 0; i < count; ++i) {
    const Rect& r = rects[i];
    const char* label = rows_[i].title.c_str();
    const bool isPrimary = i == primary;
    const bool smallRow = r.height <= 60;
    const int radius = smallRow ? 10 : (r.height < 90 ? 24 : kRadius);
    if (isPrimary) {
      renderer.fillRoundedRect(r.x, r.y, r.width, r.height, radius, Color::Black);
    } else if (smallRow) {
      renderer.drawLine(r.x, r.y, r.x + r.width, r.y, true);
      renderer.drawLine(r.x, r.y + r.height - 1, r.x + r.width, r.y + r.height - 1, true);
    } else {
      renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, radius, true);
    }
    // The cursor is a ring -- the same "selected" language as the cover grid --
    // so it stays readable through both pill styles.
    if (i == selected_) {
      renderer.drawRoundedRect(r.x - 3, r.y - 3, r.width + 6, r.height + 6, 2, radius + 3, true);
    }
    const std::string shown = renderer.truncatedText(UI_10_FONT_ID, label, std::max(16, r.width - 16));
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, shown.c_str());
    renderer.drawText(UI_10_FONT_ID, r.x + (r.width - textW) / 2, r.y + (r.height - textLine) / 2,
                      shown.c_str(), !isPrimary);
  }

  // Design frame 12's page order: card, actions, the double rule, then the
  // blurb. The synopsis is the page's tail and the one part that scrolls past
  // (maxDetailScroll() pays for it), so the actions always own the first
  // screen.
  const DetailLayout& L = detailLayout();
  {
    const int bandH = cpyDetailPillLayout(top, nullptr, 0);
    int y = top + bandH;
    constexpr int kSidePad = 32;
    const int x = safe.x + kSidePad;
    const int w = safe.width - kSidePad * 2;
    renderer.drawLine(x, y, x + w, y, true);
    renderer.drawLine(x, y + 4, x + w, y + 4, true);
    y += kCpyBlurbRuleBlock;
    if (L.blurbLines > 0) {
      const int lineH = renderer.getLineHeight(kListFont);
      if (y + lineH > listTop() && y < detailViewportBottom()) {
        renderer.drawText(kListFont, x, y, tr(STR_JJWXC_DETAIL_INTRO), true, EpdFontFamily::BOLD);
      }
      y += lineH;
      const char* text = blurbText();
      const size_t len = blurbLen();
      char buf[kBlurbLineBytes];
      for (int i = 0; i < L.blurbLines; ++i) {
        const int ly = y + i * lineH;
        if (ly + lineH <= listTop()) continue;  // scrolled above the viewport
        if (ly >= detailViewportBottom()) break;
        if (static_cast<size_t>(i) >= blurbLines_.size()) break;
        const auto& wl = blurbLines_[static_cast<size_t>(i)];
        if (wl.offset >= len) break;
        copyWrappedLine(buf, sizeof(buf), text + wl.offset, std::min<size_t>(wl.bytes, len - wl.offset));
        if (buf[0] != '\0') renderer.drawText(kListFont, x, ly, buf, true);
      }
    }
  }
}

void JjwxcActivity::renderDisclaimer() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int x = safe.x + metrics.contentSidePadding;
  int y = safe.y + metrics.verticalSpacing;
  const bool cpy = UITheme::getInstance().usesCpyLayout();
  if (cpy) {
    // Frame 18/19's page type: the standard sub-page bar (round back + title +
    // battery) over the body, exactly like every other CPY page.
    renderHeader(tr(STR_JJWXC_DISCLAIMER_TITLE), nullptr);
    y = safe.y + metrics.headerHeight + 12;
  } else {
    renderer.drawText(kBodyFont, x, y, tr(STR_JJWXC_DISCLAIMER_TITLE), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(kBodyFont) + 8;
  }

  // Fixed lines rather than a wrapped paragraph: the wording is short and this
  // avoids pulling in the reader's layout engine for one screen.
  const char* lines[] = {tr(STR_JJWXC_DISCLAIMER_1), tr(STR_JJWXC_DISCLAIMER_2), tr(STR_JJWXC_DISCLAIMER_3),
                         tr(STR_JJWXC_DISCLAIMER_4)};
  for (const char* line : lines) {
    if (!line || !line[0]) continue;
    renderer.drawText(kListFont, x, y, line, true);
    y += renderer.getLineHeight(kListFont) + 6;
  }
  y += 12;
  const Rect acceptRect{safe.x, y, safe.width, rowHeight()};
  // Cached so the input handler hit-tests the same geometry it drew.
  disclaimerAcceptY_ = y;
  if (cpy) {
    // The design's primary pill, centred in the same accept band the hit-test
    // reads -- drawing and hit stay on one geometry.
    constexpr int kPillH = 64;
    constexpr int kPillPad = 32;
    const int pillW = std::min(safe.width - kPillPad * 2, 360);
    const int pillX = safe.x + (safe.width - pillW) / 2;
    const int pillY = y + (rowHeight() - kPillH) / 2;
    renderer.fillRoundedRect(pillX, pillY, pillW, kPillH, kPillH / 2, Color::Black);
    const char* label = tr(STR_JJWXC_ACCEPT);
    const int textW = renderer.getTextWidth(kBodyFont, label);
    const int lineH = renderer.getLineHeight(kBodyFont);
    renderer.drawText(kBodyFont, pillX + (pillW - textW) / 2, pillY + (kPillH - lineH) / 2, label, false,
                      EpdFontFamily::BOLD);
  } else {
    const bool fg = GUI.drawSelectionBackground(renderer, acceptRect);
    renderer.drawText(kBodyFont, x, y + 6, tr(STR_JJWXC_ACCEPT), fg, EpdFontFamily::BOLD);
  }
}

void JjwxcActivity::renderSearchChips() const {
  Rect chips[3];
  searchChipLayout(chips, 3);
  const char* labels[3] = {tr(STR_JJWXC_SEARCH_MODE_TITLE), tr(STR_JJWXC_SEARCH_MODE_AUTHOR),
                           tr(STR_JJWXC_SEARCH_MODE_ADV)};
  const int lineH = renderer.getLineHeight(kBodyFont);
  for (int i = 0; i < 3; ++i) {
    if (chips[i].width <= 0 || chips[i].height <= 0) continue;
    const bool active = searchMode_ == static_cast<uint8_t>(i);
    // Selected: solid pill with inverted text. Unselected: the same pill outline
    // with normal text. Both are drawn from the rects searchChipLayout() also
    // hands to loop()'s tap test, so the chip a finger hits is the chip it sees.
    if (active) {
      renderer.fillRoundedRect(chips[i].x, chips[i].y, chips[i].width, chips[i].height, chips[i].height / 2,
                               Color::Black);
    } else {
      renderer.drawRoundedRect(chips[i].x, chips[i].y, chips[i].width, chips[i].height, 2, chips[i].height / 2,
                               true);
    }
    char label[64];
    snprintf(label, sizeof(label), "%s", labels[i]);
    truncateToWidth(renderer, kBodyFont, label, std::max(0, chips[i].width - 16));
    const int styleWidth = renderer.getTextWidth(kBodyFont, label, active ? EpdFontFamily::BOLD
                                                                        : EpdFontFamily::REGULAR);
    renderer.drawText(kBodyFont, chips[i].x + (chips[i].width - styleWidth) / 2,
                      chips[i].y + (chips[i].height - lineH) / 2, label, !active,
                      active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}

void JjwxcActivity::renderInfoBlock() const {
  // The unified search page's info band is the three mode chips, not text lines.
  if (state_ == State::AdvancedSearch) {
    renderSearchChips();
    return;
  }
  // BookDetail is not a stack of equal lines: it has a picture in it and a wrapped
  // paragraph under it. Its own renderer, driven by the same cached layout that
  // infoBlockHeight() -- and therefore rowTop(), visibleRows() and the row hit-test --
  // reads, so the drawing and the geometry cannot drift apart (rule #14).
  if (state_ == State::BookDetail) {
    renderBookDetail();
    return;
  }
  if (state_ == State::Account && UITheme::getInstance().usesCpyLayout()) {
    renderCpyAccountHeader();
    return;
  }
  const int count = infoLineCount();
  if (count <= 0) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int x = safe.x + metrics.contentSidePadding;
  const int maxWidth = safe.width - 2 * metrics.contentSidePadding;
  const int lineH = renderer.getLineHeight(kSmallFont);
  int y = listTop();
  char buf[kInfoLineBytes];
  for (int i = 0; i < count; ++i) {
    if (!infoLine(i, buf, sizeof(buf))) break;
    // Clipped rather than wrapped: the block's height is what rowTop() reports to
    // both the drawing loop and the row hit-test, so it must stay exactly
    // infoLineCount() lines tall.
    truncateToWidth(renderer, kSmallFont, buf, maxWidth);
    renderer.drawText(kSmallFont, x, y, buf, true);
    y += lineH;
  }
}

// Draws the book-detail block from the cached layout. Every number it uses comes out
// of detailLayout(), and every line it draws is one the layout measured and recorded,
// so what appears on the panel and what rowTop() tells the hit-tester are the same
// geometry.
//
// Allocation-free and measurement-free: the cover's Bitmap and its row scratch were
// created when the state was entered, the wrapped lines were recorded by the layout
// pass, and the two text buffers are on the stack. Per frame this only copies lines out
// of PSRAM and skips the ones the page has scrolled past.
void JjwxcActivity::renderBookDetail() const {
  const DetailLayout& L = detailLayout();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int x0 = safe.x + metrics.contentSidePadding;
  const int top = blockTop();
  const int viewportBottom = detailViewportBottom();
  // The page scrolls, so the block can be partly above the viewport; without the clip it
  // would paint over the header. Same tool renderRows() uses for the same reason.
  const GfxRenderer::ClipScope clip(renderer, safe.x, listTop(), safe.width,
                                    std::max(0, viewportBottom - listTop()));

  // CPY (frame 12): the card frames the cover + facts band. Its extents come
  // from the cached layout -- the same numbers the drawing below uses -- so it
  // can never frame the wrong box. The bottom edge stops short when the blurb
  // follows, because kBlurbGap is smaller than the card's padding and the
  // heading would otherwise print on the ring.
  if (UITheme::getInstance().usesCpyLayout()) {
    const int contentW = L.factX + L.factWidth;
    const int contentH = std::max(
        L.coverH, L.heroH + (L.bandH > 0 ? L.bandH + kDetailBandGap : 0) +
                      L.factLines * renderer.getLineHeight(kSmallFont));
    if (contentW > 0 && contentH > 0) {
      constexpr int kPad = 24;  // .card>.in padding
      constexpr int kRing = 2;
      const int bottomPad = L.blurbLines > 0 ? std::max(4, kBlurbGap - kRing - 2) : kPad;
      int cardL = x0 - kPad - kRing;
      int cardR = x0 + contentW + kPad + kRing;
      if (cardL < safe.x) cardL = safe.x;
      if (cardR > safe.x + safe.width) cardR = safe.x + safe.width;
      const int cardT = top - kPad - kRing;
      const int cardB = top + contentH + bottomPad + kRing;
      if (cardB > cardT) GUI.drawCardFrame(renderer, Rect{cardL, cardT, cardR - cardL, cardB - cardT}, false);
    }
  }

  // drawBitmap() scales a bitmap DOWN to fit the box and never up
  // (GfxRenderer.cpp:1887), which is why the thumbnail is generated at exactly the
  // size this slot asks for: on the panel it was sized for it is a 1:1 blit, and on a
  // narrower one it shrinks once rather than twice.
  if (L.coverW > 0 && L.coverH > 0 && top + L.coverH > listTop() && top < viewportBottom) {
    const Rect slot{x0, top, L.coverW, L.coverH};
    if (coverBitmap_ && coverBitmap_->rewindToData() == BmpReaderError::Ok) {
      // coverBitmap_ is parsed once in loadCoverThumb() and retained across renders,
      // but Bitmap::readNextRow() is purely sequential and never auto-rewinds. After
      // the first full read the file sits at EOF, so the next pass returns ShortReadRow
      // and drawBitmap*/drawBitmapGrayscale16 fail silently (a blank where the cover
      // belongs). Home avoids this by rebuilding a fresh memory Bitmap and calling
      // parseHeaders() every render; we keep the file open instead, so we must rewind
      // to the data offset before each draw -- the same pattern ReadingBackground,
      // ImageViewer and SleepActivity use. On a rewind failure we fall through to the
      // placeholder rather than present a blank slot.
      // In a 16-level pass the cover must go through drawBitmapGrayscale16: plain
      // drawBitmap paints only the B/W proxy framebuffer, never the gray buffer, so
      // commitGrayscale16 would present a blank where the cover belongs. This is the
      // same explicit call the sleep cover (SleepActivity) and AirPage image use;
      // home reaches it via drawBitmapCropToFill, which routes internally. Both scale
      // fit-inside (never up), and the thumbnail shares the slot's 5:7 aspect, so the
      // 16-level and B/W paths frame the cover identically.
      if (renderer.isGrayscale16Active())
        renderer.drawBitmapGrayscale16(*coverBitmap_, slot.x, slot.y, slot.width, slot.height);
      else
        renderer.drawBitmap(*coverBitmap_, slot.x, slot.y, slot.width, slot.height);
    } else {
      // The theme's own "no cover art" box. Only reached when a cover URL is known
      // but the thumbnail is not on the card -- offline, or the download failed -- so
      // the page keeps its shape instead of collapsing.
      GUI.drawCoverPlaceholder(renderer, slot);
    }
  }

  // Design frame 11: three big numbers above the prose facts. They are derived
  // from the local toc, so the band stays meaningful offline.
  const int bandTop = top + L.heroH;
  if (L.heroH > 0 && top + L.heroH > listTop() && top < viewportBottom) {
    const int heroX = x0 + L.factX;
    const int heroW = L.factWidth;
    const char* workTitle = info_.novelName[0] ? info_.novelName : novel_.title;
    const char* author = info_.authorName[0] ? info_.authorName : novel_.author;
    std::string meta = tr(STR_SOURCE_JJWXC);
    if (info_.sizeText[0]) meta += std::string(" · ") + info_.sizeText;
    if (info_.vipChapterId > 0) meta += std::string(" · ") + tr(STR_JJWXC_DETAIL_VIP_FROM);
    const std::string metaShown = renderer.truncatedText(UI_10_FONT_ID, meta.c_str(), heroW);
    renderer.drawText(UI_10_FONT_ID, heroX, top + 2, metaShown.c_str());
    const std::string titleShown = renderer.truncatedText(NOTOSERIF_18_FONT_ID, workTitle, heroW,
                                                          EpdFontFamily::BOLD);
    renderer.drawText(NOTOSERIF_18_FONT_ID, heroX, top + 26, titleShown.c_str(), true, EpdFontFamily::BOLD);
    const std::string authorShown = renderer.truncatedText(UI_10_FONT_ID, author, heroW);
    renderer.drawText(UI_10_FONT_ID, heroX, top + 68, authorShown.c_str());
    if (info_.introShort[0]) {
      const std::string tagline = renderer.truncatedText(UI_10_FONT_ID, info_.introShort, heroW);
      renderer.drawText(UI_10_FONT_ID, heroX, top + 92, tagline.c_str());
    }
  }
  if (L.bandH > 0 && bandTop + L.bandH > listTop() && bandTop < viewportBottom) {
    const uint32_t total = novel_.remoteChapterCount != 0 ? novel_.remoteChapterCount : info_.chapterCount;
    const uint32_t cached = novel_.cachedChapterCount;
    const uint32_t blocked =
        novel_.packagedChapterCount > cached ? novel_.packagedChapterCount - cached : 0;
    char numbers[3][24];
    std::snprintf(numbers[0], sizeof(numbers[0]), "%lu", static_cast<unsigned long>(total));
    std::snprintf(numbers[1], sizeof(numbers[1]), "%lu", static_cast<unsigned long>(cached));
    std::snprintf(numbers[2], sizeof(numbers[2]), "%lu", static_cast<unsigned long>(blocked));
    const freeink::Icon* icons[3] = {&icon_book_48, &icon_download_48, &icon_hand_48};
    const StrId labels[3] = {StrId::STR_JJWXC_DETAIL_CHAPTERS, StrId::STR_JJWXC_DETAIL_CACHED,
                             StrId::STR_JJWXC_DETAIL_BLOCKED_SHORT};
    const int cellW = std::max(1, L.factWidth / 3);
    const int bandX = x0 + L.factX;
    const int iconSize = 24;
    for (int i = 0; i < 3; ++i) {
      const int cx = bandX + i * cellW;
      drawDetailIcon(renderer, *icons[i], cx, bandTop + 4, iconSize);
      renderer.drawText(UI_12_FONT_ID, cx, bandTop + 36, numbers[i], true, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, cx, bandTop + 70, I18N.get(labels[i]));
    }
    renderer.drawLine(bandX, bandTop + L.bandH - 1, bandX + L.factWidth, bandTop + L.bandH - 1, true);
  }

  const int factTop = top + L.heroH + (L.bandH > 0 ? L.bandH + kDetailBandGap : 0);
  if (L.factLines > 0 && L.factWidth > 0) {
    char buf[kInfoLineBytes];
    const int lineH = renderer.getLineHeight(kSmallFont);
    for (int i = 0; i < L.factLines; ++i) {
      const int y = factTop + i * lineH;
      if (y + lineH <= listTop()) continue;  // scrolled above the viewport
      if (y >= viewportBottom) break;        // and so is everything after it
      if (!detailFactLine(i, buf, sizeof(buf))) break;
      if (i == L.factLines - 1 && L.factCut) appendEllipsis(renderer, kSmallFont, buf, sizeof(buf), L.factWidth);
      renderer.drawText(kSmallFont, x0 + L.factX, y, buf, true);
    }
  }

  if (L.blurbLines <= 0) return;
  // CPY (frame 12): the blurb is drawn below the pill band, not inside this
  // block -- see renderCpyDetailPills().
  if (UITheme::getInstance().usesCpyLayout()) return;
  const int lineH = renderer.getLineHeight(kListFont);
  const int headingY = top + L.blurbY;
  if (headingY + lineH > listTop() && headingY < viewportBottom) {
    renderer.drawText(kListFont, x0 + L.blurbX, headingY, tr(STR_JJWXC_DETAIL_INTRO), true, EpdFontFamily::BOLD);
  }

  const char* text = blurbText();
  const size_t len = blurbLen();
  char buf[kBlurbLineBytes];
  for (int i = 0; i < L.blurbLines; ++i) {
    const int y = headingY + lineH * (i + 1);
    if (y + lineH <= listTop()) continue;
    if (y >= viewportBottom) break;
    if (static_cast<size_t>(i) >= blurbLines_.size()) break;
    const auto& wl = blurbLines_[static_cast<size_t>(i)];
    // The blurb is replaced by the worker and the offsets by the layout pass; if the two
    // ever disagree, stop rather than index past the paragraph.
    if (wl.offset >= len) break;
    copyWrappedLine(buf, sizeof(buf), text + wl.offset, std::min<size_t>(wl.bytes, len - wl.offset));
    if (i == L.blurbLines - 1 && L.blurbCut) appendEllipsis(renderer, kListFont, buf, sizeof(buf), L.blurbWidth);
    // An empty line is the gap between the tagline and the blurb: counted by the
    // layout so the two agree, and drawn as nothing.
    if (buf[0] != '\0') renderer.drawText(kListFont, x0 + L.blurbX, y, buf, true);
  }
}

void JjwxcActivity::renderMessage() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int x = safe.x + metrics.contentSidePadding;
  const int maxWidth = safe.width - 2 * metrics.contentSidePadding;
  int y = safe.y + safe.height / 4;

  // The failed operation is a bold heading, so fail() never has to splice two
  // translated strings into one sentence.
  if (!messageHeading_.empty()) {
    char heading[kInfoLineBytes];
    snprintf(heading, sizeof(heading), "%s", messageHeading_.c_str());
    truncateToWidth(renderer, kBodyFont, heading, maxWidth);
    renderer.drawText(kBodyFont, x, y, heading, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(kBodyFont) + 10;
  }

  const int lineH = renderer.getLineHeight(kBodyFont);
  const int bottom = safe.y + safe.height - 3 * lineH;
  const int maxLines = std::max(1, (bottom - y) / (lineH + 4));
  size_t pos = 0;
  for (int i = 0; i < maxLines && pos < message_.size(); ++i) {
    // Greedy fill from a stack buffer. getTextWidth() needs a NUL-terminated
    // string, and the previous implementation built a std::string for every
    // candidate byte, i.e. hundreds of tiny render-path allocations per frame;
    // truncateToWidth() cuts on a code-point boundary in place instead.
    char lineBuf[kInfoLineBytes];
    const size_t take = std::min(message_.size() - pos, sizeof(lineBuf) - 1);
    memcpy(lineBuf, message_.data() + pos, take);
    lineBuf[take] = '\0';
    truncateToWidth(renderer, kBodyFont, lineBuf, maxWidth);
    size_t used = strlen(lineBuf);
    if (used == 0) {
      // Nothing at all fits: advance one whole code point so the loop terminates.
      used = 1;
      while (pos + used < message_.size() && (static_cast<unsigned char>(message_[pos + used]) & 0xC0) == 0x80) ++used;
    } else {
      renderer.drawText(kBodyFont, x, y, lineBuf, true);
    }
    y += lineH + 4;
    pos += used;
  }
  y += 10;
  if (UITheme::getInstance().usesCpyLayout()) {
    // Frame 19's page type: an OK pill. The old hint was actively wrong on
    // this hardware -- ReadPico has no Confirm key at all.
    constexpr int kPillH = 64;
    constexpr int kPillW = 220;
    const int pillX = safe.x + (safe.width - kPillW) / 2;
    renderer.fillRoundedRect(pillX, y, kPillW, kPillH, kPillH / 2, Color::Black);
    const char* label = tr(STR_OK_BUTTON);
    const int textW = renderer.getTextWidth(kBodyFont, label);
    const int lineH = renderer.getLineHeight(kBodyFont);
    renderer.drawText(kBodyFont, pillX + (kPillW - textW) / 2, y + (kPillH - lineH) / 2, label, false,
                      EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(kSmallFont, y, tr(STR_JJWXC_PRESS_OK));
  }
}

// Generic "a job owns the screen" page. State::Downloading keeps its own page
// with a bar; every other network or SD job shares this one, so a request that
// takes seconds can never look like a hang.
void JjwxcActivity::renderBusy() const {
  auto& theme = UITheme::getInstance();
  const Rect safe = theme.getScreenSafeArea(renderer, true, true);
  const int maxWidth = safe.width - 2 * theme.getMetrics().contentSidePadding;
  if (!theme.usesCpyLayout()) {
    int y = safe.y + safe.height / 3;
    char line[kInfoLineBytes];
    snprintf(line, sizeof(line), "%s", busyText_.empty() ? tr(STR_JJWXC_LOADING) : busyText_.c_str());
    truncateToWidth(renderer, kBodyFont, line, maxWidth);
    renderer.drawCenteredText(kBodyFont, y, line, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(kBodyFont) + 12;
    renderer.drawCenteredText(kSmallFont, y, tr(STR_JJWXC_BUSY_HINT), true);
    y += renderer.getLineHeight(kSmallFont) + 24;
    renderer.drawCenteredText(kSmallFont, y, tr(STR_JJWXC_CANCEL_HINT), true);
    busyCancelRect_ = Rect{};
    return;
  }

  // The CPY waiting card: the mascot on its waterline, what the device is doing,
  // how long it has been doing it, and one clear way out. A blank page with
  // "please wait" was the whole previous design; the elapsed line is what tells
  // a 45-second TLS timeout apart from a hang.
  constexpr int kCardW = 560;
  constexpr int kCardH = 328;
  constexpr int kPad = 32;
  const int cardW = std::min(kCardW, safe.width - 48);
  const int cardH = kCardH;
  const int cardX = safe.x + (safe.width - cardW) / 2;
  const int cardY = safe.y + (safe.height - cardH) / 2;
  GUI.drawCardFrame(renderer, Rect{cardX, cardY, cardW, cardH}, false);

  // Indeterminate wait: swim the duck back and forth along the theme's OWN wave --
  // the exact drawProgressBar the reading/download progress bars use -- so the
  // water matches that style (full-width sine waterline + dithered body, mascot
  // riding the boundary) and the duck visibly moves on each 2 s busy repaint.
  // The previous hand-rolled triangular ripple was static and off-theme. current
  // ping-pongs 0 -> total -> 0 over kSwimPeriodMs; showPercentage stays off so it
  // reads as "working", not as determinate progress.
  const uint32_t swimElapsed = millis() - busyStartedMs_;
  constexpr uint32_t kSwimPeriodMs = 12000;
  const uint32_t swimHalf = kSwimPeriodMs / 2;
  const uint32_t swimPhase = swimElapsed % kSwimPeriodMs;
  const uint32_t swimTri = swimPhase < swimHalf ? swimPhase : (kSwimPeriodMs - swimPhase);
  const Rect waveRect{cardX + kPad, cardY + 54, cardW - 2 * kPad, 12};
  GUI.drawProgressBar(renderer, waveRect, static_cast<size_t>(swimTri), static_cast<size_t>(swimHalf), false);

  char title[kInfoLineBytes];
  snprintf(title, sizeof(title), "%s", busyText_.empty() ? tr(STR_JJWXC_LOADING) : busyText_.c_str());
  truncateToWidth(renderer, kBodyFont, title, cardW - 2 * kPad);
  int y = cardY + 104;
  renderer.drawText(kBodyFont, cardX + kPad, y, title, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(kBodyFont) + 6;
  // "Loading…" alone says nothing about what is loading. jobWhat_ is the action
  // the user asked for (shelf/search/login/delete), so it rides under the status
  // line whenever it is a different string.
  if (!jobWhat_.empty() && strcmp(jobWhat_.c_str(), title) != 0) {
    char action[kInfoLineBytes];
    snprintf(action, sizeof(action), "%s", jobWhat_.c_str());
    truncateToWidth(renderer, kSmallFont, action, cardW - 2 * kPad);
    renderer.drawText(kSmallFont, cardX + kPad, y, action, true);
    y += renderer.getLineHeight(kSmallFont) + 4;
  }

  char elapsed[64];
  const uint32_t seconds = (millis() - busyStartedMs_) / 1000;
  snprintf(elapsed, sizeof(elapsed), "%lu s", static_cast<unsigned long>(seconds));
  renderer.drawText(kSmallFont, cardX + kPad, y, elapsed, true);

  const int pillW = 200;
  const int pillH = 56;
  const int pillX = cardX + (cardW - pillW) / 2;
  const int pillY = cardY + cardH - 28 - pillH;
  renderer.fillRoundedRect(pillX, pillY, pillW, pillH, pillH / 2, Color::Black);
  const char* cancel = tr(STR_CANCEL);
  const int cancelW = renderer.getTextWidth(kBodyFont, cancel);
  const int cancelLine = renderer.getLineHeight(kBodyFont);
  renderer.drawText(kBodyFont, pillX + (pillW - cancelW) / 2, pillY + (pillH - cancelLine) / 2, cancel, false);
  busyCancelRect_ = Rect{pillX, pillY, pillW, pillH};
}

void JjwxcActivity::renderInlineJobOverlay() const {
  if (!UITheme::getInstance().usesCpyLayout()) {
    renderBusy();
    return;
  }
  GUI.drawModalScrim(renderer);
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  constexpr int kCardW = 480;
  constexpr int kCardH = 212;
  constexpr int kPad = 30;
  const int cardW = std::min(kCardW, safe.width - 64);
  const int cardX = safe.x + (safe.width - cardW) / 2;
  const int cardY = safe.y + (safe.height - kCardH) / 2;
  GUI.drawCardFrame(renderer, Rect{cardX, cardY, cardW, kCardH}, false);

  // The inline job card is a transient overlay, not one of the six places the
  // frozen list allows the mascot: no duck, no water band. It stays the same
  // white card as every other dialog.
  const int textX = cardX + kPad;
  const int textY = cardY + 28;
  char title[kInfoLineBytes];
  snprintf(title, sizeof(title), "%s…", busyText_.empty() ? tr(STR_JJWXC_LOADING) : busyText_.c_str());
  truncateToWidth(renderer, kBodyFont, title, cardW - 2 * kPad);
  renderer.drawText(kBodyFont, textX, textY, title, true, EpdFontFamily::BOLD);
  renderer.drawText(kSmallFont, textX, textY + renderer.getLineHeight(kBodyFont) + 4, tr(STR_JJWXC_BUSY_HINT),
                    true);
  busyCancelRect_ = Rect{};
}

void JjwxcActivity::renderToast() const {
  if (toast_.empty() || static_cast<int32_t>(millis() - toastUntilMs_) >= 0) return;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  char text[160];
  snprintf(text, sizeof(text), "%s", toast_.c_str());
  const int maxWidth = safe.width - 96;
  truncateToWidth(renderer, kSmallFont, text, maxWidth);
  const int textW = renderer.getTextWidth(kSmallFont, text);
  const int textH = renderer.getLineHeight(kSmallFont);
  const int pillW = std::min(safe.width - 48, textW + 56);
  const int pillH = 56;
  const int pillX = safe.x + (safe.width - pillW) / 2;
  const int pillY = safe.y + safe.height - pillH - 32;
  renderer.fillRoundedRect(pillX, pillY, pillW, pillH, pillH / 2, Color::Black);
  renderer.drawText(kSmallFont, pillX + (pillW - textW) / 2, pillY + (pillH - textH) / 2, text, false);
}

void JjwxcActivity::renderProgress() const {
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, true, true);
  const int x = safe.x + metrics.contentSidePadding;
  int y = safe.y + safe.height / 3;

  // The packaging half of the job has its own label: it is SD-bound, usually
  // short, and showing "Downloading" while nothing is on the network would be a
  // lie about what the device is doing.
  const bool packaging = dlPhase_.load() == static_cast<uint8_t>(JobPhase::Packaging);
  const char* headline = packaging ? tr(STR_JJWXC_PACKAGING) : busyText_.c_str();
  if (headline == nullptr || headline[0] == '\0') headline = tr(STR_JJWXC_LOADING);
  renderer.drawCenteredText(kBodyFont, y, headline, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(kBodyFont) + 8;

  const size_t done = dlDone_.load();
  const size_t total = dlTotal_.load();
  const uint8_t stage = dlStage_.load();

  // One combined 0..1000 scale for both halves. The old page reset the bar to 0
  // when packaging began, so a one-chapter run showed 0% -> 100% twice; and
  // because the chapter counter only moves when a chapter ENDS, that same run
  // sat at 0% until it jumped to 100%. The stage weights below are what make the
  // bar move while the first (and only) chapter is still being fetched.
  int permille = 0;
  if (total > 0) {
    if (packaging) {
      const size_t clamped = std::min(done, total);
      permille = 850 + static_cast<int>(clamped * 150 / total);
    } else {
      static constexpr uint16_t kStageWeight[4] = {0, 150, 400, 750};
      const uint16_t weight = kStageWeight[std::min<int>(stage, 3)];
      const uint64_t numerator = static_cast<uint64_t>(std::min(done, total)) * 1000 + weight;
      permille = static_cast<int>(numerator * 850 / (static_cast<uint64_t>(total) * 1000));
      permille = std::min(permille, 850);
    }
  }
  const int percent = permille / 10;

  if (!packaging && total > 0) {
    // What the device is waiting on right now. The WAP VIP path makes two
    // requests and decodes a watermark font; naming the stage is the difference
    // between "frozen" and "working".
    StrId stageId = StrId::STR_JJWXC_STAGE_BODY;
    switch (stage) {
      case 1: stageId = StrId::STR_JJWXC_STAGE_FONT; break;
      case 2: stageId = StrId::STR_JJWXC_STAGE_DECODE; break;
      case 3: stageId = StrId::STR_JJWXC_STAGE_WRITE; break;
      default: break;
    }
    renderer.drawCenteredText(kSmallFont, y, I18N.get(stageId), true);
    y += renderer.getLineHeight(kSmallFont) + 10;
  } else {
    y += 4;
  }

  char line[192];
  if (total > 0) {
    if (packaging) {
      snprintf(line, sizeof(line), "%d%% · %zu / %zu", percent, std::min(done, total), total);
    } else {
      // The skipped count is part of the line rather than a footnote: without it a
      // run that is sailing through purchase stubs reads as a successful download,
      // and the bar reaches 100% either way.
      const size_t skipped = dlSkipped_.load();
      if (skipped > 0) {
        snprintf(line, sizeof(line), "%d%% · %zu / %zu · %zu KB · %s %zu", percent, std::min(done, total), total,
                 dlBytes_.load() / 1024, tr(STR_JJWXC_SUM_SKIPPED), skipped);
      } else {
        snprintf(line, sizeof(line), "%d%% · %zu / %zu · %zu KB", percent, std::min(done, total), total,
                 dlBytes_.load() / 1024);
      }
    }
    renderer.drawCenteredText(kListFont, y, line, true);
    y += renderer.getLineHeight(kListFont) + 12;
    // The theme's progress bar (CPY draws the water wave with the duck riding
    // the fill edge). The extra headroom above the track gives the duck room
    // without crowding the count line.
    const int barW = safe.width - 2 * metrics.contentSidePadding;
    y += 16;
    GUI.drawProgressBar(renderer, Rect{x, y, barW, 10}, static_cast<size_t>(permille), 1000, false);
    y += 26;
  } else {
    renderer.drawCenteredText(kListFont, y, tr(STR_JJWXC_BUSY_HINT), true);
    y += renderer.getLineHeight(kListFont) + 12;
  }
  if (!dlTitle_.empty()) {
    char title[128];
    snprintf(title, sizeof(title), "%s", dlTitle_.c_str());
    truncateToWidth(renderer, kSmallFont, title, safe.width - 2 * metrics.contentSidePadding);
    renderer.drawCenteredText(kSmallFont, y, title, true);
  }

  // One explicit cancel target, on screen and tappable, instead of the old
  // "press Back" sentence. Back still works; the pill is what a touch user sees.
  const int pillW = 200;
  const int pillH = 56;
  const int pillX = safe.x + (safe.width - pillW) / 2;
  const int pillY = safe.y + safe.height - pillH - 24;
  renderer.fillRoundedRect(pillX, pillY, pillW, pillH, pillH / 2, Color::Black);
  const char* cancel = tr(STR_CANCEL);
  const int cancelW = renderer.getTextWidth(kBodyFont, cancel);
  const int cancelLine = renderer.getLineHeight(kBodyFont);
  renderer.drawText(kBodyFont, pillX + (pillW - cancelW) / 2, pillY + (pillH - cancelLine) / 2, cancel, false);
  busyCancelRect_ = Rect{pillX, pillY, pillW, pillH};
}

void JjwxcActivity::render(RenderLock&&) {
  // The option dialog paints over the retained framebuffer, so it has to be
  // offered before anything clears the screen (WeReadActivity::render() uses the
  // same order).
  if (optionPopup_.processRender(renderer, mappedInput)) return;

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // BookDetail carries a photographic cover. Render that one page in a single full-page
  // 16-level pass -- exactly the home dashboard's cover path (InxRecentActivity::render) --
  // so the cover keeps its continuous tones instead of being thresholded to 1-bit by the
  // ordinary B/W push. Every other state is pure B/W chrome and stays on the cheap
  // differential push. beginGrayscale16() clears the screen itself (it retains a B/W proxy
  // for the fallback), so the explicit clear is the non-gray path only.
  //
  // This once rebooted the device on a later open of a book. Chain: the gray16 cover draw's
  // internal-heap row scratch tipped free heap under the SD face's 40 KB retain floor, so
  // SdCardFontSystem's rebind of UI_10_FONT_ID to the SD card font dropped into stub mode
  // (stubData.bitmap == nullptr); a Chinese blurb glyph whose overflow-ring slot had been
  // cleared on another task then fell through GfxRenderer::getGlyphBitmap()'s
  // `&fontData->bitmap[dataOffset]` and produced a wild, unmapped pointer (Cache error /
  // MMU entry fault). The null base is now guarded in getGlyphBitmap (it returns nullptr and
  // renderCharImpl skips the glyph), so gray16 is safe here again. Keep it that way: any new
  // large internal-heap allocation on this page can re-trigger stub mode, though it can no
  // longer fault.
  const bool gray16 = state_ == State::BookDetail && !busyScreen_ && renderer.beginGrayscale16();
  if (!gray16) renderer.clearScreen();

  switch (state_) {
    case State::Disclaimer:
      renderDisclaimer();
      break;
    case State::Downloading:
      renderProgress();
      break;
    case State::Message:
      renderMessage();
      break;
    default: {
      if (busyScreen_ && !inlineJob_) {
        renderBusy();
        break;
      }
      const char* title = tr(STR_JJWXC_TITLE);
      switch (state_) {
        case State::Shelf:
          title = shelfTitle_.empty() ? tr(STR_JJWXC_SHELF) : shelfTitle_.c_str();
          break;
        case State::Favorites: title = tr(STR_JJWXC_FAVORITES); break;
        case State::FavoritePicker: title = tr(STR_JJWXC_FAV_PICK); break;
        case State::FavoriteManage: title = tr(STR_JJWXC_FAV_MANAGE); break;
        case State::ParagraphComments: title = tr(STR_JJWXC_COMMENTS); break;
        case State::CommentList: title = tr(STR_JJWXC_COMMENTS); break;
        case State::Discover: title = tr(STR_JJWXC_DISCOVER); break;
        case State::DiscoverList:
          title = discoverTitle_.empty() ? tr(STR_JJWXC_DISCOVER) : discoverTitle_.c_str();
          break;
        case State::AdvancedSearch: title = tr(STR_JJWXC_SEARCH); break;
        case State::SearchResults:
          title = searchType_ == 2 ? tr(STR_JJWXC_SEARCH_AUTHOR) : tr(STR_JJWXC_SEARCH);
          break;
        case State::Library: title = tr(STR_JJWXC_LIBRARY); break;
        case State::Account: title = tr(STR_JJWXC_ACCOUNT); break;
        case State::BookDetail:
          // refreshed UI frame 03: the header names the page; the work's own title,
          // author and tagline live in the hero block below it.
          title = tr(STR_JJWXC_BOOK_DETAIL);
          break;
        case State::DownloadRange:
          title = refreshPending_ ? tr(STR_JJWXC_REFRESH_CACHE) : tr(STR_JJWXC_PICK_RANGE);
          break;
        case State::ChapterList: title = tr(STR_JJWXC_CHAPTERS); break;
        default: break;
      }
      char cpyTitle[96] = {};
      char subtitle[96] = {};
      if (state_ == State::ChapterList && !chapters_.empty()) {
        // The range endpoints moved into the info block, so the one header line
        // is free to say how a chapter is activated -- which differs by hardware,
        // because ReadPico has no physical Confirm key at all.
        if (UITheme::getInstance().usesCpyLayout()) {
          // Frame 14: the count rides in the title (章节列表 · 186 章); the
          // tap hint is the INX shape and stays on that side.
          char countLine[32];
          snprintf(countLine, sizeof(countLine), tr(STR_JJWXC_CHAPTERS_COUNT),
                   static_cast<unsigned long>(tocSummary_.count));
          snprintf(cpyTitle, sizeof(cpyTitle), "%s · %s", tr(STR_JJWXC_CHAPTERS), countLine);
          title = cpyTitle;
        } else {
          snprintf(subtitle, sizeof(subtitle), "%s",
                   mappedInput.hasTouch() ? tr(STR_JJWXC_TAP_CHAPTER_HINT) : tr(STR_JJWXC_KEY_CHAPTER_HINT));
        }
      } else if (state_ == State::SearchResults && searchTotal_ > 0) {
        snprintf(subtitle, sizeof(subtitle), "%s  %lu", tr(STR_JJWXC_SEARCH), static_cast<unsigned long>(searchTotal_));
      } else if (state_ == State::DiscoverList && !discover_.empty()) {
        snprintf(subtitle, sizeof(subtitle), tr(STR_JJWXC_PAGE_FMT), static_cast<unsigned long>(discoverPage_));
      }
      renderHeader(title, subtitle[0] ? subtitle : nullptr);
      renderInfoBlock();
      renderRows();
      break;
    }
  }

  // The page is already painted: a short SD job (delete) only adds its card, so
  // the user does not lose the context they acted from.
  if (busyScreen_ && inlineJob_) renderInlineJobOverlay();
  renderToast();

  if (gray16) {
    // The 16-level commit IS the presentation. It also arms a one-shot FULL GC16 for the
    // next B/W page (a 16-level frame leaves midtones on the glass that only a GC16 sweep
    // erases) and resets the sweep counter, so a stale fullRefreshPending_ must not force a
    // second FULL on top of it.
    fullRefreshPending_ = false;
    if (renderer.commitGrayscale16(false)) return;
    // Native-gray commit failed this frame: the B/W proxy the pass retained is a complete
    // page, so fall through and present that rather than leave the previous frame stuck on
    // the glass.
  }

  // A generic wait repaints the whole page every 2 s to advance its elapsed
  // line; route those pushes as TransientUi so a GC16 cannot land mid-wait (the
  // screen that follows the job sweeps right after). The download progress page
  // deliberately stays Normal: its 5%-bucketed repaints want the periodic GC16
  // to keep the continuously-moving bar clean over a multi-minute run.
  const bool genericBusy = busyScreen_ && !inlineJob_ && state_ != State::Downloading;
  const auto pushContext =
      genericBusy ? DisplayRefreshContext::TransientUi : DisplayRefreshContext::Normal;

  // Every Activity flushes its own framebuffer; ActivityManager does not. Without
  // this the panel keeps showing whatever came before, so the app looks like a
  // blank, unresponsive white screen.
  const bool full = fullRefreshPending_;
  fullRefreshPending_ = false;
  renderer.displayBuffer(full ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH, pushContext);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void JjwxcActivity::loop() {
  pollJob();

  if (!toast_.empty() && static_cast<int32_t>(millis() - toastUntilMs_) >= 0) {
    toast_.clear();
    requestUpdate();
  }

  if (entryFrame_) {
    entryFrame_ = false;
    return;  // Drop the frame whose snapshot still carries the triggering gesture.
  }
  if (waitForBackRelease_ || waitForConfirmRelease_) {
    waitForBackRelease_ = waitForBackRelease_ && mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = waitForConfirmRelease_ && mappedInput.isPressed(MappedInputManager::Button::Confirm);
    return;  // Consume the release frame too.
  }

  // The first real input ends the post-job auto-sleep grace window. main.cpp
  // restarts its own inactivity clock on exactly these events (main.cpp:1149).
  if (sleepHoldoffUntil_ != 0 &&
      (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() || mappedInput.wasScreenTouchReleased())) {
    sleepHoldoffUntil_ = 0;
  }

  // CPY: the message page draws an OK pill, so a tap must honour it the way
  // the Back key already does. One release, one dismissal; the entry-frame
  // gate above keeps the opening gesture from closing it again.
  if (state_ == State::Message && UITheme::getInstance().usesCpyLayout() &&
      mappedInput.wasScreenTouchReleased()) {
    enter(returnState_);
    return;
  }

  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) {
    optionPopupClosing_ = !optionPopup_.isActive();
    return;
  }
  if (optionPopupClosing_) {
    // Rule #13: the gesture that dismissed the dialog belongs to the dialog, so
    // gate the inherited hold and consume the triggering release.
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return;
    }
    optionPopupClosing_ = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return;
    }
  }

  // While a job owns the screen the only accepted input is cancel. Without this
  // gate a tap would still hit-test the stale rows underneath the busy page.
  if (busyScreen_ || state_ == State::Downloading) {
    // A cache removal is a sequence of unlinks; stopping it halfway leaves the
    // card in a state no UI path can explain. The operation is short enough to
    // wait out, so only network jobs and downloads expose cancel.
    const bool cancellable = !inlineJob_;
    const auto cancel = [this] {
      jobCancel_.store(true);
      // A streaming fetch stops at its next chunk, but a single-request job
      // (novelbasicinfo, login, sendDeviceCode) has no sink to poll and only ends
      // at the transport's own timeout. Acknowledge the cancel on the page rather
      // than leaving it looking frozen for up to 45 s.
      busyText_ = tr(STR_JJWXC_CANCELLED);
      requestUpdate();
    };
    if (cancellable && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancel();
      return;
    }
    int tapX = 0;
    int tapY = 0;
    if (cancellable && busyCancelRect_.width > 0 && mappedInput.wasScreenTapped(tapX, tapY) &&
        tapX >= busyCancelRect_.x && tapX < busyCancelRect_.x + busyCancelRect_.width &&
        tapY >= busyCancelRect_.y && tapY < busyCancelRect_.y + busyCancelRect_.height) {
      cancel();
      return;
    }
    if (state_ == State::Downloading) {
      // Redraw only when the bar crosses into the next 5% bucket, not once per
      // second: WeReadActivity.cpp:885 buckets the same way, and a 300-chapter
      // download would otherwise ask the panel for a refresh per chapter. The
      // phase change is a bucket of its own, so switching to packaging repaints
      // immediately. The chapter sub-stage is included so a one-chapter run
      // still moves (see dlStage_).
      const uint8_t bucket = progressBucket(dlDone_.load(), dlTotal_.load(), kProgressBuckets);
      const uint8_t phase = dlPhase_.load();
      const uint8_t stage = dlStage_.load();
      if (bucket != lastProgressBucket_ || phase != lastProgressPhase_ || stage != lastProgressStage_) {
        lastProgressBucket_ = bucket;
        lastProgressPhase_ = phase;
        lastProgressStage_ = stage;
        requestUpdate();
      }
    } else if (!inlineJob_ && static_cast<uint32_t>(millis() - lastBusyTickMs_) >= 2000) {
      // The generic page has no percentage to show; its elapsed line is what
      // distinguishes a slow request from a dead one, so it gets a cheap 0.5 Hz
      // repaint instead of the per-second one a text clock would need.
      lastBusyTickMs_ = millis();
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    switch (state_) {
      case State::Disclaimer:
      case State::Menu:
        // A pushed child (Reader, Find, shelf) returns to where it came from;
        // only the legacy replaced-root entry falls back to the app grid.
        if (activityManager.hasParentActivity()) {
          finish();
        } else {
          activityManager.goToApps();
        }
        return;
      case State::Message:
        enter(returnState_);
        return;
      case State::Account:
      case State::Library:
        enter(State::Menu);
        return;
      case State::Shelf:
      case State::SearchResults:
      case State::Discover:
      case State::AdvancedSearch:
      case State::Favorites:
        enter(State::Menu);
        return;
      case State::DiscoverList:
        enter(discoverListOrigin_);
        return;
      case State::FavoriteManage:
        enter(State::Favorites);
        return;
      case State::FavoritePicker:
        enter(State::BookDetail);
        return;
      case State::ParagraphComments:
        enter(State::ChapterList);
        return;
      case State::CommentList:
        enter(State::ParagraphComments);
        return;
      case State::BookDetail:
        if (deepLinkOpenBookDetail_) {
          // Opened from the reader's More panel: Back leaves the app instead of
          // walking into the app's own shelf/menu.
          finish();
          return;
        }
        enter(bookDetailOrigin_);
        return;
      case State::DownloadRange:
      case State::ChapterList:
        enter(State::BookDetail);
        return;
      default:
        enter(State::Menu);
        return;
    }
  }

  if (state_ == State::Disclaimer) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
    const bool tappedAccept =
        disclaimerAcceptY_ >= 0 && mappedInput.wasTapInRect(safe.x, disclaimerAcceptY_, safe.width, rowHeight());
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || tappedAccept) {
      if (!JjwxcStore::acceptDisclaimer()) {
        // The marker did not reach the card, so the next entry would ask again.
        // Say so instead of silently pretending it was recorded.
        LOG_ERR(kTag, "acceptDisclaimer failed to persist");
        showMessage(tr(STR_JJWXC_ERR_STORAGE), State::Disclaimer);
        return;
      }
      enter(State::Menu);
    }
    return;
  }

  if (state_ == State::Message) {
    int x, y;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      enter(returnState_);
    }
    return;
  }

  // Swipes page the list, matching every other long list in the repo
  // (WifiSelectionActivity.cpp:898, UiListActivity.cpp:87). Checked before the
  // row hit-test because a swipe ends in a release that wasScreenTapped never
  // reports, so the two cannot both claim one contact.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int pages = swipe == MappedInputManager::SwipeDir::Up ? 1 : -1;
    // BookDetail scrolls the whole page -- block and rows together -- rather than
    // windowing the rows, because an expanded blurb is taller than the panel and the
    // action rows have to be able to reach the top. Falls through to paging the rows when
    // the page has nothing to scroll, which is every other state (and a detail page whose
    // content already fits).
    if (!scrollDetailByPage(pages)) moveSelectionByPage(pages);
    return;
  }

  // Unified search: the three mode chips are above rowTop(), so the row touch
  // test below cannot see them; they get their own hit-test against the very
  // rects renderSearchChips() draws (rule #14). A tap that lands on a chip is
  // consumed even when it is the already-selected one, so it can never fall
  // through to the row underneath.
  if (state_ == State::AdvancedSearch) {
    Rect chips[3];
    searchChipLayout(chips, 3);
    for (int i = 0; i < 3; ++i) {
      if (chips[i].width <= 0 ||
          !mappedInput.wasTapInRect(chips[i].x, chips[i].y, chips[i].width, chips[i].height)) {
        continue;
      }
      if (searchMode_ != static_cast<uint8_t>(i)) {
        // Hold the render lock while rows_/rowIds_ are replaced: the render task
        // reads them, and the row language differs by mode (advanced shows four
        // extra rows), so a half-rebuilt vector must never reach a frame.
        RenderLock lock(*this);
        searchMode_ = static_cast<uint8_t>(i);
        rebuildRows();
        selected_ = 0;
        scroll_ = 0;
        // A chip flips between a filled and an outlined pill; a FAST partial
        // update would leave the old fill ghosting on e-ink, so ask for a full
        // sweep on the next frame.
        fullRefreshPending_ = true;
        lock.unlock();
        requestUpdate();
      }
      return;
    }
  }

  // CPY: BookDetail's "rows" are frame 12's pills, so the band hit-test is
  // per-pill rects (from the same layout the drawing uses), not the y-slot
  // rowTouch computes. A tap between pills hits nothing -- there is no other
  // touch target below the block on this page.
  const bool cpyDetail = UITheme::getInstance().usesCpyLayout() && state_ == State::BookDetail;
  if (cpyDetail) {
    // Must match renderCpyDetailPills(): a smaller array here would leave the
    // tail pills drawn but untappable.
    constexpr int kMaxPills = 12;
    Rect rects[kMaxPills];
    cpyDetailPillLayout(rowTop(), rects, kMaxPills);
    const int count = std::min(static_cast<int>(rows_.size()), kMaxPills);
    for (int i = 0; i < count; ++i) {
      // The band scrolls with the page; a band that has scrolled up must not
      // answer taps in the header its rect would otherwise reach into.
      if (rects[i].y < listTop() || rects[i].y >= detailViewportBottom()) continue;
      if (mappedInput.wasTapInRect(rects[i].x, rects[i].y, rects[i].width, rects[i].height)) {
        RenderLock lock(*this);
        selected_ = i;
        lock.unlock();
        onListConfirm();
        return;
      }
    }
  }

  int row = -1;
  // rowTouch() returns a SCREEN-RELATIVE slot -- it computes (y - top) / rowStep and
  // knows nothing about scrolling. renderRows() draws absolute row i at
  // rowTop() + (i - scroll_) * h, so the inverse is i = scroll_ + slot.
  //
  // The old call assigned the slot straight to selected_ and passed rows_.size() as the
  // bound, so on any scrolled list a tap on the k-th VISIBLE row selected absolute row
  // k. On the chapter list that is not a cosmetic mis-highlight: both books tested have
  // vipChapterid=21 and only ~9 rows fit, so reaching a VIP chapter requires scrolling,
  // and a scrolled tap could therefore only ever land on an early FREE chapter -- which
  // then downloaded successfully with no error and opened in the reader. That is the
  // reported "an unpurchased VIP chapter silently became an earlier free one". On the
  // 135-book shelf the same bug opens the wrong book.
  //
  // Sokoban (SokobanGameActivity.cpp:203, `selectedLevel = scrollOffset + touched`) and
  // WeRead both add their scroll offset here; this list was the one that did not.
  const int bound = std::min(visibleRows(), static_cast<int>(rows_.size()));
  const auto touch = cpyDetail ? MappedInputManager::RowTouch::None
                               : mappedInput.rowTouch(row, rowTop(), rowHeight(), bound);
  if (touch != MappedInputManager::RowTouch::None && row >= 0) {
    row += scroll_;
    if (row >= static_cast<int>(rows_.size())) row = -1;
  }
  if (touch == MappedInputManager::RowTouch::Down && row >= 0 && row != selected_) {
    RenderLock lock(*this);
    selected_ = row;
    ensureVisible();
    lock.unlock();
    requestUpdate();
    return;
  }
  if (touch == MappedInputManager::RowTouch::Tap && row >= 0) {
    RenderLock lock(*this);
    selected_ = row;
    lock.unlock();
    onListConfirm();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onListConfirm();
    return;
  }

  navigateListButtons();
}

// One shared navigation path for every list state.
//
// ReadPico's three strip keys publish BTN_UP / BTN_BACK / BTN_DOWN
// (freeink-sdk/libs/hardware/BoardReadPico/src/BoardReadPico.cpp:447-449), and
// MappedInputManager maps Button::PageBack / Button::PageForward onto those very
// same BTN_UP / BTN_DOWN pins (MappedInputManager.cpp:112-140). A
// wasReleased(PageForward) test placed after wasReleased(Down) can therefore
// never fire: the single-step branch consumes the edge first. That is why the old
// `moveSelection(+-visibleRows())` code was dead on every board.
//
// The repo's own answer is ButtonNavigator -- short press moves one row, holding
// the same key repeats by a full page (docs/engineering/ui-and-input.md, "Input
// Frames and Event Semantics"; UiListActivity::navigateButtons and
// WifiSelectionActivity::loop do exactly this).
void JjwxcActivity::navigateListButtons() {
  buttonNavigator_.onPreviousPress([this] { moveSelection(-1); });
  buttonNavigator_.onNextPress([this] { moveSelection(1); });
  // Held, the same keys page. On BookDetail a page moves the whole page (see the swipe
  // path in loop() for why), and only falls back to windowing the rows when there is
  // nothing to scroll.
  buttonNavigator_.onPreviousContinuous([this] {
    if (!scrollDetailByPage(-1)) moveSelectionByPage(-1);
  });
  buttonNavigator_.onNextContinuous([this] {
    if (!scrollDetailByPage(1)) moveSelectionByPage(1);
  });
}

void JjwxcActivity::moveSelectionByPage(const int pages) {
  const int count = static_cast<int>(rows_.size());
  if (count == 0) return;
  const int perPage = visibleRows();
  const int next = pages > 0 ? ButtonNavigator::nextPageIndex(selected_, count, perPage)
                             : ButtonNavigator::previousPageIndex(selected_, count, perPage);
  if (next == selected_) return;
  selected_ = next;
  ensureVisible();
  requestUpdate();
}

void JjwxcActivity::onListConfirm() {
  switch (state_) {
    case State::Menu: onMenuConfirm(); return;
    case State::Account: onAccountConfirm(); return;
    case State::Shelf:
    case State::SearchResults:
    case State::DiscoverList:
      if (selected_ >= 0 && static_cast<size_t>(selected_) < rowIds_.size()) {
        const std::string id(rowIds_[static_cast<size_t>(selected_)].c_str());
        if (state_ == State::DiscoverList && id == "__next") {
          if (!discoverNextUrl_.empty() && discover_.size() < kMaxRows) {
            loadDiscoverPage(discoverNextUrl_, /*append=*/true);
          }
          return;
        }
        openBook(id);
      }
      return;
    case State::Discover:
      if (selected_ >= 0) loadDiscoverSource(static_cast<size_t>(selected_));
      return;
    case State::Favorites:
      if (selected_ < 0 || static_cast<size_t>(selected_) >= rowIds_.size()) return;
      {
        const std::string id(rowIds_[static_cast<size_t>(selected_)].c_str());
        if (id == "__create") {
          promptNewFavoriteClass();
        } else if (id == "__manage") {
          enter(State::FavoriteManage);
        } else {
          openFavoriteFolder(id);
        }
      }
      return;
    case State::FavoritePicker:
      if (selected_ < 0 || static_cast<size_t>(selected_) >= rowIds_.size()) return;
      {
        const std::string id(rowIds_[static_cast<size_t>(selected_)].c_str());
        for (const auto& c : favClasses_) {
          if (id == c.id) {
            addToFavoriteClass(c.id, c.name);
            return;
          }
        }
      }
      return;
    case State::FavoriteManage:
      showFavoriteClassActions();
      return;
    case State::ParagraphComments:
      if (selected_ >= 0 && static_cast<size_t>(selected_) < paragraphs_.size()) {
        openCommentList(static_cast<uint32_t>(strtoul(paragraphs_[selected_].paragraphId, nullptr, 10)));
      }
      return;
    case State::CommentList:
      if (selected_ >= 0 && static_cast<size_t>(selected_) < rowIds_.size()) {
        const std::string id(rowIds_[static_cast<size_t>(selected_)].c_str());
        if (id == "__more") {
          loadMoreComments();
        }
      }
      return;
    case State::AdvancedSearch: {
      if (selected_ < 0 || static_cast<size_t>(selected_) >= rowIds_.size()) return;
      const std::string id(rowIds_[static_cast<size_t>(selected_)].c_str());
      if (id == "advkeyword") promptAdvKeyword();
      else if (id == "advinclude") promptAdvInclude();
      else if (id == "advexclude") promptAdvExclude();
      else if (id == "advfinish") cycleAdvFinish();
      else if (id == "advsort") cycleAdvSort();
      else if (id == "advrun") startAdvancedSearch();
      return;
    }
    case State::Library:
      // The downloaded list is an entry point, not a shortcut into the reader:
      // open the detail page so chapters can be refreshed/deleted or the local
      // copy opened deliberately.
      if (selected_ >= 0 && static_cast<size_t>(selected_) < rowIds_.size()) openBook(rowIds_[selected_].c_str());
      return;
    case State::BookDetail: showBookActions(); return;
    case State::DownloadRange: {
      if (selected_ < 0 || static_cast<size_t>(selected_) >= rowIds_.size()) return;
      // Copied, not aliased: rowIds_ holds jjwxc::PsramString now, and the dispatch
      // below is a chain of `id == "literal"` comparisons. Binding a const char* here
      // instead would still compile and would compare pointers, so every row would
      // silently fall through to the default branch.
      const std::string id(rowIds_[static_cast<size_t>(selected_)].c_str());
      const CacheMode mode = refreshPending_ ? CacheMode::Refresh : CacheMode::Incremental;
      if (id == "whole") {
        startCaching(0, tocSummary_.count > 0 ? tocSummary_.count - 1 : 0, /*openAfter=*/false, mode);
      } else if (id == "firstn") {
        promptFirstN();
      } else if (id == "uncached") {
        if (!tocSummary_.hasPending) {
          showMessage(tr(STR_JJWXC_ALL_CACHED), State::DownloadRange);
          return;
        }
        startCaching(tocSummary_.firstPending, tocSummary_.lastPending, /*openAfter=*/false, mode);
      } else if (id == "retry") {
        // Explicitly asked for, so the range is the refused chapters only and the
        // mode is what makes the run touch nothing else. This is the only path that
        // re-requests a recorded refusal, which over WAP costs nothing (an
        // unpurchased chapter is declined with 302 -> /buy) but is how a chapter
        // bought later, on the phone, ever reaches the card -- which is why it is
        // its own row rather than part of "uncached".
        startCaching(tocSummary_.firstPending, tocSummary_.lastPending, /*openAfter=*/false, CacheMode::RetryBlocked);
      } else if (id == "custom") {
        promptRangeStart();
      }
      return;
    }
    case State::ChapterList: {
      if (selected_ < 0 || static_cast<size_t>(selected_) >= rowIds_.size()) return;
      const std::string id(rowIds_[static_cast<size_t>(selected_)].c_str());
      if (id == "__prevpage") {
        chapterWindow_ -= chapterPageSpan_;
        if (chapterWindow_ < 0) chapterWindow_ = 0;
        rebuildRows();
        selected_ = chapterRowStart_;
        scroll_ = 0;
        requestUpdate();
        return;
      }
      if (id == "__nextpage") {
        chapterWindow_ += chapterPageSpan_;
        rebuildRows();
        selected_ = chapterRowStart_;
        scroll_ = 0;
        requestUpdate();
        return;
      }
      if (id == "__jump") {
        promptChapterJump();
        return;
      }
      showChapterActions();
      return;
    }
    default: return;
  }
}

void JjwxcActivity::onMenuConfirm() {
  if (selected_ < 0 || static_cast<size_t>(selected_) >= rowIds_.size()) return;
  const std::string id(rowIds_[selected_].c_str());
  if (id == "shelf") {
    // The menu's shelf is always the unfiltered favourites view; a folder view is
    // entered from the favourites page instead.
    shelfClassId_.clear();
    shelfTitle_.clear();
    loadShelf();
  } else if (id == "favorites") loadFavoriteClasses(State::Favorites);
  else if (id == "discover") enter(State::Discover);
  else if (id == "search") enter(State::AdvancedSearch);
  else if (id == "library") enter(State::Library);
  else if (id == "account") enter(State::Account);
}

void JjwxcActivity::onAccountConfirm() {
  if (selected_ < 0 || static_cast<size_t>(selected_) >= rowIds_.size()) return;
  const std::string id(rowIds_[selected_].c_str());
  if (id == "last") loginWithSavedAccount();
  else if (id == "login") {
    // Prefilled from the saved shortcut: an on-screen keyboard on e-ink is slow and
    // the common case is the same account with a fresh password, not a different
    // person. Clearing the field is one key press.
    if (accountName_.empty() && savedAccount_.valid()) accountName_ = savedAccount_.loginName;
    promptAccountName();
  }
  else if (id == "logout") promptLogout();
  else if (id == "authapprove") startMainDeviceApproval();
  else if (id == "clearaccount") promptClearAccount();
  else if (id == "clearpay") promptClearPayPassword();
}

void JjwxcActivity::loadLibrary() {
  books_.clear();
  JjwxcStore::listNovels(&JjwxcActivity::onNovelEntry, this, kMaxLocalBooks);
}

// Hands a finished file to the built-in reader. While Wi-Fi is up this mirrors
// WeReadActivity::openBook() (WeReadActivity.cpp:1357-1380) exactly: persist the
// target and silent-restart into the reader. The restart is not cosmetic -- it is
// what reclaims the radio's heap for the reader and what restarts main.cpp's
// inactivity clock, so a download that outlasted the sleep timeout does not put
// the device straight back to sleep on the reader's first frame.
void JjwxcActivity::handOffToReader(const std::string& path) {
  // Reading is offline: drop the TLS session first so the reader gets the heap back.
  transport_.resetSession();
#ifdef CROSSPOINT_EMULATED
  activityManager.goToReader(path);
#else
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    activityManager.goToReader(path);
    return;
  }

  APP_STATE.openEpubPath = path;
  APP_STATE.readerActivityLoadCount = 0;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR(kTag, "failed to persist reader target; opening without restart");
    activityManager.goToReader(path);
    return;
  }

  WiFi.disconnect(false);
  delay(30);
  silentRestartToReader();
#endif
}

void JjwxcActivity::openLocalBook(const std::string& novelId) {
  JjwxcStore::NovelRecord rec;
  if (!JjwxcStore::loadNovel(novelId.c_str(), rec)) {
    showMessage(tr(STR_JJWXC_NOT_DOWNLOADED), State::Library);
    return;
  }
  const std::string path = JjwxcStore::finalBookPath(rec);
  if (!Storage.exists(path.c_str())) {
    LOG_ERR(kTag, "novel.bin present but the EPUB is not: %s", path.c_str());
    showMessage(tr(STR_JJWXC_NOT_DOWNLOADED), State::Library);
    return;
  }
  // The map is derived from toc.bin, so an EPUB packaged before the all-chapters
  // spine change still gets a correct mapping. Best effort: a write failure only
  // disables comment bubbles/progress jumps, never reading.
  JjwxcStore::writeSpineMap(novelId.c_str());
  // Reading is offline and the reader wants the heap: no network step, no job.
  handOffToReader(path);
}

// Second confirmation, then the two failure modes WeRead separates in
// performLogout() (WeReadActivity.cpp:1507): a token that could not be deleted is
// retryable and must not be reported as "signed out".
void JjwxcActivity::promptLogout() {
  // ActivityManager owns the child across frames, so this has to be a fallible
  // heap allocation rather than a stack object (AGENTS.md rule #2).
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_JJWXC_LOGOUT_CONFIRM),
                                                              tr(STR_JJWXC_LOGOUT_KEEP_DOWNLOADS));
  if (!confirmation) {
    LOG_ERR(kTag, "OOM: logout confirmation (%u bytes)", (unsigned)sizeof(ConfirmationActivity));
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    // The child can close on a key edge; arm the release barrier so that same
    // edge cannot also drive this activity (rule #13).
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    performLogout();
  });
}

void JjwxcActivity::performLogout() {
  // The WAP jar is the session now, so it goes first and both halves have to succeed:
  // the in-memory one, or haveSession() keeps saying "signed in" and the account page
  // keeps offering Sign out; and the file, or the next boot restores the session the
  // user just ended. `device` is deliberately untouched -- that is the same split whose
  // absence made every login demand a fresh verification code.
  bool sessionCleared = true;
  if (jar_ != nullptr) jar_->clear();
  if (!JjwxcStore::clearCookies()) {
    LOG_ERR(kTag, "clearCookies failed");
    sessionCleared = false;
  }
  if (!JjwxcStore::clearCredentials()) {
    // The token is still on the card, so the session is NOT over. Keep cred_ and
    // send the user back to the account page, where signing out again retries.
    LOG_ERR(kTag, "clearCredentials failed");
    sessionCleared = false;
  }
  if (!sessionCleared) {
    showMessage(tr(STR_JJWXC_LOGOUT_FAILED), State::Account);
    return;
  }
  cred_ = jjwxc::Credentials{};
  // Dropping the whole struct would drop the fingerprint with it, and signing in
  // again without leaving the app never re-runs onEnter() -- so startLogin() would
  // mint a fresh identity and overwrite the device file. Reload it here for the same
  // reason it lives in its own file: reusing one fingerprint is what stops JJWXC
  // demanding a verification code on every login.
  JjwxcStore::DeviceIdentity device;
  if (JjwxcStore::loadDevice(device)) {
    snprintf(cred_.identifiers, sizeof(cred_.identifiers), "%s", device.identifiers);
    snprintf(cred_.readerId, sizeof(cred_.readerId), "%s", device.readerId);
  }
  // The transport may still hold a keep-alive TLS session to m.jjwxc.net that was
  // opened with the old cookie header. Nothing replays a stale Cookie header -- the
  // jar is rebuilt per request -- but dropping the connection costs one handshake and
  // removes any doubt about which session a following request belongs to.
  transport_.resetSession();
  // The remembered payment password belongs to the account that just left;
  // leaving it would let the next account spend points with a stale secret.
  jjwxcpay::clear();
  purchaseAttemptInFlight_ = false;
  purchaseUsedSavedPassword_ = false;
  pendingPaySave_ = false;
  jobPurchaseWrongPassword_ = false;
  memset(lastPurchasePassword_, 0, sizeof(lastPurchasePassword_));
  // The previous account's lists must not survive the sign-out. The chapter cache,
  // the generated EPUBs and the disclaimer acceptance record are deliberately kept,
  // and so is savedAccount_ -- it is the login shortcut, and "清除已保存的账号" is
  // its own explicit action, so a logout never takes a stored password with it by
  // accident.
  shelf_.clear();
  search_.clear();
  searchTotal_ = 0;
  showMessage(tr(STR_JJWXC_LOGGED_OUT), State::Menu);
}

// The only path that removes a stored password. Confirmed separately because it
// is not reversible and because the file is obfuscated rather than encrypted: the
// point of the action is to make it gone, not unreadable.
void JjwxcActivity::promptClearAccount() {
  if (!savedAccount_.valid()) {
    enter(State::Account);
    return;
  }
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_JJWXC_CLEAR_ACCOUNT_CONFIRM),
                                                              savedAccount_.loginName.c_str());
  if (!confirmation) {
    LOG_ERR(kTag, "OOM: clear-account confirmation (%u bytes)", (unsigned)sizeof(ConfirmationActivity));
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    // The file is a few dozen bytes and this is a one-off action, so the SD write
    // stays on the UI thread instead of costing a worker task.
    if (!JjwxcStore::clearAccount()) {
      showMessage(tr(STR_JJWXC_CLEAR_ACCOUNT_FAILED), State::Account);
      return;
    }
    savedAccount_ = JjwxcStore::SavedAccount{};
    password_.clear();
    saveScope_ = JjwxcStore::SaveScope::AccountOnly;
    jjwxcpay::clear();
    showMessage(tr(STR_JJWXC_CLEAR_ACCOUNT_DONE), State::Account);
  });
}

void JjwxcActivity::promptClearPayPassword() {
  if (!jjwxcpay::available()) {
    enter(State::Account);
    return;
  }
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(
      renderer, mappedInput, tr(STR_JJWXC_CLEAR_PAY_PASSWORD), tr(STR_JJWXC_CLEAR_PAY_PASSWORD_HINT));
  if (!confirmation) {
    LOG_ERR(kTag, "OOM: clear payment password confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    if (!jjwxcpay::clear()) {
      showMessage(tr(STR_JJWXC_PAY_PASSWORD_CLEAR_FAILED), State::Account);
      return;
    }
    showMessage(tr(STR_JJWXC_PAY_PASSWORD_CLEARED), State::Account);
  });
}

// ---------------------------------------------------------------------------
// Book / chapter actions
// ---------------------------------------------------------------------------

void JjwxcActivity::showBookActions() {
  if (selected_ < 0 || static_cast<size_t>(selected_) >= rowIds_.size()) return;
  const std::string id(rowIds_[static_cast<size_t>(selected_)].c_str());
  if (id == "intro") {
    toggleBlurb();
  } else if (id == "cache") {
    loadChapters(State::DownloadRange);
  } else if (id == "chapters") {
    loadChapters(State::ChapterList);
  } else if (id == "open") {
    openLocalBook(novelId_);
  } else if (id == "refresh") {
    promptRefresh();
  } else if (id == "favadd") {
    promptFavoritePick();
  } else if (id == "favdel") {
    promptRemoveFavorite();
  } else if (id == "cloudjump") {
    jumpToCloudProgress();
  } else if (id == "synclist") {
    loadChapters(State::ChapterList);
  } else if (id == "updatecurrent") {
    refreshCurrentChapter();
  } else if (id == "commentbodies") {
    promptPrefetchCommentBodies();
  } else if (id == "deletecache") {
    promptDeleteNovelCache();
  }
}

// Expands or collapses the detail block. This is the one input on this page that changes
// the page's own geometry, so it invalidates everything derived from it: the cached
// layout (which rebuildRows() drops), the row label, and the scroll offset -- an offset
// that was sane for a 400-px block is not sane for a 5,000-px one, and a stale value
// would leave the user looking at blank panel with the rows unreachable.
void JjwxcActivity::toggleBlurb() {
  blurbExpanded_ = !blurbExpanded_;
  detailScroll_ = 0;
  // Rebuilds the row (its subtitle is the state) and drops detailLayoutValid_, so the
  // next geometry read recomputes in the new mode.
  rebuildRows();
  if (blurbExpanded_) {
    // Put the paragraph the user just asked for at the top of the viewport. An expanded
    // blurb sits under a full (uncut) fact column, so at offset 0 it is entirely below
    // the fold and the only visible effect of the tap would be an ellipsis disappearing
    // -- which reads as a tap that did nothing. Clamped, so a short expansion that
    // already fits does not move the page at all.
    detailScroll_ = std::max(0, detailLayout().blurbY);
  }
  // Collapsing starts back at the top: the cover, the facts and the action rows are what
  // the collapsed page is for, and an offset that was sane for a 5,000-px block is not
  // sane for a 700-px one.
  clampDetailScroll();
  scroll_ = 0;
  if (selected_ >= static_cast<int>(rows_.size())) {
    selected_ = rows_.empty() ? 0 : static_cast<int>(rows_.size()) - 1;
  }
  // Deliberately NOT ensureVisible(): that follows the selection, and expanding makes the
  // block thousands of pixels tall, so "show the selected row" would throw the user
  // straight past the paragraph they just asked to read. Navigation moves the offset
  // afterwards, which is what the page scroll is for.
  // A screenful of prose appearing or disappearing is worth a full waveform; the fast LUT
  // ghosts the previous layout badly at this scale.
  fullRefreshPending_ = true;
  requestUpdate();
}

// Refresh is an explicit choice, never a consequence of the EPUB already
// existing. It refetches every chapter in range, including the ones recorded as
// unpurchased or locked, and updates savedAt/remoteSize per chapter.
void JjwxcActivity::promptRefresh() {
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_JJWXC_REFRESH_CONFIRM),
                                                              tr(STR_JJWXC_REFRESH_BODY));
  if (!confirmation) {
    LOG_ERR(kTag, "OOM: refresh confirmation (%u bytes)", (unsigned)sizeof(ConfirmationActivity));
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    // Refresh means "re-download the chapters I already have", not "pick a range".
    // The range picker is gone from this path: it used to make the user choose a
    // range after already confirming the refresh.
    startCachedRefresh();
  });
}

void JjwxcActivity::startCachedRefresh() {
  if (tocSummary_.count == 0) {
    showMessage(tr(STR_JJWXC_NO_CATALOGUE), State::BookDetail);
    return;
  }
  withNetwork(
      [this] {
        launchCacheJob(0, tocSummary_.count - 1, /*openAfter=*/false, CacheMode::RefreshCached,
                       State::BookDetail);
      },
      State::BookDetail);
}

void JjwxcActivity::promptFirstN() {
  if (tocSummary_.count == 0) {
    showMessage(tr(STR_JJWXC_NO_CATALOGUE), State::BookDetail);
    return;
  }
  const int maximum = static_cast<int>(std::min<uint32_t>(tocSummary_.count, 999999));
  auto handler = [this, maximum](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      enter(State::DownloadRange);
      return;
    }
    const auto& kb = std::get<KeyboardResult>(result.data);
    const int n = parseOrdinal(kb.text, maximum);
    if (n < 0) {
      showMessage(tr(STR_JJWXC_RANGE_INVALID), State::DownloadRange);
      return;
    }
    firstN_ = n;
    startCaching(0, static_cast<uint32_t>(n) - 1, /*openAfter=*/false,
                 refreshPending_ ? CacheMode::Refresh : CacheMode::Incremental);
  };
  char initial[8];
  snprintf(initial, sizeof(initial), "%d", firstN_);
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_RANGE_COUNT), initial, 6, InputType::Ascii);
}

void JjwxcActivity::promptRangeStart() {
  if (tocSummary_.count == 0) {
    showMessage(tr(STR_JJWXC_NO_CATALOGUE), State::BookDetail);
    return;
  }
  const int maximum = static_cast<int>(tocSummary_.count);
  auto handler = [this, maximum](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      enter(State::DownloadRange);
      return;
    }
    const auto& kb = std::get<KeyboardResult>(result.data);
    const int from = parseOrdinal(kb.text, maximum);
    if (from < 0) {
      showMessage(tr(STR_JJWXC_RANGE_INVALID), State::DownloadRange);
      return;
    }
    rangeFrom_ = from - 1;
    if (rangeTo_ < rangeFrom_) rangeTo_ = rangeFrom_;
    promptRangeEnd();
  };
  char initial[8];
  snprintf(initial, sizeof(initial), "%d", rangeFrom_ >= 0 ? rangeFrom_ + 1 : 1);
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_RANGE_START), initial, 6, InputType::Ascii);
}

void JjwxcActivity::promptRangeEnd() {
  const int maximum = static_cast<int>(tocSummary_.count);
  auto handler = [this, maximum](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      enter(State::DownloadRange);
      return;
    }
    const auto& kb = std::get<KeyboardResult>(result.data);
    const int to = parseOrdinal(kb.text, maximum);
    if (to < 0) {
      showMessage(tr(STR_JJWXC_RANGE_INVALID), State::DownloadRange);
      return;
    }
    rangeTo_ = to - 1;
    startCaching(static_cast<uint32_t>(rangeFrom_), static_cast<uint32_t>(rangeTo_), /*openAfter=*/false,
                 refreshPending_ ? CacheMode::Refresh : CacheMode::Incremental);
  };
  char initial[8];
  snprintf(initial, sizeof(initial), "%d", rangeTo_ >= 0 ? rangeTo_ + 1 : maximum);
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_RANGE_END), initial, 6, InputType::Ascii);
}

void JjwxcActivity::promptChapterJump() {
  if (chapters_.empty()) return;
  const int maximum = static_cast<int>(chapters_.size());
  auto handler = [this, maximum](const ActivityResult& result) {
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) {
      enter(State::ChapterList);
      return;
    }
    const auto& kb = std::get<KeyboardResult>(result.data);
    const int n = parseOrdinal(kb.text, maximum);
    if (n < 0) {
      showMessage(tr(STR_JJWXC_RANGE_INVALID), State::ChapterList);
      return;
    }
    // Target at the bottom of the window when it is near the end, so the last
    // chapter does not open as the only row on a mostly empty page.
    const int span = chapterPageSpan_ > 0 ? chapterPageSpan_ : static_cast<int>(kMaxRows);
    const int lastStart = maximum > span ? maximum - span : 0;
    chapterWindow_ = std::clamp(n - 1, 0, lastStart);
    enter(State::ChapterList);
    selected_ = chapterRowStart_ + (n - 1 - chapterWindow_);
    ensureVisible();
    requestUpdate();
  };
  char initial[12];
  snprintf(initial, sizeof(initial), "%d", chapterWindow_ + 1);
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_JUMP_PROMPT), initial, 7, InputType::Ascii);
}

// Chapter actions. ReadPico maps its three strip keys to BTN_UP / BTN_BACK /
// BTN_DOWN only (freeink-sdk BoardReadPico.cpp:447-449), so there is no physical
// Confirm on that board and a tap is the only way in -- which is why the popup,
// not a key sequence, owns every chapter action.
void JjwxcActivity::showChapterActions() {
  const int chapterIndex = chapterIndexForRow(selected_);
  if (chapterIndex < 0) return;
  const int rowIndex = selected_;
  // OptionPopup copies every label into its own std::string during show(), so
  // these tr() pointers only have to stay alive for the call.
  const char* options[7];
  int optionCount = 0;
  options[optionCount++] = tr(STR_JJWXC_CHAPTER_READ);
  options[optionCount++] = tr(STR_JJWXC_CHAPTER_CACHE);
  options[optionCount++] = tr(STR_JJWXC_CHAPTER_FROM);
  options[optionCount++] = tr(STR_JJWXC_CHAPTER_TO);
  options[optionCount++] = tr(STR_JJWXC_CHAPTER_DOWNLOAD_RANGE);
  const uint32_t ordinal = static_cast<uint32_t>(chapterIndex);
  const uint8_t rawState = static_cast<size_t>(chapterIndex) < chapterStates_.size()
                               ? chapterStates_[static_cast<size_t>(chapterIndex)]
                               : 0;
  const bool canBuy = haveSession() && chapters_[static_cast<size_t>(chapterIndex)].isVip &&
                      rawState != static_cast<uint8_t>(JjwxcStore::ChapterState::Cached) &&
                      rawState != static_cast<uint8_t>(JjwxcStore::ChapterState::Locked);
  int purchaseIndex = -1;
  if (canBuy) {
    purchaseIndex = optionCount;
    options[optionCount++] = tr(STR_JJWXC_BUY_CHAPTER);
  }
  // The chapter name is the dialog's subject line, so the user can see which
  // chapter the actions apply to.
  optionPopup_.show(tr(STR_JJWXC_CHAPTER_ACTION), chapters_[static_cast<size_t>(chapterIndex)].name, options,
                    optionCount, 0, [this, ordinal, rowIndex, purchaseIndex](const int index) {
                      switch (index) {
                        case 0:
                          // Caches this one chapter and then opens the same single
                          // EPUB a whole-book cache produces: one artefact per
                          // novel, whatever the entry point.
                          cacheSingleChapter(ordinal, /*openAfter=*/true);
                          return;
                        case 1:
                          cacheSingleChapter(ordinal, /*openAfter=*/false);
                          return;
                        case 2:
                          rangeFrom_ = static_cast<int>(ordinal);
                          if (rangeTo_ < rangeFrom_) rangeTo_ = rangeFrom_;
                          break;
                        case 3:
                          rangeTo_ = static_cast<int>(ordinal);
                          if (rangeFrom_ < 0 || rangeFrom_ > rangeTo_) rangeFrom_ = rangeTo_;
                          break;
                        case 4:
                          if (rangeFrom_ < 0 || rangeTo_ < 0) {
                            showMessage(tr(STR_JJWXC_RANGE_INVALID), State::ChapterList);
                            return;
                          }
                          startCaching(static_cast<uint32_t>(rangeFrom_), static_cast<uint32_t>(rangeTo_),
                                       /*openAfter=*/false, CacheMode::Incremental);
                          return;
                        default:
                          // The purchase row only exists for an unpurchased VIP
                          // chapter, so its index shifts.
                          if (index == purchaseIndex) promptPurchase(ordinal);
                          return;
                      }
                      // The "●" range markers are row subtitles, so the rows have
                      // to be rebuilt for the new endpoints to show.
                      selected_ = rowIndex;
                      infoLineCount_ = -1;
                      rebuildRows();
                      ensureVisible();
                      requestUpdate();
                    });
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Login
//
// JJWXC demands a device verification code for any client it has not seen
// before, so the flow is: account -> password -> attempt -> if the server asks
// for a code (221003), send one to the bound email/phone and retry with it.
// ---------------------------------------------------------------------------

void JjwxcActivity::promptAccountName() {
  auto handler = [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto& kb = std::get<KeyboardResult>(result.data);
      accountName_ = kb.text;
    }
    if (accountName_.empty()) {
      enter(State::Account);
      return;
    }
    promptPassword();
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_ACCOUNT_NAME), accountName_, 64,
                                                    InputType::Ascii);
}

void JjwxcActivity::promptPassword() {
  auto handler = [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto& kb = std::get<KeyboardResult>(result.data);
      password_ = kb.text;
    }
    if (password_.empty()) {
      enter(State::Account);
      return;
    }
    promptSaveScope();
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_PASSWORD), "", 64, InputType::Password);
}

// "用上次账号登录". The whole point of the shortcut is to skip the keyboard, so a
// stored password goes straight to startLogin(); only the scope prompt is skipped
// too, because the answer is already on the card.
void JjwxcActivity::loginWithSavedAccount() {
  if (!savedAccount_.valid()) {
    enter(State::Account);
    return;
  }
  accountName_ = savedAccount_.loginName;
  if (!savedAccount_.hasPassword()) {
    promptPassword();
    return;
  }
  password_ = savedAccount_.password;
  saveScope_ = savedAccount_.scope;
  startLogin(false);
}

// Asked once per manual login, before the request rather than after: the answer
// decides what lands on the card, and asking afterwards would mean a failed login
// had already written a password that was never verified.
void JjwxcActivity::promptSaveScope() {
  const char* options[] = {tr(STR_JJWXC_SAVE_NOTHING), tr(STR_JJWXC_SAVE_ACCOUNT), tr(STR_JJWXC_SAVE_BOTH)};
  int initial = static_cast<int>(saveScope_);
  if (initial < 0 || initial > static_cast<int>(JjwxcStore::SaveScope::AccountAndPassword)) {
    initial = static_cast<int>(JjwxcStore::SaveScope::AccountOnly);
  }
  optionPopup_.showMessage(tr(STR_JJWXC_SAVE_SCOPE), tr(STR_JJWXC_SAVE_SCOPE_HINT), options,
                           static_cast<int>(sizeof(options) / sizeof(options[0])), initial, [this](const int index) {
                             switch (index) {
                               case 0:
                                 saveScope_ = JjwxcStore::SaveScope::Nothing;
                                 break;
                               case 1:
                                 saveScope_ = JjwxcStore::SaveScope::AccountOnly;
                                 break;
                               case 2:
                                 saveScope_ = JjwxcStore::SaveScope::AccountAndPassword;
                                 break;
                               default:
                                 // Back dismisses without firing the callback, so an
                                 // out-of-range index cannot reach here; treat it as
                                 // "abort" rather than as a silent scope change.
                                 return;
                             }
                             startLogin(false);
                           });
  requestUpdate();
}

void JjwxcActivity::promptDeviceCode() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) {
      enter(State::Account);
      return;
    }
    const auto& kb = std::get<KeyboardResult>(result.data);
    const std::string code = kb.text;
    if (code.empty()) {
      enter(State::Account);
      return;
    }
    pendingDeviceCode_ = code;
    startLogin(true);
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_JJWXC_DEVICE_CODE), "", 8, InputType::Ascii);
}

void JjwxcActivity::startLogin(bool withDeviceCode) {
  // A login that carries no code is the start of a sequence, so it re-arms the one
  // code send that sequence is allowed. A retry (withDeviceCode) must not.
  if (!withDeviceCode) deviceCodeSent_ = false;
  if (!ensureWapSession()) {
    showMessage(tr(STR_JJWXC_ERR_MEMORY), State::Account);
    return;
  }
  const std::string checkType = withDeviceCode ? pendingCheckType_ : std::string();
  const std::string code = withDeviceCode ? pendingDeviceCode_ : std::string();
  // Lengths only. The account name is an email and the password is a password; neither
  // belongs in a serial log, and the DEBUG HTTP log already prints full URLs.
  LOG_INF(kTag, "WAP login: account=%u chars, code=%u chars, checktype='%s'", (unsigned)accountName_.size(),
          (unsigned)code.size(), checkType.c_str());

  withNetwork([this, checkType, code, withDeviceCode] {
    jobWhat_ = tr(STR_JJWXC_LOGIN);
    jobNextState_ = State::Account;
    returnState_ = State::Account;
    busyText_ = tr(STR_JJWXC_LOGINING);
    launchJob(
        [this, checkType, code, withDeviceCode] {
          jjwxc::WapLoginBuffers bufs;
          bufs.form = reinterpret_cast<char*>(scratch_);
          bufs.formCap = kWapFormBytes;
          bufs.page = reinterpret_cast<char*>(scratch_ + kWapFormBytes);
          bufs.pageCap = (scratchCap_ > kWapFormBytes) ? (scratchCap_ - kWapFormBytes) : 0;
          if (bufs.pageCap > kWapPageBytes) bufs.pageCap = kWapPageBytes;
          if (scratch_ == nullptr || bufs.formCap == 0 || bufs.pageCap < 48 * 1024) {
            jobStatus_ = jjwxc::Status::TooLarge;
            jobFailKind_ = FailKind::OutOfMemory;
            return;
          }

          wap_->setNowUnix(cookieClock());
          jjwxc::WapLoginResult res;
          const jjwxc::WapLoginStatus st =
              withDeviceCode ? jjwxc::wapLoginWithCode(*wap_, accountName_.c_str(), password_.c_str(),
                                                       checkType.c_str(), code.c_str(), bufs, res)
                             : jjwxc::wapLogin(*wap_, accountName_.c_str(), password_.c_str(), bufs, res);

          // The page buffer is NEVER logged, in whole or in part. The needs-code page
          // echoes loginname and loginpass back into the form in cleartext --
          // type="password" only affects how a browser renders it -- so printing any of
          // it would put the password in the serial log and, if anyone saved it, on the
          // card.
          LOG_INF(kTag, "WAP login -> %s http=%d redirects=%u readerId=%lu cookies=%u truncated=%d cookieTrunc=%d",
                  jjwxc::wapLoginStatusText(st), res.httpStatus, (unsigned)res.redirects,
                  (unsigned long)res.readerId, (unsigned)jar_->count(cookieClock()), (int)res.pageTruncated,
                  (int)res.cookieHeaderTruncated);

          switch (st) {
            case jjwxc::WapLoginStatus::Ok:
              jobStatus_ = jjwxc::Status::Ok;
              applyLoginSuccessWorker(res);
              break;
            case jjwxc::WapLoginStatus::NeedDeviceCode:
              // Reported as a successful job: pollJob() turns needDeviceCode_ into the
              // code prompt, and a failure path would skip it.
              jobStatus_ = jjwxc::Status::Ok;
              pendingCheckType_ = res.checkType[0] != '\0' ? res.checkType : "email";
              needDeviceCode_ = true;
              break;
            case jjwxc::WapLoginStatus::Refused:
              jobStatus_ = jjwxc::Status::BadCredentials;
              jobFailKind_ = FailKind::LoginReturned;
              break;
            case jjwxc::WapLoginStatus::Network:
              jobStatus_ = jjwxc::Status::Network;
              jobDetailCode_ = res.httpStatus;
              break;
            case jjwxc::WapLoginStatus::Protocol:
            default:
              jobStatus_ = jjwxc::Status::Protocol;
              jobDetailCode_ = res.httpStatus;
              LOG_ERR(kTag, "unrecognised login page: http=%d redirects=%u truncated=%d", res.httpStatus,
                      (unsigned)res.redirects, (int)res.pageTruncated);
              break;
          }
        },
        tr(STR_JJWXC_LOGINING));
  },
              State::Account);
}

bool JjwxcActivity::hasLoginPassword() const {
  return !password_.empty() || savedAccount_.hasPassword();
}

void JjwxcActivity::applyLoginSuccessWorker(const jjwxc::WapLoginResult& res) {
  // The jar IS the session, so this is the write that makes the login survive a
  // reboot. Without it every power cycle costs another attempt against a
  // risk-controlled account.
  if (!JjwxcStore::saveCookies(*jar_)) {
    LOG_ERR(kTag, "saveCookies failed; this session will not survive a reboot");
  }
  // NAMES only, never values -- a log line must stay shareable. See startLogin()
  // for why this exists.
  {
    const uint64_t now = cookieClock();
    LOG_INF(kTag, "session cookies: sid=%d JJSESS=%d certification=%d nickname=%d openid=%d JJEVER=%d closecomment=%d",
            (int)jar_->has("sid", now), (int)jar_->has("JJSESS", now), (int)jar_->has("certification", now),
            (int)jar_->has("nickname", now), (int)jar_->has("wap_reader_openid", now), (int)jar_->has("JJEVER", now),
            (int)jar_->has("userclosecomment", now));
  }
  JjwxcStore::DeviceIdentity device;
  if (!JjwxcStore::loadDevice(device)) device = JjwxcStore::DeviceIdentity{};
  if (res.readerId != 0) snprintf(device.readerId, sizeof(device.readerId), "%lu", (unsigned long)res.readerId);
  if (!JjwxcStore::saveDevice(device)) LOG_ERR(kTag, "saveDevice failed after login");
  // Only a login the server accepted may write the shortcut.
  persistSavedAccount();
}

void JjwxcActivity::startMainDeviceApproval() {
  const std::string loginName = !accountName_.empty() ? accountName_ : savedAccount_.loginName;
  const std::string password = !password_.empty() ? password_ : savedAccount_.password;
  if (loginName.empty() || password.empty()) {
    showMessage(tr(STR_JJWXC_AUTH_NO_PASSWORD), State::Account);
    return;
  }
  accountName_ = loginName;
  password_ = password;
  withNetwork(
      [this, loginName, password] {
        jobWhat_ = tr(STR_JJWXC_AUTH_APPROVE);
        jobNextState_ = State::Account;
        returnState_ = State::Account;
        busyText_ = tr(STR_JJWXC_AUTH_WAIT);
        launchJob(
            [this, loginName, password] {
              jobStatus_ = runMainDeviceApproval(loginName, password);
              if (jobStatus_ == jjwxc::Status::Ok) jobNotice_ = tr(STR_JJWXC_AUTH_APPROVED);
            },
            tr(STR_JJWXC_AUTH_WAIT));
      },
      State::Account);
}

jjwxc::Status JjwxcActivity::runMainDeviceApproval(const std::string& loginName, const std::string& password) {
  if (!ensureWapSession()) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  jjwxc::WapLoginBuffers bufs;
  bufs.form = reinterpret_cast<char*>(scratch_);
  bufs.formCap = kWapFormBytes;
  bufs.page = reinterpret_cast<char*>(scratch_ + kWapFormBytes);
  bufs.pageCap = (scratchCap_ > kWapFormBytes) ? (scratchCap_ - kWapFormBytes) : 0;
  if (bufs.pageCap > kWapPageBytes) bufs.pageCap = kWapPageBytes;
  if (scratch_ == nullptr || bufs.formCap == 0 || bufs.pageCap < 48 * 1024) {
    jobFailKind_ = FailKind::OutOfMemory;
    return jjwxc::Status::TooLarge;
  }
  wap_->setNowUnix(cookieClock());

  char body[512];
  const size_t notifyLen = jjwxc::buildNotifyBody(loginName.c_str(), body, sizeof(body));
  if (notifyLen == 0) return jjwxc::Status::Protocol;
  auto* page = reinterpret_cast<char*>(scratch_ + kWapFormBytes);
  auto postCollect = [&](const char* url, const char* data, const size_t dataLen, const size_t cap) -> int {
    BufferCollector col{page, cap, 0, false};
    jjwxc::WapOptions opt;
    opt.contentType = jjwxc::kWapFormContentType;
    opt.referer = "https://m.jjwxc.net/my/login?login_mode=jjwxc";
    jjwxc::WapResponse res;
    const int http = wap_->post(url, data, dataLen, opt, &collectBuffer, &col, res);
    if (http < 0 || res.sawRedirect || http != 200 || col.overflow) return -1;
    page[col.len] = '\0';
    return static_cast<int>(col.len);
  };

  int len = postCollect("https://m.jjwxc.net/app.jjwxc/Wap/Login/sendVerifyNotifyToMainDevice", body, notifyLen,
                        4096);
  if (len <= 0) {
    jobDetailCode_ = len;
    return jjwxc::Status::Network;
  }
  char authKey[128];
  if (!jjwxc::parseAuthKey(page, static_cast<size_t>(len), authKey, sizeof(authKey))) {
    LOG_ERR(kTag, "main-device notify was refused (%d bytes)", len);
    return jjwxc::Status::Server;
  }
  LOG_INF(kTag, "main-device approval requested (%u-byte key)", (unsigned)strlen(authKey));

  bool approved = false;
  for (int attempt = 0; attempt < 60 && !approved; ++attempt) {
    if (jobCancel_.load()) return jjwxc::Status::Cancelled;
    const size_t askLen = jjwxc::buildAskBody(authKey, body, sizeof(body));
    if (askLen == 0) return jjwxc::Status::Protocol;
    len = postCollect("https://m.jjwxc.net/app.jjwxc/Wap/Login/askLoginRes", body, askLen, 2048);
    if (len > 0 && jjwxc::parseApproval(page, static_cast<size_t>(len))) {
      approved = true;
      break;
    }
    if (len < 0) {
      jobDetailCode_ = len;
      return jjwxc::Status::Network;
    }
    delay(1000);
  }
  if (!approved) {
    LOG_ERR(kTag, "main-device approval not granted after 60 polls");
    snprintf(jobServerMessage_, sizeof(jobServerMessage_), "%s", tr(STR_JJWXC_AUTH_FAILED));
    return jjwxc::Status::Server;
  }

  jjwxc::WapLoginResult res;
  const jjwxc::WapLoginStatus st = jjwxc::wapLoginWithAuthKey(*wap_, loginName.c_str(), password.c_str(), authKey,
                                                              bufs, res);
  LOG_INF(kTag, "WAP login (approved) -> %s http=%d redirects=%u readerId=%lu cookies=%u", jjwxc::wapLoginStatusText(st),
          res.httpStatus, (unsigned)res.redirects, (unsigned long)res.readerId, (unsigned)jar_->count(cookieClock()));
  switch (st) {
    case jjwxc::WapLoginStatus::Ok:
      applyLoginSuccessWorker(res);
      return jjwxc::Status::Ok;
    case jjwxc::WapLoginStatus::NeedDeviceCode:
      // The approval was accepted but the server still wants a code: report it as
      // such, since silently retrying would spend another approval.
      pendingCheckType_ = res.checkType[0] != '\0' ? res.checkType : "email";
      jobServerMessage_[0] = '\0';
      return jjwxc::Status::NeedDeviceCode;
    case jjwxc::WapLoginStatus::Refused:
      jobFailKind_ = FailKind::LoginReturned;
      return jjwxc::Status::BadCredentials;
    case jjwxc::WapLoginStatus::Network:
      jobDetailCode_ = res.httpStatus;
      return jjwxc::Status::Network;
    default:
      jobDetailCode_ = res.httpStatus;
      return jjwxc::Status::Protocol;
  }
}

// Worker task. Reads accountName_/password_/saveScope_, which the UI thread filled
// in before launching and cannot touch while busyScreen_ owns the input, and
// publishes only an atomic flag: savedAccount_ owns std::strings that rebuildRows()
// walks, so the copy happens in applyPersistedAccount() on the UI thread.
bool JjwxcActivity::persistSavedAccount() {
  JjwxcStore::SavedAccount acc;
  acc.scope = saveScope_;
  if (saveScope_ != JjwxcStore::SaveScope::Nothing) {
    acc.loginName = accountName_;
    if (saveScope_ == JjwxcStore::SaveScope::AccountAndPassword) acc.password = password_;
  }
  const bool ok = JjwxcStore::saveAccount(acc);
  if (!ok) {
    // Not fatal: the session itself is already on the card, so this login still
    // worked. It only means the shortcut will not be there next time.
    LOG_ERR(kTag, "saveAccount failed (scope=%d)", (int)saveScope_);
    return false;
  }
  jobAccountPersisted_.store(true);
  return true;
}

void JjwxcActivity::applyPersistedAccount() {
  if (!jobAccountPersisted_.exchange(false)) return;
  savedAccount_ = JjwxcStore::SavedAccount{};
  if (saveScope_ == JjwxcStore::SaveScope::Nothing) {
    // The user declined to store the password, so do not keep the plaintext alive
    // in RAM for the rest of the session either.
    password_.clear();
    return;
  }
  savedAccount_.scope = saveScope_;
  savedAccount_.loginName = accountName_;
  if (saveScope_ == JjwxcStore::SaveScope::AccountAndPassword) {
    savedAccount_.password = password_;
  } else {
    password_.clear();
  }
}
