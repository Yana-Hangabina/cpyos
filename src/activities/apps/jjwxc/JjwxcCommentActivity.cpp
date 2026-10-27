#include "JjwxcCommentActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Epub/JjwxcCommentSidecar.h>
#include <JjwxcCrypto.h>
#include <JjwxcWapLogin.h>
#include <Logging.h>
#include <TxtEncoding.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

#include "NetworkStartup.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include "JjwxcStore.h"
#include "JjwxcTextWrap.h"

#ifdef ARDUINO
#include <WiFi.h>
#include <esp_wifi.h>
#endif

namespace {

constexpr const char* kTag = "JJWXC";

// 64 KB of PSRAM. A 20-comment page is typically 10-30 KB of JSON once the
// bodies are in UTF-8; keeping the room means a long comment cannot turn into a
// truncated page that parses as "fewer comments than there are".
constexpr size_t kResponseBytes = 64 * 1024;
// The keyboard counts UTF-8 bytes. 900 holds 300 CJK characters (the site's
// composer limit), which the double-encoded writeComment GET expands to ~4.5 KB
// of URL (each byte becomes "%25XX") -- comfortably inside WapIo::kUrlCap.
constexpr size_t kMaxComposeBytes = 900;
constexpr size_t kMaxComposeChars = 300;
constexpr size_t kReplyBodyBytes = 2048;
// The PC reply list pages at 10; 20 keeps a typical thread on one request while
// leaving the "more" path for the rare long one.
constexpr size_t kReplyPageSize = 20;

// Matches the old in-app comment view: every request carries the same referer.
constexpr const char* kReferer = "https://m.jjwxc.net/book2/";

uint64_t cookieClock() {
  const time_t t = time(nullptr);
  // 2020-01-01. An ESP32 without NTP reports seconds since boot; feeding that to
  // the jar would compare it against a sid whose Max-Age came from a real clock
  // and declare the session expired on every cold boot.
  if (t < 1577836800) return 0;
  return static_cast<uint64_t>(t);
}

struct BodyBuffer {
  char* data = nullptr;
  size_t cap = 0;
  size_t len = 0;
  bool overflow = false;
};

// Tolerant `"code"` reader. The response is JSON, but this deliberately mirrors
// the approach in JjwxcComments.cpp rather than pulling in a second JSON parser:
// the value may be a string or a number, and only a handful of codes matter.
int jsonCode(const char* body) {
  const char* key = strstr(body, "\"code\"");
  if (key == nullptr) return -1;
  const char* p = strchr(key, ':');
  if (p == nullptr) return -1;
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '"') ++p;
  int value = 0;
  bool digits = false;
  while (*p >= '0' && *p <= '9') {
    value = value * 10 + (*p - '0');
    digits = true;
    ++p;
  }
  return digits ? value : -1;
}

// commentTotal lives outside the array. Same tolerant scan as the original view
// (JjwxcActivity::fetchCommentList), kept here rather than changing the tested
// parser's contract.
// The nickname cookie is percent-encoded UTF-8. The write endpoints take the
// display name as a plain form value; sending "%E4%B8%AD" would post the escape
// text as the author.
void truncateText(const GfxRenderer& renderer, const int fontId, char* buf, const int maxWidth) {
  if (buf == nullptr || maxWidth <= 0) {
    if (buf != nullptr) buf[0] = '\0';
    return;
  }
  const size_t total = strlen(buf);
  int width = 0;
  size_t i = 0;
  while (i < total) {
    const auto lead = static_cast<unsigned char>(buf[i]);
    size_t len = 1;
    if (lead >= 0xF0) len = 4;
    else if (lead >= 0xE0) len = 3;
    else if (lead >= 0xC0) len = 2;
    if (i + len > total) len = total - i;
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

size_t percentDecode(const char* in, size_t len, char* out, size_t outCap) {
  size_t o = 0;
  for (size_t i = 0; i < len && o + 1 < outCap; ++i) {
    if (in[i] == '%' && i + 2 < len) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
      };
      const int hi = hex(in[i + 1]);
      const int lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out[o++] = static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out[o++] = in[i];
  }
  out[o] = '\0';
  return o;
}

size_t utf8CodePoints(const char* text) {
  size_t count = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
    // Count lead bytes; continuation bytes are 10xxxxxx.
    if ((*p & 0xC0) != 0x80) ++count;
  }
  return count;
}

uint32_t jsonCommentTotal(const char* body) {
  const char* key = strstr(body, "\"commentTotal\"");
  if (key == nullptr) return 0;
  const char* p = strchr(key, ':');
  if (p == nullptr) return 0;
  ++p;
  while (*p == ' ' || *p == '"') ++p;
  uint32_t total = 0;
  while (*p >= '0' && *p <= '9') total = total * 10 + static_cast<uint32_t>(*p++ - '0');
  return total;
}

}  // namespace

JjwxcCommentActivity::JjwxcCommentActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string novelId, const uint32_t chapterId,
                                           const uint32_t paragraphId, const uint16_t paragraphIndex)
    : Activity("JjwxcComment", renderer, mappedInput),
      novelId_(std::move(novelId)),
      chapterId_(chapterId),
      paragraphId_(paragraphId),
      paragraphIndex_(paragraphIndex) {}

bool JjwxcCommentActivity::ensureSession() {
  if (jar_ != nullptr && wapIo_ != nullptr && wap_ != nullptr) return true;
  // Placement-new into PSRAM: the jar is 2.3 KB and the WAP io buffers ~15 KB
  // (the write-comment URL alone needs 12 KB), and this activity runs next to
  // the reader, whose internal RAM budget is the scarce resource (AGENTS.md
  // section 7).
  if (jar_ == nullptr) {
    auto buf = memory::makePsramByteBufferNoThrow(sizeof(jjwxc::CookieJar));
    if (!buf) {
      LOG_ERR(kTag, "no PSRAM for the cookie jar");
      return false;
    }
    jar_ = new (buf.release()) jjwxc::CookieJar();
  }
  if (wapIo_ == nullptr) {
    auto buf = memory::makePsramByteBufferNoThrow(sizeof(jjwxc::WapIo));
    if (!buf) {
      LOG_ERR(kTag, "no PSRAM for the WAP io buffers");
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
  return true;
}

void JjwxcCommentActivity::releaseSession() {
  if (wap_ != nullptr) {
    delete wap_;
    wap_ = nullptr;
  }
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

bool JjwxcCommentActivity::loadCachedComments() {
  auto buffer = memory::makePsramByteBufferNoThrow(kMaxComments * sizeof(JjwxcCommentSidecar::Bodies::Item));
  if (!buffer) return false;
  auto* items = reinterpret_cast<JjwxcCommentSidecar::Bodies::Item*>(buffer.get());
  size_t count = 0;
  uint32_t total = 0;
  if (!JjwxcStore::loadCommentBodies(novelId_.c_str(), chapterId_, paragraphId_, items, kMaxComments, count, total)) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    jjwxc::CommentItem item;
    strncpy(item.commentId, items[i].commentId, sizeof(item.commentId) - 1);
    strncpy(item.author, items[i].author, sizeof(item.author) - 1);
    strncpy(item.body, items[i].body, sizeof(item.body) - 1);
    strncpy(item.ip, items[i].ip, sizeof(item.ip) - 1);
    strncpy(item.date, items[i].date, sizeof(item.date) - 1);
    item.agreeNum = items[i].agreeNum;
    item.disagreeNum = items[i].disagreeNum;
    item.replyTotal = items[i].replyTotal;
    item.readerAgree = items[i].readerAgree;
    item.isAuthor = items[i].isAuthor;
    comments_.push_back(item);
  }
  if (comments_.empty()) return false;
  commentTotal_ = total > comments_.size() ? total : static_cast<uint32_t>(comments_.size());
  LOG_INF(kTag, "comment cache: para=%lu %u shown / %u total", (unsigned long)paragraphId_,
          (unsigned)comments_.size(), (unsigned)commentTotal_);
  return true;
}

void JjwxcCommentActivity::persistComments() {
  if (comments_.empty()) return;
  auto buffer = memory::makePsramByteBufferNoThrow(comments_.size() * sizeof(JjwxcCommentSidecar::Bodies::Item));
  if (!buffer) {
    LOG_ERR(kTag, "no PSRAM for the comment cache image");
    return;
  }
  auto* items = reinterpret_cast<JjwxcCommentSidecar::Bodies::Item*>(buffer.get());
  for (size_t i = 0; i < comments_.size(); ++i) {
    snprintf(items[i].commentId, sizeof(items[i].commentId), "%s", comments_[i].commentId);
    snprintf(items[i].author, sizeof(items[i].author), "%s", comments_[i].author);
    snprintf(items[i].date, sizeof(items[i].date), "%s", comments_[i].date);
    snprintf(items[i].ip, sizeof(items[i].ip), "%s", comments_[i].ip);
    snprintf(items[i].body, sizeof(items[i].body), "%s", comments_[i].body);
    items[i].agreeNum = comments_[i].agreeNum;
    items[i].disagreeNum = comments_[i].disagreeNum;
    items[i].replyTotal = comments_[i].replyTotal;
    items[i].readerAgree = comments_[i].readerAgree;
    items[i].isAuthor = comments_[i].isAuthor;
  }
  JjwxcStore::saveCommentBodies(novelId_.c_str(), chapterId_, paragraphId_, items, comments_.size(), commentTotal_);
}

void JjwxcCommentActivity::onEnter() {
  Activity::onEnter();

  response_ = memory::makePsramByteBufferNoThrow(kResponseBytes);
  urlBuf_ = memory::makePsramByteBufferNoThrow(jjwxc::WapIo::kUrlCap);
  bodyBuf_ = memory::makePsramByteBufferNoThrow(kReplyBodyBytes);
  if (!response_ || !urlBuf_ || !bodyBuf_) {
    fail(tr(STR_JJWXC_ERR_MEMORY));
    return;
  }

  // Reserve up front: a vector growth through PsramAllocator is a throwing
  // allocation, and -fno-exceptions turns that into abort(). incoming_ is the
  // worker's parse target, so it must be ready before the first task starts.
  constexpr size_t kReserveBytes = kMaxComments * sizeof(jjwxc::CommentItem);
  constexpr size_t kReplyReserveBytes = kMaxReplies * sizeof(jjwxc::CommentReplyItem);
  if (!memory::psramHasHeadroom(kReserveBytes * 2 + kReplyReserveBytes * 2, sizeof(jjwxc::CommentItem), 64 * 1024)) {
    fail(tr(STR_JJWXC_ERR_MEMORY));
    return;
  }
  comments_.reserve(kMaxComments);
  incoming_.reserve(kMaxComments);
  replies_.reserve(kMaxReplies);
  incomingReplies_.reserve(kMaxReplies);

  // Cached comments are the normal path: no radio, no cookie jar, no TLS.
  if (loadCachedComments()) {
    showingCache_ = true;
    state_ = State::Ready;
    requestUpdate();
    return;
  }

  if (!ensureSession()) {
    fail(tr(STR_JJWXC_ERR_MEMORY));
    return;
  }
  const uint64_t now = cookieClock();
  if (!JjwxcStore::loadCookies(*jar_, now) || !JjwxcStore::hasSession(*jar_, now)) {
    fail(tr(STR_JJWXC_NEED_LOGIN));
    return;
  }

  state_ = State::Loading;
  beginWifi(WifiPurpose::FirstFetch);
}

void JjwxcCommentActivity::beginWifi(const WifiPurpose purpose) {
  NetworkStartup::prepare(renderer);
  wifiPurpose_ = purpose;
  if (transport_.networkReady()) {
    if (purpose == WifiPurpose::FirstFetch) state_ = State::Loading;
    requestUpdate();
    if (purpose == WifiPurpose::Action) {
      startWorker();
    } else if (view_ == View::Replies) {
      startRepliesFetch();
    } else {
      startFetch(0, /*append=*/false);
    }
    return;
  }
  wifiBroughtUp_ = true;
  if (!startActivityForResultWith<WifiSelectionActivity>(
          [this](const ActivityResult& result) { onWifiResult(result); })) {
    if (purpose == WifiPurpose::Action) {
      pendingAction_ = ActionKind::None;
      refreshing_ = false;
      replyLoading_ = false;
      showFeedback(tr(STR_JJWXC_NO_WIFI));
    } else if (purpose == WifiPurpose::Refresh && !comments_.empty()) {
      refreshing_ = false;
      replyLoading_ = false;
      if (view_ == View::Replies) {
        replyFailed_ = true;
        replyErrorText_ = tr(STR_JJWXC_NO_WIFI);
      } else {
        moreFailed_ = true;
        state_ = State::Ready;
      }
      requestUpdate();
    } else {
      fail(tr(STR_JJWXC_NO_WIFI));
    }
  }
}

void JjwxcCommentActivity::onWifiResult(const ActivityResult& result) {
  if (!result.isCancelled && transport_.networkReady()) {
    if (wifiPurpose_ == WifiPurpose::FirstFetch) state_ = State::Loading;
    requestUpdate();
    if (wifiPurpose_ == WifiPurpose::Action) {
      startWorker();
    } else if (view_ == View::Replies) {
      startRepliesFetch();
    } else {
      startFetch(0, /*append=*/false);
    }
    return;
  }
  if (wifiPurpose_ == WifiPurpose::Action) {
    // The user backed out of the Wi-Fi page; drop the pending write rather than
    // firing it later against a network that may have changed.
    pendingAction_ = ActionKind::None;
    refreshing_ = false;
    replyLoading_ = false;
    showFeedback(tr(STR_JJWXC_NO_WIFI));
    return;
  }
  if (wifiPurpose_ == WifiPurpose::Refresh && !comments_.empty()) {
    // Keep the cached list; the footer says the refresh failed.
    refreshing_ = false;
    replyLoading_ = false;
    if (view_ == View::Replies) {
      replyFailed_ = true;
      replyErrorText_ = tr(STR_JJWXC_NO_WIFI);
    } else {
      moreFailed_ = true;
      state_ = State::Ready;
    }
    requestUpdate();
    return;
  }
  // Backing out of the Wi-Fi page does not leave the book: the reader is still
  // underneath, and Back here returns to it.
  fail(tr(STR_JJWXC_NO_WIFI));
}

void JjwxcCommentActivity::refresh() {
  if (refreshing_ || replyLoading_ || loadingMore_ || fetchTask_ != nullptr || fetchDone_.load()) return;
  feedback_.clear();
  if (view_ == View::Replies) {
    replyLoading_ = true;
  } else {
    refreshing_ = true;
  }
  moreFailed_ = false;
  requestUpdate();

  // The cached view never needed the session; a refresh does. Keep the old list
  // if any of this fails so the panel does not turn into a blank error page.
  if (!ensureSession()) {
    refreshing_ = false;
    replyLoading_ = false;
    if (view_ == View::Replies) {
      replyFailed_ = true;
      replyErrorText_ = tr(STR_JJWXC_ERR_MEMORY);
      requestUpdate();
    } else if (comments_.empty()) {
      fail(tr(STR_JJWXC_ERR_MEMORY));
    } else {
      moreFailed_ = true;
      requestUpdate();
    }
    return;
  }
  const uint64_t now = cookieClock();
  if (!JjwxcStore::loadCookies(*jar_, now) || !JjwxcStore::hasSession(*jar_, now)) {
    refreshing_ = false;
    replyLoading_ = false;
    if (view_ == View::Replies) {
      replyFailed_ = true;
      replyErrorText_ = tr(STR_JJWXC_NEED_LOGIN);
      requestUpdate();
    } else if (comments_.empty()) {
      fail(tr(STR_JJWXC_NEED_LOGIN));
    } else {
      moreFailed_ = true;
      requestUpdate();
    }
    return;
  }

  if (transport_.networkReady()) {
    if (view_ == View::Replies) {
      startRepliesFetch();
    } else {
      startFetch(0, /*append=*/false);
    }
  } else {
    beginWifi(WifiPurpose::Refresh);
  }
}

void JjwxcCommentActivity::fail(const char* text) {
  state_ = State::Failed;
  errorText_ = text;
  loadingMore_ = false;
  refreshing_ = false;
  requestUpdate();
}

void JjwxcCommentActivity::onExit() {
  reapFetch();
  transport_.resetSession();
  if (wifiBroughtUp_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    esp_wifi_deinit();
  }
  wifiBroughtUp_ = false;
  releaseSession();
  response_.reset();
  urlBuf_.reset();
  bodyBuf_.reset();
  Activity::onExit();
}

bool JjwxcCommentActivity::collectBody(void* ctx, const uint8_t* data, const size_t len) {
  auto* buffer = static_cast<BodyBuffer*>(ctx);
  if (buffer == nullptr || (data == nullptr && len != 0)) return false;
  if (buffer->len + len >= buffer->cap) {
    buffer->overflow = true;
    return false;  // abort the transfer; the caller reports TooLarge
  }
  memcpy(buffer->data + buffer->len, data, len);
  buffer->len += len;
  buffer->data[buffer->len] = '\0';
  return true;
}

bool JjwxcCommentActivity::onCommentItem(void* ctx, const jjwxc::CommentItem& item, size_t) {
  auto* self = static_cast<JjwxcCommentActivity*>(ctx);
  if (self->incoming_.size() >= kMaxComments) return false;  // stop parsing, keep what fits
  self->incoming_.push_back(item);
  return true;
}

void JjwxcCommentActivity::fetchPage(const uint32_t offset, const bool append) {
  incoming_.clear();
  fetchOk_ = true;
  fetchError_ = nullptr;
  fetchedReceived_ = 0;
  fetchedTotal_ = 0;
  auto failPage = [&](const char* text) {
    fetchOk_ = false;
    fetchError_ = text;
    LOG_ERR(kTag, "comment list request failed (offset=%lu, append=%d): %s", (unsigned long)offset, append ? 1 : 0,
            text);
  };

  if (wap_ == nullptr || jar_ == nullptr || !response_) {
    failPage(tr(STR_JJWXC_ERR_MEMORY));
    return;
  }

  char sid[256];
  char token[400];
  token[0] = '\0';
  const uint64_t now = cookieClock();
  if (jar_->get("sid", sid, sizeof(sid), now) != 0) {
    char encoded[400];
    const bool encodedOk =
        jjwxc::Base64::encode(reinterpret_cast<const uint8_t*>(sid), strlen(sid), encoded, sizeof(encoded)) > 0 &&
        jjwxc::wapUrlEncode(encoded, strlen(encoded), token, sizeof(token)) > 0;
    if (!encodedOk) token[0] = '\0';
  }

  if (!urlBuf_) {
    failPage(tr(STR_JJWXC_ERR_MEMORY));
    return;
  }
  auto* url = reinterpret_cast<char*>(urlBuf_.get());
  const int n = snprintf(url, jjwxc::WapIo::kUrlCap,
                         "https://www.jjwxc.net/app.jjwxc/Pc/comment/getCommentList"
                         "?versionCode=489&novelId=%s&chapterId=%lu&paragraph_id=%lu&offset=%lu&limit=%u%s%s",
                         novelId_.c_str(), static_cast<unsigned long>(chapterId_),
                         static_cast<unsigned long>(paragraphId_), static_cast<unsigned long>(offset),
                         static_cast<unsigned>(kPageSize), token[0] ? "&token=" : "", token);
  if (n <= 0 || static_cast<size_t>(n) >= jjwxc::WapIo::kUrlCap) {
    failPage(tr(STR_JJWXC_ERR_TOO_LARGE));
    return;
  }

  auto* buffer = reinterpret_cast<char*>(response_.get());
  buffer[0] = '\0';
  BodyBuffer collector{buffer, kResponseBytes, 0, false};
  jjwxc::WapOptions options;
  options.referer = kReferer;
  jjwxc::WapResponse response;
  const int http = wap_->get(url, options, &collectBody, &collector, response);

  if (http < 0) {
    LOG_ERR(kTag, "comment list request failed: transport=%d", http);
    failPage(tr(STR_JJWXC_ERR_NETWORK));
    return;
  }
  if (http == 301 || http == 302 || http == 303 || http == 307 || http == 308 ||
      strstr(response.finalUrl, "/my/login") != nullptr) {
    failPage(tr(STR_JJWXC_NEED_LOGIN));
    return;
  }
  if (http != 200) {
    LOG_ERR(kTag, "comment list HTTP %d", http);
    failPage(tr(STR_JJWXC_ERR_SERVER));
    return;
  }
  if (collector.overflow) {
    failPage(tr(STR_JJWXC_ERR_TOO_LARGE));
    return;
  }
  if (collector.len == 0) {
    failPage(tr(STR_JJWXC_ERR_EMPTY));
    return;
  }
  buffer[collector.len] = '\0';

  size_t textLen = collector.len;
  const auto encoding = txt_encoding::detect(reinterpret_cast<const uint8_t*>(buffer), collector.len, true);
  if (encoding == txt_encoding::Encoding::Gbk) {
    const auto converted = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(buffer), collector.len,
                                                             kResponseBytes, true);
    if (converted.rawLength == 0 || converted.utf8Length == 0) {
      failPage(tr(STR_JJWXC_ERR_PROTOCOL));
      return;
    }
    textLen = converted.utf8Length;
    buffer[textLen] = '\0';
  }

  // A non-200 business code means the parser below would return 0 and we would
  // report "no comments" on a session problem. Name the two auth-ish cases.
  const int code = jsonCode(buffer);
  if (code == 1004 || code == 1001) {
    failPage(tr(STR_JJWXC_NEED_LOGIN));
    return;
  }
  if (code >= 0 && code != 200 && code != 0) {
    LOG_ERR(kTag, "comment list code=%d", code);
    failPage(tr(STR_JJWXC_ERR_SERVER));
    return;
  }

  fetchedReceived_ = jjwxc::parseCommentList(buffer, textLen, &onCommentItem, this);
  fetchedTotal_ = jsonCommentTotal(buffer);
}

void JjwxcCommentActivity::fetchReplies(const uint32_t offset, const bool append) {
  incomingReplies_.clear();
  fetchOk_ = true;
  fetchError_ = nullptr;
  fetchedReplies_ = 0;
  auto failPage = [&](const char* text) {
    fetchOk_ = false;
    fetchError_ = text;
    LOG_ERR(kTag, "reply list request failed (offset=%lu, append=%d): %s", (unsigned long)offset, append ? 1 : 0,
            text);
  };

  if (wap_ == nullptr || jar_ == nullptr || !response_ || parentCommentIndex_ >= comments_.size()) {
    failPage(tr(STR_JJWXC_ERR_MEMORY));
    return;
  }
  const jjwxc::CommentItem& parent = comments_[parentCommentIndex_];
  if (parent.commentId[0] == '\0') {
    failPage(tr(STR_JJWXC_COMMENT_CACHED_ONLY));
    return;
  }
  if (!urlBuf_) {
    failPage(tr(STR_JJWXC_ERR_MEMORY));
    return;
  }
  auto* url = reinterpret_cast<char*>(urlBuf_.get());
  if (jjwxc::buildCommentReplyListUrl(url, jjwxc::WapIo::kUrlCap, novelId_.c_str(), chapterId_, parent.commentId,
                                      offset, static_cast<uint32_t>(kReplyPageSize)) == 0) {
    failPage(tr(STR_JJWXC_ERR_TOO_LARGE));
    return;
  }

  auto* buffer = reinterpret_cast<char*>(response_.get());
  buffer[0] = '\0';
  BodyBuffer collector{buffer, kResponseBytes, 0, false};
  jjwxc::WapOptions options;
  options.referer = kReferer;
  jjwxc::WapResponse response;
  const int http = wap_->get(url, options, &collectBody, &collector, response);
  if (http < 0) {
    failPage(tr(STR_JJWXC_ERR_NETWORK));
    return;
  }
  if (http == 301 || http == 302 || http == 303 || http == 307 || http == 308 ||
      strstr(response.finalUrl, "/my/login") != nullptr) {
    failPage(tr(STR_JJWXC_NEED_LOGIN));
    return;
  }
  if (http != 200) {
    failPage(tr(STR_JJWXC_ERR_SERVER));
    return;
  }
  if (collector.overflow) {
    failPage(tr(STR_JJWXC_ERR_TOO_LARGE));
    return;
  }
  if (collector.len == 0) {
    failPage(tr(STR_JJWXC_ERR_EMPTY));
    return;
  }
  buffer[collector.len] = '\0';

  size_t textLen = collector.len;
  const auto encoding = txt_encoding::detect(reinterpret_cast<const uint8_t*>(buffer), collector.len, true);
  if (encoding == txt_encoding::Encoding::Gbk) {
    const auto converted = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t*>(buffer), collector.len,
                                                             kResponseBytes, true);
    if (converted.rawLength == 0 || converted.utf8Length == 0) {
      failPage(tr(STR_JJWXC_ERR_PROTOCOL));
      return;
    }
    textLen = converted.utf8Length;
    buffer[textLen] = '\0';
  }

  const int code = jsonCode(buffer);
  if (code == 1004 || code == 1001) {
    failPage(tr(STR_JJWXC_NEED_LOGIN));
    return;
  }
  if (code >= 0 && code != 200 && code != 0) {
    failPage(tr(STR_JJWXC_ERR_SERVER));
    return;
  }

  struct ReplyCtx {
    std::vector<jjwxc::CommentReplyItem, jjwxc::PsramAllocator<jjwxc::CommentReplyItem>>* out;
    size_t cap;
  } ctx{&incomingReplies_, kMaxReplies};
  fetchedReplies_ = jjwxc::parseReplyList(
      buffer, textLen,
      [](void* raw, const jjwxc::CommentReplyItem& item, size_t) {
        auto* c = static_cast<ReplyCtx*>(raw);
        if (c->out->size() >= c->cap) return false;
        c->out->push_back(item);
        return true;
      },
      &ctx);
  (void)append;
}

void JjwxcCommentActivity::applyRepliesResult() {
  replyLoading_ = false;
  if (!fetchOk_) {
    replyFailed_ = true;
    replyErrorText_ = fetchError_ != nullptr ? fetchError_ : tr(STR_JJWXC_ERR_UNKNOWN);
    if (view_ == View::Replies) requestUpdate();
    return;
  }
  const size_t received = fetchedReplies_;
  if (pendingAppend_ && !replies_.empty()) {
    for (const auto& item : incomingReplies_) {
      if (replies_.size() >= kMaxReplies) break;
      replies_.push_back(item);
    }
  } else {
    replies_.swap(incomingReplies_);
  }
  incomingReplies_.clear();
  replyFailed_ = false;
  replyErrorText_ = nullptr;
  if (replyTotal_ < replies_.size()) replyTotal_ = static_cast<uint32_t>(replies_.size());
  if (received == 0) {
    if (pendingAppend_ && !replies_.empty()) replyTotal_ = static_cast<uint32_t>(replies_.size());
    else if (replies_.empty()) replyTotal_ = 0;
  } else if (replyTotal_ <= replies_.size() && received >= kReplyPageSize) {
    // The service does not always return a reply total; a full page means there
    // may be one more, and the next request will settle it.
    replyTotal_ = static_cast<uint32_t>(replies_.size() + 1);
  }
  if (parentCommentIndex_ < comments_.size()) {
    const bool changed = comments_[parentCommentIndex_].replyTotal != replyTotal_;
    comments_[parentCommentIndex_].replyTotal = replyTotal_;
    if (changed) persistComments();
  }
  // A just-posted reply is at the end of the thread; show it without making the
  // user scroll. A refresh keeps the reading position.
  if (pendingAction_ == ActionKind::PostReply) scrollRepliesToBottom_ = true;
  LOG_INF(kTag, "reply list: %u shown / %u total", (unsigned)replies_.size(), (unsigned)replyTotal_);
  requestUpdate();
}

bool JjwxcCommentActivity::prepareAction() {
  if (!ensureSession()) {
    showFeedback(tr(STR_JJWXC_ERR_MEMORY));
    return false;
  }
  const uint64_t now = cookieClock();
  if (!JjwxcStore::loadCookies(*jar_, now) || !JjwxcStore::hasSession(*jar_, now)) {
    showFeedback(tr(STR_JJWXC_NEED_LOGIN));
    return false;
  }
  return true;
}

void JjwxcCommentActivity::runActionWhenOnline() {
  if (transport_.networkReady()) {
    startWorker();
  } else {
    beginWifi(WifiPurpose::Action);
  }
}

void JjwxcCommentActivity::showFeedback(const char* text) {
  feedback_ = text != nullptr ? text : "";
  requestUpdate();
}

void JjwxcCommentActivity::showActionError() {
  refreshing_ = false;
  replyLoading_ = false;
  if (actionCaptcha_) {
    feedback_ = tr(STR_JJWXC_COMMENT_CAPTCHA);
  } else if (actionCode_ == 1004 || actionCode_ == 1001) {
    feedback_ = tr(STR_JJWXC_NEED_LOGIN);
  } else if (actionCode_ == jjwxc::kTransportTooLarge) {
    feedback_ = tr(STR_JJWXC_ERR_TOO_LARGE);
  } else if (actionCode_ == jjwxc::kTransportNetwork) {
    feedback_ = tr(STR_JJWXC_ERR_NETWORK);
  } else if (actionMessage_[0] != '\0') {
    // The service's own sentence is the only actionable one for moderation and
    // rate-limit refusals; surfacing it is better than a generic "server error".
    feedback_ = actionMessage_;
  } else {
    feedback_ = tr(STR_JJWXC_ERR_SERVER);
  }
  requestUpdate();
}

void JjwxcCommentActivity::startLikeAction(const uint32_t index, const bool reply) {
  if (refreshing_ || replyLoading_ || loadingMore_ || fetchTask_ != nullptr || fetchDone_.load()) return;
  if (reply) {
    if (index >= replies_.size() || parentCommentIndex_ >= comments_.size()) return;
    if (comments_[parentCommentIndex_].commentId[0] == '\0' || replies_[index].replyId[0] == '\0') {
      showFeedback(tr(STR_JJWXC_COMMENT_CACHED_ONLY));
      return;
    }
  } else {
    if (index >= comments_.size()) return;
    if (comments_[index].commentId[0] == '\0') {
      showFeedback(tr(STR_JJWXC_COMMENT_CACHED_ONLY));
      return;
    }
  }

  pendingAction_ = reply ? ActionKind::LikeReply : ActionKind::LikeComment;
  actionCommentIndex_ = index;
  actionIsReply_ = reply;
  actionLike_ = reply ? (replies_[index].readerAgree != 1) : (comments_[index].readerAgree != 1);
  actionOk_ = true;
  actionCode_ = -1;
  actionCaptcha_ = false;
  actionMessage_[0] = '\0';
  feedback_.clear();
  refreshing_ = true;
  if (!prepareAction()) {
    pendingAction_ = ActionKind::None;
    refreshing_ = false;
    return;
  }
  requestUpdate();
  runActionWhenOnline();
}

void JjwxcCommentActivity::performLike(const bool reply) {
  actionOk_ = false;
  actionCode_ = -1;
  actionMessage_[0] = '\0';
  actionCaptcha_ = false;
  if (!urlBuf_ || !response_) return;
  auto* url = reinterpret_cast<char*>(urlBuf_.get());
  bool built = false;
  if (reply) {
    if (actionCommentIndex_ >= replies_.size() || parentCommentIndex_ >= comments_.size()) return;
    built = jjwxc::buildCommentAgreeUrl(url, jjwxc::WapIo::kUrlCap, actionLike_, novelId_.c_str(), chapterId_,
                                        comments_[parentCommentIndex_].commentId,
                                        replies_[actionCommentIndex_].replyId) != 0;
  } else {
    if (actionCommentIndex_ >= comments_.size()) return;
    built = jjwxc::buildCommentAgreeUrl(url, jjwxc::WapIo::kUrlCap, actionLike_, novelId_.c_str(), chapterId_,
                                        comments_[actionCommentIndex_].commentId, "0") != 0;
  }
  if (!built) {
    actionCode_ = jjwxc::kTransportTooLarge;
    return;
  }

  auto* buffer = reinterpret_cast<char*>(response_.get());
  BodyBuffer collector{buffer, kResponseBytes, 0, false};
  jjwxc::WapOptions options;
  options.referer = kReferer;
  jjwxc::WapResponse response;
  const int http = wap_->get(url, options, &collectBody, &collector, response);
  if (http < 0) {
    actionCode_ = http;
    return;
  }
  if (http == 301 || http == 302 || http == 303 || http == 307 || http == 308 ||
      strstr(response.finalUrl, "/my/login") != nullptr) {
    actionCode_ = 1004;
    return;
  }
  if (http != 200 || collector.len == 0) {
    actionCode_ = http != 200 ? http : -1;
    return;
  }
  buffer[collector.len] = '\0';
  jjwxc::CommentActionResult result;
  if (!jjwxc::parseCommentActionResult(buffer, collector.len, result)) return;
  actionCode_ = result.code;
  snprintf(actionMessage_, sizeof(actionMessage_), "%s", result.message);
  actionCaptcha_ = result.code == 10;
  actionOk_ = result.ok();
  LOG_INF(kTag, "comment like %s: %s code=%d", reply ? "reply" : "comment", actionOk_ ? "ok" : "failed",
          result.code);
}

void JjwxcCommentActivity::performPostComment() {
  actionOk_ = false;
  actionCode_ = -1;
  actionMessage_[0] = '\0';
  actionCaptcha_ = false;
  if (!urlBuf_ || !response_ || draftText_.empty()) return;
  char rawNickname[96] = {};
  const size_t rawLen = jar_->get("nickname", rawNickname, sizeof(rawNickname) - 1, cookieClock());
  char author[96] = {};
  percentDecode(rawNickname, rawLen, author, sizeof(author));
  auto* url = reinterpret_cast<char*>(urlBuf_.get());
  if (jjwxc::buildCommentWriteUrl(url, jjwxc::WapIo::kUrlCap, novelId_.c_str(), chapterId_, paragraphId_,
                                  draftText_.c_str(), author, "get_deviceid_timeout") == 0) {
    actionCode_ = jjwxc::kTransportTooLarge;
    return;
  }

  auto* buffer = reinterpret_cast<char*>(response_.get());
  BodyBuffer collector{buffer, kResponseBytes, 0, false};
  jjwxc::WapOptions options;
  options.referer = kReferer;
  jjwxc::WapResponse response;
  const int http = wap_->get(url, options, &collectBody, &collector, response);
  if (http < 0) {
    actionCode_ = http;
    return;
  }
  if (http == 301 || http == 302 || http == 303 || http == 307 || http == 308 ||
      strstr(response.finalUrl, "/my/login") != nullptr) {
    actionCode_ = 1004;
    return;
  }
  if (http != 200 || collector.len == 0) {
    actionCode_ = http != 200 ? http : -1;
    return;
  }
  buffer[collector.len] = '\0';
  jjwxc::CommentActionResult result;
  if (!jjwxc::parseCommentActionResult(buffer, collector.len, result)) return;
  actionCode_ = result.code;
  snprintf(actionMessage_, sizeof(actionMessage_), "%s", result.message);
  actionCaptcha_ = result.code == 10;
  actionOk_ = result.ok();
  LOG_INF(kTag, "paragraph comment post: %s code=%d", actionOk_ ? "ok" : "failed", result.code);
  if (actionOk_) {
    // Re-read the page rather than synthesizing a row: the server assigns the
    // id/date/floor and may apply moderation before it is visible to us.
    pendingAppend_ = false;
    refreshing_ = true;
    fetchPage(0, /*append=*/false);
  }
}

void JjwxcCommentActivity::performPostReply() {
  actionOk_ = false;
  actionCode_ = -1;
  actionMessage_[0] = '\0';
  actionCaptcha_ = false;
  if (!bodyBuf_ || !response_ || draftText_.empty() || parentCommentIndex_ >= comments_.size()) return;
  const jjwxc::CommentItem& parent = comments_[parentCommentIndex_];
  if (parent.commentId[0] == '\0') {
    actionCode_ = 0;  // not a server failure; the caller's guard should have stopped this
    return;
  }
  char rawNickname[96] = {};
  const size_t rawLen = jar_->get("nickname", rawNickname, sizeof(rawNickname) - 1, cookieClock());
  char author[96] = {};
  percentDecode(rawNickname, rawLen, author, sizeof(author));
  auto* body = reinterpret_cast<char*>(bodyBuf_.get());
  const size_t bodyLen =
      jjwxc::buildCommentReplyBody(body, kReplyBodyBytes, novelId_.c_str(), chapterId_, parent.commentId,
                                   draftText_.c_str(), author);
  if (bodyLen == 0) {
    actionCode_ = jjwxc::kTransportTooLarge;
    return;
  }

  auto* buffer = reinterpret_cast<char*>(response_.get());
  BodyBuffer collector{buffer, kResponseBytes, 0, false};
  jjwxc::WapOptions options;
  options.referer = kReferer;
  options.contentType = jjwxc::kWapFormContentType;
  jjwxc::WapResponse response;
  const int http = wap_->post("https://www.jjwxc.net/app.jjwxc/Pc/comment/writeReply", body, bodyLen, options,
                              &collectBody, &collector, response);
  if (http < 0) {
    actionCode_ = http;
    return;
  }
  if (http == 301 || http == 302 || http == 303 || http == 307 || http == 308 ||
      strstr(response.finalUrl, "/my/login") != nullptr) {
    actionCode_ = 1004;
    return;
  }
  if (http != 200 || collector.len == 0) {
    actionCode_ = http != 200 ? http : -1;
    return;
  }
  buffer[collector.len] = '\0';
  jjwxc::CommentActionResult result;
  if (!jjwxc::parseCommentActionResult(buffer, collector.len, result)) return;
  actionCode_ = result.code;
  snprintf(actionMessage_, sizeof(actionMessage_), "%s", result.message);
  actionCaptcha_ = result.code == 10;
  actionOk_ = result.ok();
  LOG_INF(kTag, "paragraph comment reply: %s code=%d", actionOk_ ? "ok" : "failed", result.code);
  if (actionOk_) {
    pendingAppend_ = false;
    replyLoading_ = true;
    fetchReplies(0, /*append=*/false);
  }
}

void JjwxcCommentActivity::applyActionResult() {
  const ActionKind action = pendingAction_;
  if (!actionOk_) {
    pendingAction_ = ActionKind::None;
    showActionError();
    return;
  }
  switch (action) {
    case ActionKind::LikeComment:
      if (actionCommentIndex_ < comments_.size()) {
        auto& c = comments_[actionCommentIndex_];
        if (actionLike_) {
          if (c.readerAgree == 2 && c.disagreeNum > 0) --c.disagreeNum;
          c.readerAgree = 1;
          ++c.agreeNum;
        } else {
          c.readerAgree = 0;
          if (c.agreeNum > 0) --c.agreeNum;
        }
        persistComments();
      }
      break;
    case ActionKind::LikeReply:
      if (actionCommentIndex_ < replies_.size()) {
        auto& r = replies_[actionCommentIndex_];
        if (actionLike_) {
          if (r.readerAgree == 2 && r.disagreeNum > 0) --r.disagreeNum;
          r.readerAgree = 1;
          ++r.agreeNum;
        } else {
          r.readerAgree = 0;
          if (r.agreeNum > 0) --r.agreeNum;
        }
      }
      break;
    case ActionKind::PostComment:
      draftText_.clear();
      feedback_ = tr(STR_JJWXC_COMMENT_POSTED);
      if (fetchOk_) {
        applyFetchResult();
      } else {
        refreshing_ = false;
        moreFailed_ = true;
      }
      break;
    case ActionKind::PostReply:
      draftText_.clear();
      feedback_ = tr(STR_JJWXC_COMMENT_POSTED);
      if (parentCommentIndex_ < comments_.size()) {
        ++replyTotal_;
        comments_[parentCommentIndex_].replyTotal = replyTotal_;
        persistComments();
      }
      if (fetchOk_) {
        applyRepliesResult();
      } else {
        replyLoading_ = false;
        replyFailed_ = true;
      }
      break;
    default:
      break;
  }
  if (action == ActionKind::LikeComment || action == ActionKind::LikeReply) refreshing_ = false;
  pendingAction_ = ActionKind::None;
  requestUpdate();
}

void JjwxcCommentActivity::enterReplies(const uint32_t commentIndex) {
  if (commentIndex >= comments_.size()) return;
  if (comments_[commentIndex].commentId[0] == '\0') {
    showFeedback(tr(STR_JJWXC_COMMENT_CACHED_ONLY));
    return;
  }
  commentsScrollY_ = scrollY_;
  view_ = View::Replies;
  parentCommentIndex_ = commentIndex;
  replyTotal_ = comments_[commentIndex].replyTotal;
  scrollY_ = 0;
  repliesScrollY_ = 0;
  scrollRepliesToBottom_ = false;
  replyFailed_ = false;
  replyErrorText_ = nullptr;
  feedback_.clear();
  replyLoading_ = true;
  requestUpdate();

  if (!ensureSession()) {
    replyLoading_ = false;
    replyFailed_ = true;
    replyErrorText_ = tr(STR_JJWXC_ERR_MEMORY);
    requestUpdate();
    return;
  }
  const uint64_t now = cookieClock();
  if (!JjwxcStore::loadCookies(*jar_, now) || !JjwxcStore::hasSession(*jar_, now)) {
    replyLoading_ = false;
    replyFailed_ = true;
    replyErrorText_ = tr(STR_JJWXC_NEED_LOGIN);
    requestUpdate();
    return;
  }
  if (transport_.networkReady()) {
    startRepliesFetch();
  } else {
    beginWifi(WifiPurpose::Refresh);
  }
}

void JjwxcCommentActivity::leaveReplies() {
  repliesScrollY_ = scrollY_;
  view_ = View::Comments;
  scrollY_ = commentsScrollY_;
  replyLoading_ = false;
  replyFailed_ = false;
  feedback_.clear();
  requestUpdate();
}

void JjwxcCommentActivity::addHit(const HitKind kind, const uint16_t index, const int x, const int y, const int w,
                                  const int h) {
  if (hitCount_ >= kMaxHitTargets || w <= 0 || h <= 0) return;
  hits_[hitCount_++] = HitTarget{kind, index, static_cast<int16_t>(x), static_cast<int16_t>(y),
                                 static_cast<int16_t>(w), static_cast<int16_t>(h)};
}

bool JjwxcCommentActivity::handleTap(const int x, const int y) {
  for (size_t i = 0; i < hitCount_; ++i) {
    const HitTarget& hit = hits_[i];
    if (x < hit.x || x >= hit.x + hit.w || y < hit.y || y >= hit.y + hit.h) continue;
    if (hit.kind == HitKind::Like) {
      startLikeAction(hit.index, view_ == View::Replies);
    } else if (hit.kind == HitKind::Reply) {
      if (view_ == View::Comments) {
        enterReplies(hit.index);
      } else {
        openComposer(/*reply=*/true);
      }
    } else {
      showFeedback(tr(STR_JJWXC_COMMENT_CACHED_ONLY));
    }
    return true;
  }
  return false;
}

void JjwxcCommentActivity::openComposer(const bool reply) {
  if (refreshing_ || replyLoading_ || fetchTask_ != nullptr || fetchDone_.load()) return;
  if (reply && parentCommentIndex_ >= comments_.size()) return;
  const bool reuseDraft = draftIsReply_ == reply && !draftText_.empty();
  const auto handler = [this, reply](const ActivityResult& result) {
    inputBlockUntilMs_ = millis() + 400;
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const std::string text = std::get<KeyboardResult>(result.data).text;
    if (text.empty()) {
      requestUpdate();
      return;
    }
    draftText_ = text;
    draftIsReply_ = reply;
    if (utf8CodePoints(text.c_str()) > kMaxComposeChars) {
      showFeedback(tr(STR_JJWXC_COMMENT_TOO_LONG));
      return;
    }
    pendingAction_ = reply ? ActionKind::PostReply : ActionKind::PostComment;
    actionOk_ = true;
    actionCode_ = -1;
    actionCaptcha_ = false;
    actionMessage_[0] = '\0';
    feedback_.clear();
    refreshing_ = true;
    if (!prepareAction()) {
      pendingAction_ = ActionKind::None;
      refreshing_ = false;
      return;
    }
    requestUpdate();
    runActionWhenOnline();
  };
  const bool started = startActivityForResultWith<KeyboardEntryActivity>(
      handler, reply ? tr(STR_JJWXC_COMMENT_REPLY) : tr(STR_JJWXC_COMMENT_WRITE),
      reuseDraft ? draftText_ : std::string(), kMaxComposeBytes, InputType::Text);
  if (!started) showFeedback(tr(STR_JJWXC_ERR_MEMORY));
}

void JjwxcCommentActivity::applyFetchResult() {
  const bool append = pendingAppend_;
  if (!fetchOk_) {
    if (append) {
      moreFailed_ = true;
      loadingMore_ = false;
      return;
    }
    refreshing_ = false;
    if (!comments_.empty()) {
      // A refresh failed; the cached list is still valid and stays on screen.
      state_ = State::Ready;
      showingCache_ = true;
      moreFailed_ = true;
      return;
    }
    fail(fetchError_ != nullptr ? fetchError_ : tr(STR_JJWXC_ERR_UNKNOWN));
    return;
  }

  const size_t received = fetchedReceived_;
  const uint32_t total = fetchedTotal_;
  errorText_ = nullptr;

  if (append) {
    if (received == 0 || incoming_.empty()) {
      // Nothing new: stop offering "more" instead of looping on the same offset.
      commentTotal_ = static_cast<uint32_t>(comments_.size());
    } else {
      for (const auto& item : incoming_) {
        if (comments_.size() >= kMaxComments) break;
        comments_.push_back(item);
      }
      if (comments_.size() >= kMaxComments) {
        // The local cap is full. Pretending more exists would page forever
        // without the list ever growing; the reader can reopen to start over.
        commentTotal_ = static_cast<uint32_t>(comments_.size());
      } else if (total > 0) {
        commentTotal_ = total;
      } else {
        commentTotal_ = static_cast<uint32_t>(comments_.size() + (received >= kPageSize ? 1 : 0));
      }
    }
    loadingMore_ = false;
    moreFailed_ = false;
    state_ = State::Ready;
    showingCache_ = false;
    incoming_.clear();
    persistComments();
    LOG_INF(kTag, "comment list +%u (para=%lu %u shown / %u total)", (unsigned)received,
            (unsigned long)paragraphId_, (unsigned)comments_.size(), (unsigned)commentTotal_);
    return;
  }

  if (incoming_.empty()) {
    if (total > 0) {
      // The count says there is something, the parser found nothing: the shape
      // changed under us. Keep a good cached list; otherwise say so instead of
      // showing a convincing "no comments".
      LOG_ERR(kTag, "comment list: total=%u but nothing parsed", (unsigned)total);
      if (!comments_.empty()) {
        refreshing_ = false;
        moreFailed_ = true;
        state_ = State::Ready;
        showingCache_ = true;
      } else {
        fail(tr(STR_JJWXC_ERR_PROTOCOL));
      }
      return;
    }
    comments_.clear();
    commentTotal_ = 0;
    state_ = State::Empty;
    showingCache_ = false;
    refreshing_ = false;
    // A server-confirmed empty list invalidates an older sidecar.
    JjwxcStore::removeCommentBodies(novelId_.c_str(), chapterId_, paragraphId_);
    return;
  }

  comments_.swap(incoming_);
  incoming_.clear();
  commentTotal_ = total > 0 ? total : static_cast<uint32_t>(comments_.size() + (received >= kPageSize ? 1 : 0));
  if (comments_.size() >= kMaxComments) commentTotal_ = static_cast<uint32_t>(comments_.size());
  state_ = State::Ready;
  scrollY_ = 0;
  commentsScrollY_ = 0;
  refreshing_ = false;
  loadingMore_ = false;
  moreFailed_ = false;
  showingCache_ = false;
  persistComments();
  LOG_INF(kTag, "comment list: para=%lu %u shown / %u total", (unsigned long)paragraphId_,
          (unsigned)comments_.size(), (unsigned)commentTotal_);
}

void JjwxcCommentActivity::loadMore() {
  if (loadingMore_ || refreshing_ || replyLoading_ || fetchTask_ != nullptr || fetchDone_.load()) return;
  if (view_ == View::Replies) {
    if (!replyHasMore()) return;
    replyLoading_ = true;
    moreFailed_ = false;
    requestUpdate();
    startRepliesFetch(/*append=*/true);
    return;
  }
  if (!hasMore() || state_ != State::Ready) return;
  loadingMore_ = true;
  moreFailed_ = false;
  requestUpdate();
  startFetch(static_cast<uint32_t>(comments_.size()), /*append=*/true);
}

bool JjwxcCommentActivity::hasMore() const { return commentTotal_ > comments_.size(); }

bool JjwxcCommentActivity::replyHasMore() const {
  return replyTotal_ > replies_.size() && replies_.size() < kMaxReplies;
}

void JjwxcCommentActivity::startWorker() {
  if (fetchTask_ != nullptr) return;
  fetchDone_.store(false);
#ifdef ARDUINO
  BaseType_t ok = pdFAIL;
#ifdef BOARD_HAS_PSRAM
  ok = xTaskCreatePinnedToCoreWithCaps(fetchTrampoline, "jjwxcComment", 32 * 1024, this, 1, &fetchTask_, 0,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  fetchTaskInPsram_ = ok == pdPASS;
  if (ok != pdPASS) fetchTask_ = nullptr;
#endif
  if (fetchTask_ == nullptr) {
    ok = xTaskCreatePinnedToCore(fetchTrampoline, "jjwxcComment", 12 * 1024, this, 1, &fetchTask_, 0);
    fetchTaskInPsram_ = false;
    if (ok != pdPASS) {
      fetchTask_ = nullptr;
      pendingAction_ = ActionKind::None;
      refreshing_ = false;
      replyLoading_ = false;
      fail(tr(STR_JJWXC_ERR_MEMORY));
      return;
    }
  }
#else
  // Host/simulator: no FreeRTOS. The activity is unreachable there (PSRAM-gated),
  // but keep it compilable and deterministic.
  workerRun();
  fetchDone_.store(true);
#endif
}

void JjwxcCommentActivity::startFetch(const uint32_t offset, const bool append) {
  if (fetchTask_ != nullptr) {
    refreshing_ = false;
    return;
  }
  pendingAction_ = ActionKind::FetchPage;
  pendingOffset_ = offset;
  pendingAppend_ = append;
  startWorker();
}

void JjwxcCommentActivity::startRepliesFetch(const bool append) {
  if (fetchTask_ != nullptr) {
    replyLoading_ = false;
    return;
  }
  pendingAction_ = ActionKind::FetchReplies;
  pendingOffset_ = append ? static_cast<uint32_t>(replies_.size()) : 0;
  pendingAppend_ = append;
  startWorker();
}

void JjwxcCommentActivity::workerRun() {
  switch (pendingAction_) {
    case ActionKind::FetchPage:
      fetchPage(pendingOffset_, pendingAppend_);
      break;
    case ActionKind::FetchReplies:
      fetchReplies(pendingOffset_, pendingAppend_);
      break;
    case ActionKind::LikeComment:
      performLike(/*reply=*/false);
      break;
    case ActionKind::LikeReply:
      performLike(/*reply=*/true);
      break;
    case ActionKind::PostComment:
      performPostComment();
      break;
    case ActionKind::PostReply:
      performPostReply();
      break;
    case ActionKind::None:
      break;
  }
}

void JjwxcCommentActivity::fetchTrampoline(void* param) {
  auto* self = static_cast<JjwxcCommentActivity*>(param);
  self->workerRun();
  self->fetchDone_.store(true);
  vTaskSuspend(nullptr);
}

void JjwxcCommentActivity::reapFetch() {
  if (fetchTask_ == nullptr) return;
#ifdef ARDUINO
  if (fetchTaskInPsram_) {
    vTaskDeleteWithCaps(fetchTask_);
  } else {
    vTaskDelete(fetchTask_);
  }
#endif
  fetchTask_ = nullptr;
  fetchTaskInPsram_ = false;
  fetchDone_.store(false);
}

int JjwxcCommentActivity::maxScroll() const {
  return contentHeight_ > viewportHeight_ ? contentHeight_ - viewportHeight_ : 0;
}

void JjwxcCommentActivity::loop() {
  if (inputBlockUntilMs_ != 0) {
    if (static_cast<int32_t>(millis() - inputBlockUntilMs_) < 0) return;
    inputBlockUntilMs_ = 0;
  }

  if (fetchDone_.load()) {
    if (fetchTask_ != nullptr) {
      reapFetch();
    } else {
      fetchDone_.store(false);
    }
    if (pendingAction_ == ActionKind::FetchPage) {
      applyFetchResult();
      pendingAction_ = ActionKind::None;
    } else if (pendingAction_ == ActionKind::FetchReplies) {
      applyRepliesResult();
      pendingAction_ = ActionKind::None;
    } else if (pendingAction_ != ActionKind::None) {
      applyActionResult();
    } else {
      // Defensive: a worker never publishes done without an action, but a stale
      // flag must not strand the panel in "loading".
      refreshing_ = false;
      replyLoading_ = false;
    }
    requestUpdate();
  }

  // Toolbar chips are available in every state: refresh, and the one write
  // action that belongs to the current view.
  if (mappedInput.hasTouch()) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      if (refreshW_ > 0 && x >= refreshX_ && x < refreshX_ + refreshW_ && y >= refreshY_ &&
          y < refreshY_ + refreshH_) {
        refresh();
        return;
      }
      if (actionW_ > 0 && x >= actionX_ && x < actionX_ + actionW_ && y >= actionY_ && y < actionY_ + actionH_) {
        if (view_ == View::Replies) {
          openComposer(/*reply=*/true);
        } else {
          openComposer(/*reply=*/false);
        }
        return;
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view_ == View::Replies) {
      leaveReplies();
    } else {
      finish();
    }
    return;
  }

  if (view_ == View::Comments && state_ != State::Ready) {
    if ((state_ == State::Empty || state_ == State::Failed) && !refreshing_ &&
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      refresh();
    }
    return;
  }
  if (view_ == View::Replies && (replyLoading_ || replyFailed_)) {
    if (replyFailed_ && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) refresh();
    return;
  }

  const int step = renderer.getLineHeight(UI_10_FONT_ID) * 2;
  int delta = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) delta = -step;
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) delta = step;
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) delta = viewportHeight_ - step;
  if (swipe == MappedInputManager::SwipeDir::Down) delta = -(viewportHeight_ - step);
  if (delta != 0) {
    const int limit = maxScroll();
    const int updated = std::clamp(static_cast<int>(scrollY_) + delta, 0, limit);
    if (updated != static_cast<int>(scrollY_)) {
      scrollY_ = static_cast<uint32_t>(updated);
      if (view_ == View::Replies) repliesScrollY_ = scrollY_;
      else commentsScrollY_ = scrollY_;
      requestUpdate();
    }
    return;
  }

  if (refreshing_ || loadingMore_ || replyLoading_) return;

  if (mappedInput.hasTouch()) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      if ((view_ == View::Replies ? replyHasMore() : hasMore()) && moreVisible_ && x >= moreX_ &&
          x < moreX_ + moreW_ && y >= moreY_ && y < moreY_ + moreH_) {
        loadMore();
        return;
      }
      if (handleTap(x, y)) return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // At the end of the list the same key becomes Refresh; in the reply view it
    // is the reply composer. The touch chips remain the discoverable path.
    if (view_ == View::Replies) {
      openComposer(/*reply=*/true);
    } else if (hasMore()) {
      loadMore();
    } else {
      refresh();
    }
  }
}

namespace {
constexpr int kCardPadX = 16;
constexpr int kCardPadY = 8;
constexpr int kCardGap = 10;
constexpr int kMaxBodyLines = 10;
constexpr int kChipPadX = 16;
constexpr int kChipHeight = 36;
constexpr int kToolbarGap = 10;
// A few pixels of slack inside the measured column. Wrapping is based on glyph
// advances, while the ink of the last glyph can extend a little past its
// advance; without this the final character of a full line touches the clip.
constexpr int kTextInset = 3;
// Wrapping sums per-code-point advances; the drawn line resolves one fallback
// font for the whole run, so a mixed ASCII/CJK line can be a little wider than
// the sum. This reserve keeps such a line off the clip instead of shaving the
// final glyph.
constexpr int kWrapSafety = 12;
constexpr int kScrollbarWidth = 4;
constexpr int kScrollbarGap = 8;
}  // namespace

namespace {
int cardMeasure(const GfxRenderer& renderer, const char* text, const size_t bytes) {
  char probe[512];
  const size_t copy = bytes < sizeof(probe) ? bytes : sizeof(probe) - 1;
  memcpy(probe, text, copy);
  probe[copy] = '\0';
  return renderer.getTextWidth(UI_12_FONT_ID, probe, EpdFontFamily::REGULAR);
}

int countBodyLines(const GfxRenderer& renderer, const char* body, const int textWidth) {
  const auto measure = [&renderer](const char* text, const size_t bytes) {
    return cardMeasure(renderer, text, bytes);
  };
  const size_t bodyLen = strlen(body);
  size_t pos = 0;
  int lines = 0;
  while (pos < bodyLen && lines < kMaxBodyLines) {
    const size_t bytes = jjwxc::wrapLineLen(body, bodyLen, pos, textWidth, jjwxc::WrapField::Plain, measure);
    if (bytes == 0) break;
    pos += bytes;
    ++lines;
  }
  return lines;
}

int cardChipHeight(const GfxRenderer& renderer) { return renderer.getLineHeight(SMALL_FONT_ID) + 10; }
}  // namespace

int JjwxcCommentActivity::commentCardHeight(const jjwxc::CommentItem& comment, const int textWidth) const {
  const int metaHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lines = countBodyLines(renderer, comment.body, textWidth);
  const int footer = comment.commentId[0] != '\0' ? 6 + cardChipHeight(renderer) : 0;
  return kCardPadY * 2 + metaHeight + (lines > 0 ? 4 + lines * bodyHeight : 0) + footer;
}

int JjwxcCommentActivity::replyCardHeight(const jjwxc::CommentReplyItem& reply, const int textWidth) const {
  const int metaHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lines = countBodyLines(renderer, reply.body, textWidth);
  const int footer = reply.replyId[0] != '\0' ? 6 + cardChipHeight(renderer) : 0;
  return kCardPadY * 2 + metaHeight + (lines > 0 ? 4 + lines * bodyHeight : 0) + footer;
}

int JjwxcCommentActivity::drawCardBody(const char* author, const char* body, const char* ip, const char* date,
                                       const int x, const int y, const int textWidth, const int bodyHeight,
                                       const int metaHeight) {
  const char* shownAuthor = author != nullptr && author[0] != '\0' ? author : "—";
  renderer.drawText(SMALL_FONT_ID, x, y, shownAuthor, true, EpdFontFamily::BOLD);
  const int authorWidth = renderer.getTextWidth(SMALL_FONT_ID, shownAuthor, EpdFontFamily::BOLD);
  char meta[128];
  if (ip != nullptr && ip[0] != '\0' && date != nullptr && date[0] != '\0') {
    snprintf(meta, sizeof(meta), "· %s · %s", date, ip);
  } else if (date != nullptr && date[0] != '\0') {
    snprintf(meta, sizeof(meta), "· %s", date);
  } else if (ip != nullptr && ip[0] != '\0') {
    snprintf(meta, sizeof(meta), "· %s", ip);
  } else {
    meta[0] = '\0';
  }
  if (meta[0] != '\0') {
    renderer.drawText(SMALL_FONT_ID, x + authorWidth + 4, y, meta, true, EpdFontFamily::REGULAR);
  }

  const auto measure = [this](const char* text, const size_t bytes) {
    return cardMeasure(renderer, text, bytes);
  };
  const size_t bodyLen = strlen(body);
  size_t pos = 0;
  int lineY = y + metaHeight + 4;
  for (int lineNo = 0; lineNo < kMaxBodyLines && pos < bodyLen; ++lineNo) {
    const size_t bytes =
        jjwxc::wrapLineLen(body, bodyLen, pos, textWidth, jjwxc::WrapField::Plain, measure);
    if (bytes == 0) break;
    if (body[pos] == '\n' || body[pos] == '\r') {
      pos += bytes;
      lineY += bodyHeight;
      continue;
    }
    char line[512];
    const size_t copy = bytes < sizeof(line) ? bytes : sizeof(line) - 1;
    memcpy(line, body + pos, copy);
    line[copy] = '\0';
    renderer.drawText(UI_12_FONT_ID, x, lineY, line, true, EpdFontFamily::REGULAR);
    lineY += bodyHeight;
    pos += bytes;
  }
  return lineY;
}

void JjwxcCommentActivity::drawNotice(const char* text, const char* subtitle) {
  const int screenHeight = renderer.getScreenHeight();
  int y = screenHeight / 2 - 8;
  if (subtitle != nullptr && subtitle[0] != '\0') y -= 14;
  renderer.drawCenteredText(UI_12_FONT_ID, y, text, true, EpdFontFamily::BOLD);
  if (subtitle != nullptr && subtitle[0] != '\0') {
    renderer.drawCenteredText(SMALL_FONT_ID, y + 26, subtitle, true, EpdFontFamily::REGULAR);
  }
}

void JjwxcCommentActivity::drawRefreshChip(const int right, const int y, const int height) {
  const char* label = refreshing_ ? tr(STR_JJWXC_LOADING) : tr(STR_JJWXC_COMMENTS_REFRESH);
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
  const int width = textWidth + kChipPadX * 2;
  const int x = right - width;
  refreshX_ = x;
  refreshY_ = y;
  refreshW_ = width;
  refreshH_ = height;

  renderer.fillRoundedRect(x, y, width, height, height / 2, Color::Black);
  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x + kChipPadX, y + (height - textHeight) / 2, label, /*black=*/false,
                    EpdFontFamily::BOLD);
}

void JjwxcCommentActivity::drawActionChip(const int right, const int y, const int height, const char* label,
                                          int& xOut, int& wOut) {
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
  const int width = textWidth + kChipPadX * 2;
  const int x = right - width;
  xOut = x;
  wOut = width;
  renderer.drawRoundedRect(x, y, width, height, 1, height / 2, true);
  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x + kChipPadX, y + (height - textHeight) / 2, label, true, EpdFontFamily::BOLD);
}

void JjwxcCommentActivity::drawList(const int top) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int left = safe.x + metrics.contentSidePadding;
  const int right = safe.x + safe.width - metrics.contentSidePadding;
  const int bottom = safe.y + safe.height - metrics.buttonHintsHeight - metrics.verticalSpacing;
  hitCount_ = 0;
  if (right - left <= 0 || bottom <= top) {
    viewportHeight_ = 0;
    contentHeight_ = 0;
    moreVisible_ = false;
    return;
  }
  viewportHeight_ = bottom - top;
  scrollY_ = commentsScrollY_;

  // The scrollbar lives outside the text column so it can never cover a glyph.
  // Its space is reserved even when the list fits, which keeps the column from
  // shifting the first time a long chapter's comments overflow one screen.
  const int scrollbarX = right - kScrollbarWidth;
  const int textLeft = left + kCardPadX;
  const int textRight = right - kCardPadX - kScrollbarGap - kScrollbarWidth;
  const int textColumn = textRight - textLeft;
  const int textWidth = textColumn - kTextInset * 2 - kWrapSafety;
  if (textWidth <= 0) {
    viewportHeight_ = 0;
    contentHeight_ = 0;
    moreVisible_ = false;
    return;
  }

  const int metaHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int chipH = cardChipHeight(renderer);
  const int moreHeight = bodyHeight + 16;

  // Heights first: contentHeight_ (and therefore maxScroll) must be final before
  // the first card is placed, and wrapping twice per visible card would waste the
  // render budget on a long list.
  uint16_t heights[kMaxComments];
  contentHeight_ = 0;
  for (size_t i = 0; i < comments_.size(); ++i) {
    heights[i] = static_cast<uint16_t>(commentCardHeight(comments_[i], textWidth));
    contentHeight_ += heights[i] + kCardGap;
  }
  if (hasMore()) contentHeight_ += moreHeight;
  if (scrollY_ > static_cast<uint32_t>(maxScroll())) scrollY_ = static_cast<uint32_t>(maxScroll());
  commentsScrollY_ = scrollY_;

  const auto drawPill = [&](const int x, const int y, const int h, const char* label, const bool filled) {
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, label, EpdFontFamily::REGULAR);
    const int w = textW + 20;
    if (filled) {
      renderer.fillRoundedRect(x, y, w, h, h / 2, Color::Black);
    } else {
      renderer.drawRoundedRect(x, y, w, h, 1, h / 2, true);
    }
    const int textH = renderer.getLineHeight(SMALL_FONT_ID);
    renderer.drawText(SMALL_FONT_ID, x + 10, y + (h - textH) / 2, label, !filled, EpdFontFamily::REGULAR);
    return w;
  };
  const auto addClippedHit = [&](const HitKind kind, const uint16_t index, const int x, const int y, const int w,
                                 const int h) {
    const int y0 = std::max(y, top);
    const int y1 = std::min(y + h, bottom);
    if (y1 > y0) addHit(kind, index, x, y0, w, y1 - y0);
  };

  {
    // Clip to the *text column*, not the whole content band. Wrapping is based
    // on advances; a glyph's ink may overhang its advance a little, and that
    // overhang used to paint into the white side padding.
    GfxRenderer::ClipScope clip(renderer, textLeft, top, textColumn, viewportHeight_);
    const int textX = textLeft + kTextInset;

    int y = top - static_cast<int>(scrollY_);
    for (size_t i = 0; i < comments_.size(); ++i) {
      const auto& comment = comments_[i];
      const int height = heights[i];
      if (y + height <= top) {
        y += height + kCardGap;
        continue;
      }
      if (y >= bottom) break;

      drawCardBody(comment.author, comment.body, comment.ip, comment.date, textX, y + kCardPadY, textWidth,
                   bodyHeight, metaHeight);

      if (comment.commentId[0] != '\0') {
        const int chipY = y + height - kCardPadY - chipH;
        char like[64];
        snprintf(like, sizeof(like), "%s %u", tr(STR_JJWXC_COMMENT_LIKE),
                 static_cast<unsigned>(comment.agreeNum));
        const int likeW = drawPill(textX, chipY, chipH, like, comment.readerAgree == 1);
        addClippedHit(HitKind::Like, static_cast<uint16_t>(i), textX, chipY, likeW, chipH);

        char replies[64];
        snprintf(replies, sizeof(replies), "%s %u", tr(STR_JJWXC_COMMENT_REPLIES),
                 static_cast<unsigned>(comment.replyTotal));
        const int replyW = drawPill(textX + likeW + 10, chipY, chipH, replies, false);
        addClippedHit(HitKind::Reply, static_cast<uint16_t>(i), textX + likeW + 10, chipY, replyW, chipH);
      } else {
        // A v1 sidecar has no server id: the rows are readable but cannot be
        // liked or replied to until a refresh writes the v2 format.
        addClippedHit(HitKind::NeedsRefresh, static_cast<uint16_t>(i), textX, y, textColumn, height);
      }

      // A hairline between comments, inset to the text column: cheaper visually
      // than a boxed card and it keeps long bodies from running together.
      const int separatorY = y + height + kCardGap / 2;
      if (separatorY < bottom) {
        renderer.drawLine(textLeft, separatorY, textRight - 1, separatorY, 1, true);
      }
      y += height + kCardGap;
    }

    moreVisible_ = false;
    if (hasMore()) {
      const int moreWidth =
          std::min(textColumn, renderer.getTextWidth(UI_10_FONT_ID, tr(STR_JJWXC_COMMENTS_MORE),
                                                     EpdFontFamily::REGULAR) +
                                   kChipPadX * 4);
      moreX_ = textLeft + (textColumn - moreWidth) / 2;
      moreY_ = y;
      moreW_ = moreWidth;
      moreH_ = y < bottom ? std::min(moreHeight, bottom - y) : 0;
      if (y >= top && y < bottom && moreH_ > 0) {
        moreVisible_ = true;
        renderer.drawRoundedRect(moreX_, moreY_, moreW_, moreH_, 1, moreH_ / 2, true);
        const char* moreLabel = tr(STR_JJWXC_COMMENTS_MORE);
        const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, moreLabel, EpdFontFamily::REGULAR);
        renderer.drawText(UI_10_FONT_ID, moreX_ + (moreW_ - labelWidth) / 2, moreY_ + 8, moreLabel, true,
                          EpdFontFamily::REGULAR);
      }
    }
  }

  // Scroll position indicator. A one-pixel outline is the track, a solid block
  // the thumb; both are drawn after the text clip so the bar stays crisp at the
  // content edge.
  if (contentHeight_ > viewportHeight_) {
    renderer.drawRect(scrollbarX, top, kScrollbarWidth, viewportHeight_, 1, true);
    int thumbHeight =
        static_cast<int>(static_cast<long long>(viewportHeight_) * viewportHeight_ / contentHeight_);
    thumbHeight = std::clamp(thumbHeight, 24, std::max(24, viewportHeight_ - 2));
    const int travel = viewportHeight_ - thumbHeight;
    const int thumbY = top + static_cast<int>(static_cast<long long>(travel) * scrollY_ / maxScroll());
    renderer.fillRect(scrollbarX + 1, thumbY + 1, kScrollbarWidth - 2, thumbHeight - 2, true);
  }
}

void JjwxcCommentActivity::drawReplies(const int top) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int left = safe.x + metrics.contentSidePadding;
  const int right = safe.x + safe.width - metrics.contentSidePadding;
  const int bottom = safe.y + safe.height - metrics.buttonHintsHeight - metrics.verticalSpacing;
  hitCount_ = 0;
  if (right - left <= 0 || bottom <= top) {
    viewportHeight_ = 0;
    contentHeight_ = 0;
    moreVisible_ = false;
    return;
  }
  viewportHeight_ = bottom - top;
  scrollY_ = repliesScrollY_;

  const int scrollbarX = right - kScrollbarWidth;
  const int textLeft = left + kCardPadX;
  const int textRight = right - kCardPadX - kScrollbarGap - kScrollbarWidth;
  const int textColumn = textRight - textLeft;
  const int textWidth = textColumn - kTextInset * 2 - kWrapSafety;
  if (textWidth <= 0) {
    viewportHeight_ = 0;
    contentHeight_ = 0;
    moreVisible_ = false;
    return;
  }

  const int metaHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int chipH = cardChipHeight(renderer);
  const int moreHeight = bodyHeight + 16;

  uint16_t heights[kMaxReplies];
  contentHeight_ = 0;
  for (size_t i = 0; i < replies_.size(); ++i) {
    heights[i] = static_cast<uint16_t>(replyCardHeight(replies_[i], textWidth));
    contentHeight_ += heights[i] + kCardGap;
  }
  if (replyHasMore()) contentHeight_ += moreHeight;
  if (scrollRepliesToBottom_) {
    scrollRepliesToBottom_ = false;
    scrollY_ = static_cast<uint32_t>(maxScroll());
  } else if (scrollY_ > static_cast<uint32_t>(maxScroll())) {
    scrollY_ = static_cast<uint32_t>(maxScroll());
  }
  repliesScrollY_ = scrollY_;

  const auto drawPill = [&](const int x, const int y, const int h, const char* label, const bool filled) {
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, label, EpdFontFamily::REGULAR);
    const int w = textW + 20;
    if (filled) {
      renderer.fillRoundedRect(x, y, w, h, h / 2, Color::Black);
    } else {
      renderer.drawRoundedRect(x, y, w, h, 1, h / 2, true);
    }
    const int textH = renderer.getLineHeight(SMALL_FONT_ID);
    renderer.drawText(SMALL_FONT_ID, x + 10, y + (h - textH) / 2, label, !filled, EpdFontFamily::REGULAR);
    return w;
  };
  const auto addClippedHit = [&](const HitKind kind, const uint16_t index, const int x, const int y, const int w,
                                 const int h) {
    const int y0 = std::max(y, top);
    const int y1 = std::min(y + h, bottom);
    if (y1 > y0) addHit(kind, index, x, y0, w, y1 - y0);
  };

  {
    GfxRenderer::ClipScope clip(renderer, textLeft, top, textColumn, viewportHeight_);
    const int textX = textLeft + kTextInset;

    int y = top - static_cast<int>(scrollY_);
    for (size_t i = 0; i < replies_.size(); ++i) {
      const auto& reply = replies_[i];
      const int height = heights[i];
      if (y + height <= top) {
        y += height + kCardGap;
        continue;
      }
      if (y >= bottom) break;

      drawCardBody(reply.author, reply.body, reply.ip, reply.date, textX, y + kCardPadY, textWidth, bodyHeight,
                   metaHeight);

      if (reply.replyId[0] != '\0') {
        const int chipY = y + height - kCardPadY - chipH;
        char like[64];
        snprintf(like, sizeof(like), "%s %u", tr(STR_JJWXC_COMMENT_LIKE), static_cast<unsigned>(reply.agreeNum));
        const int likeW = drawPill(textX, chipY, chipH, like, reply.readerAgree == 1);
        addClippedHit(HitKind::Like, static_cast<uint16_t>(i), textX, chipY, likeW, chipH);
      }

      const int separatorY = y + height + kCardGap / 2;
      if (separatorY < bottom) {
        renderer.drawLine(textLeft, separatorY, textRight - 1, separatorY, 1, true);
      }
      y += height + kCardGap;
    }

    moreVisible_ = false;
    if (replyHasMore()) {
      const int moreWidth =
          std::min(textColumn, renderer.getTextWidth(UI_10_FONT_ID, tr(STR_JJWXC_COMMENTS_MORE),
                                                     EpdFontFamily::REGULAR) +
                                   kChipPadX * 4);
      moreX_ = textLeft + (textColumn - moreWidth) / 2;
      moreY_ = y;
      moreW_ = moreWidth;
      moreH_ = y < bottom ? std::min(moreHeight, bottom - y) : 0;
      if (y >= top && y < bottom && moreH_ > 0) {
        moreVisible_ = true;
        renderer.drawRoundedRect(moreX_, moreY_, moreW_, moreH_, 1, moreH_ / 2, true);
        const char* moreLabel = tr(STR_JJWXC_COMMENTS_MORE);
        const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, moreLabel, EpdFontFamily::REGULAR);
        renderer.drawText(UI_10_FONT_ID, moreX_ + (moreW_ - labelWidth) / 2, moreY_ + 8, moreLabel, true,
                          EpdFontFamily::REGULAR);
      }
    }
  }

  if (contentHeight_ > viewportHeight_) {
    renderer.drawRect(scrollbarX, top, kScrollbarWidth, viewportHeight_, 1, true);
    int thumbHeight =
        static_cast<int>(static_cast<long long>(viewportHeight_) * viewportHeight_ / contentHeight_);
    thumbHeight = std::clamp(thumbHeight, 24, std::max(24, viewportHeight_ - 2));
    const int travel = viewportHeight_ - thumbHeight;
    const int thumbY = top + static_cast<int>(static_cast<long long>(travel) * scrollY_ / maxScroll());
    renderer.fillRect(scrollbarX + 1, thumbY + 1, kScrollbarWidth - 2, thumbHeight - 2, true);
  }
}

void JjwxcCommentActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight};
  char subtitle[96];
  if (view_ == View::Replies) {
    snprintf(subtitle, sizeof(subtitle), "%s · %u", tr(STR_JJWXC_COMMENT_REPLIES),
             static_cast<unsigned>(replyTotal_));
  } else if (state_ == State::Ready || state_ == State::Empty ||
             (commentTotal_ > 0 && state_ != State::Loading)) {
    snprintf(subtitle, sizeof(subtitle), tr(STR_JJWXC_COMMENTS_PARAGRAPH_FMT),
             static_cast<unsigned long>(paragraphIndex_), static_cast<unsigned long>(commentTotal_));
  } else {
    snprintf(subtitle, sizeof(subtitle), tr(STR_JJWXC_COMMENTS_AT_FMT),
             static_cast<unsigned long>(paragraphIndex_));
  }
  GUI.drawHeader(renderer, header, tr(STR_JJWXC_COMMENTS), subtitle);

  const int contentTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int left = safe.x + metrics.contentSidePadding;
  const int right = safe.x + safe.width - metrics.contentSidePadding;
  drawRefreshChip(right, contentTop, kChipHeight);
  drawActionChip(refreshX_ - kToolbarGap, contentTop, kChipHeight,
                 view_ == View::Replies ? tr(STR_JJWXC_COMMENT_REPLY) : tr(STR_JJWXC_COMMENT_WRITE), actionX_,
                 actionW_);
  actionY_ = contentTop;
  actionH_ = kChipHeight;

  {
    char tip[192];
    tip[0] = '\0';
    if (!feedback_.empty()) {
      snprintf(tip, sizeof(tip), "%s", feedback_.c_str());
    } else if (showingCache_ && state_ == State::Ready && view_ == View::Comments) {
      bool legacyRows = false;
      for (const auto& c : comments_) {
        if (c.commentId[0] == '\0') {
          legacyRows = true;
          break;
        }
      }
      snprintf(tip, sizeof(tip), "%s",
               legacyRows ? tr(STR_JJWXC_COMMENT_CACHED_ONLY) : tr(STR_JJWXC_STATE_CACHED));
    }
    if (tip[0] != '\0') {
      const int labelY = contentTop + (kChipHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
      const int maxW = std::max(24, actionX_ - (left + kCardPadX) - 10);
      truncateText(renderer, SMALL_FONT_ID, tip, maxW);
      renderer.drawText(SMALL_FONT_ID, left + kCardPadX, labelY, tip, true, EpdFontFamily::REGULAR);
    }
  }
  const int listTop = contentTop + kChipHeight + kToolbarGap;

  if (view_ == View::Replies) {
    if (replyLoading_) {
      drawNotice(tr(STR_JJWXC_LOADING), nullptr);
    } else if (replyFailed_) {
      drawNotice(replyErrorText_ != nullptr ? replyErrorText_ : tr(STR_JJWXC_ERR_UNKNOWN), nullptr);
    } else if (replies_.empty()) {
      drawNotice(tr(STR_JJWXC_COMMENT_NO_REPLIES), nullptr);
    } else {
      drawReplies(listTop);
    }
  } else {
    switch (state_) {
      case State::Loading:
        drawNotice(tr(STR_JJWXC_LOADING), nullptr);
        break;
      case State::Empty:
        drawNotice(tr(STR_JJWXC_EMPTY), nullptr);
        break;
      case State::Failed:
        drawNotice(errorText_ != nullptr ? errorText_ : tr(STR_JJWXC_ERR_UNKNOWN), nullptr);
        break;
      case State::Ready:
        drawList(listTop);
        break;
    }
  }

  const char* second = "";
  if (loadingMore_ || refreshing_ || replyLoading_) {
    second = tr(STR_JJWXC_LOADING);
  } else if (moreFailed_ || replyFailed_) {
    second = tr(STR_JJWXC_ERR_NETWORK);
  } else if (view_ == View::Replies) {
    second = tr(STR_JJWXC_COMMENT_REPLY);
  } else if (state_ == State::Ready && hasMore()) {
    second = tr(STR_JJWXC_COMMENTS_MORE);
  } else {
    second = tr(STR_JJWXC_COMMENTS_REFRESH);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), second, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
