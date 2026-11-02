#include "JjwxcCommentFetcher.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <JjwxcComments.h>
#include <JjwxcCrypto.h>
#include <JjwxcWapLogin.h>
#include <Logging.h>
#include <Memory.h>
#include <TxtEncoding.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "JjwxcStore.h"

namespace {
constexpr const char* kTag = "JJWXC";

struct BufferCollector {
  char* buf = nullptr;
  size_t cap = 0;
  size_t len = 0;
  bool overflow = false;
};

bool collectBuffer(void* ctx, const uint8_t* data, const size_t len) {
  auto* c = static_cast<BufferCollector*>(ctx);
  if (c == nullptr || data == nullptr) return false;
  if (c->len + len > c->cap) {
    c->overflow = true;
    return false;
  }
  memcpy(c->buf + c->len, data, len);
  c->len += len;
  return true;
}

int responseBusinessCode(const char* body) {
  if (body == nullptr) return -1;
  const char* p = strstr(body, "\"code\"");
  if (p == nullptr) return -1;
  p = strchr(p, ':');
  if (p == nullptr) return -1;
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '"') ++p;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    ++p;
  }
  int v = 0;
  bool any = false;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    any = true;
    ++p;
  }
  return any ? (neg ? -v : v) : -1;
}

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

bool onCommentCount(void* ctx, const jjwxc::ParagraphComment& item, size_t) {
  auto* c = static_cast<CommentCountCtx*>(ctx);
  if (c->out == nullptr || c->count >= c->cap) return false;
  const unsigned long id = strtoul(item.paragraphId, nullptr, 10);
  if (id == 0 || item.total == 0) return true;
  c->out[c->count].paragraphId = static_cast<uint32_t>(id);
  c->out[c->count].total = static_cast<uint16_t>(item.total > 65535 ? 65535 : item.total);
  ++c->count;
  return true;
}

bool onPrefetchComment(void* ctx, const jjwxc::CommentItem& item, size_t) {
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

}  // namespace

JjwxcCommentFetcher::JjwxcCommentFetcher(jjwxc::CookieJar& jar, jjwxc::WapClient& wap, uint8_t* scratch,
                                         const size_t scratchCap)
    : jar_(jar), wap_(wap), scratch_(scratch), scratchCap_(scratchCap) {}

void JjwxcCommentFetcher::setNovelId(const char* novelId) {
  novelId_ = novelId != nullptr ? novelId : "";
}

void JjwxcCommentFetcher::setCancelFlags(const std::atomic<bool>* cancel, const std::atomic<bool>* pause) {
  cancel_ = cancel;
  pause_ = pause;
}

bool JjwxcCommentFetcher::stopped() const {
  return (cancel_ != nullptr && cancel_->load()) || (pause_ != nullptr && pause_->load());
}

JjwxcCommentFetcher::FetchResult JjwxcCommentFetcher::fetchCounts(const uint32_t chapterId, const bool includeBodies,
                                                                  const uint32_t prefetchMs,
                                                                  const size_t prefetchRequests) {
  if (scratch_ == nullptr || scratchCap_ < 64 * 1024) return FetchResult::Error;
  char sid[256];
  char token[400];
  token[0] = '\0';
  if (jar_.get("sid", sid, sizeof(sid), nowUnix_) != 0) {
    char b64[400];
    const bool encodedOk =
        jjwxc::Base64::encode(reinterpret_cast<const uint8_t*>(sid), strlen(sid), b64, sizeof(b64)) > 0 &&
        jjwxc::wapUrlEncode(b64, strlen(b64), token, sizeof(token)) > 0;
    if (!encodedOk) token[0] = '\0';
  }
  char url[768];
  const int n = snprintf(url, sizeof(url),
                         "https://m.jjwxc.net/app.jjwxc/Pc/comment/getNovelParagraphCommentNum"
                         "?versionCode=489&novelid=%s&chapterid=%lu%s%s",
                         novelId_.c_str(), static_cast<unsigned long>(chapterId), token[0] ? "&token=" : "", token);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return FetchResult::Error;
  auto* page = reinterpret_cast<char*>(scratch_);
  BufferCollector col{page, 64 * 1024, 0, false};
  jjwxc::WapOptions opt;
  opt.referer = "https://m.jjwxc.net/book2/";
  jjwxc::WapResponse res;
  const int http = wap_.get(url, opt, &collectBuffer, &col, res);
  if (http != 200 || res.sawRedirect || col.overflow || col.len == 0) {
    LOG_INF(kTag, "comment counts unavailable for chapter %lu (http=%d)", (unsigned long)chapterId, http);
    if (res.sawRedirect || http == 301 || http == 302 || http == 303 || http == 307 || http == 308 ||
        strstr(res.finalUrl, "/my/login") != nullptr) {
      return FetchResult::Auth;
    }
    return http < 0 ? FetchResult::Network : FetchResult::Error;
  }
  page[col.len] = '\0';
  const int businessCode = responseBusinessCode(page);
  if (businessCode == 1004 || businessCode == 1001 || businessCode == 10) return FetchResult::Auth;
  if (businessCode >= 0 && businessCode != 200 && businessCode != 0) return FetchResult::Error;

  // 128 entries is far past any real chapter's commented-paragraph count, and
  // keeps the worker stack free for the TLS handshake that follows.
  constexpr size_t kMaxChapterCounts = 128;
  JjwxcCommentSidecar::Count counts[kMaxChapterCounts];
  CommentCountCtx ctx{counts, kMaxChapterCounts, 0};
  jjwxc::parseParagraphComments(page, col.len, &onCommentCount, &ctx);
  // Drop the old sidecar first: after a repackage the old paragraph ids may no
  // longer match this body, and a stale bubble is worse than no bubble.
  const std::string path = JjwxcStore::commentCountsPath(novelId_.c_str(), chapterId);
  Storage.remove(path.c_str());
  // An empty table is still written. Without it a chapter with no comments is
  // indistinguishable from one never checked, and the whole-book pass would
  // re-request its counts on every run.
  if (!JjwxcStore::saveCommentCounts(novelId_.c_str(), chapterId, counts, ctx.count)) {
    LOG_ERR(kTag, "comment sidecar write failed for chapter %lu", (unsigned long)chapterId);
    return FetchResult::Error;
  }
  LOG_INF(kTag, "comment counts: chapter %lu %u paragraphs", (unsigned long)chapterId, (unsigned)ctx.count);
  if (includeBodies && ctx.count > 0) {
    prefetchBodies(chapterId, counts, ctx.count, millis() + prefetchMs, prefetchRequests);
  }
  return ctx.count == 0 ? FetchResult::Empty : FetchResult::Ok;
}

JjwxcCommentFetcher::PrefetchResult JjwxcCommentFetcher::prefetchBodies(
    const uint32_t chapterId, const JjwxcCommentSidecar::Count* counts, const size_t count,
    const uint32_t deadlineMs, const size_t requestBudget, const size_t beginIndex) {
  PrefetchResult result;
  result.nextIndex = beginIndex < count ? beginIndex : count;
  if (counts == nullptr || count == 0) return result;
  if (scratch_ == nullptr || scratchCap_ < 128 * 1024) return result;
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
  if (jar_.get("sid", sid, sizeof(sid), nowUnix_) != 0) {
    char b64[400];
    const bool encodedOk =
        jjwxc::Base64::encode(reinterpret_cast<const uint8_t*>(sid), strlen(sid), b64, sizeof(b64)) > 0 &&
        jjwxc::wapUrlEncode(b64, strlen(b64), token, sizeof(token)) > 0;
    if (!encodedOk) token[0] = '\0';
  }

  for (size_t i = beginIndex; i < count; ++i) {
    if (stopped()) break;
    const uint32_t paragraphId = counts[i].paragraphId;
    if (paragraphId == 0 || paragraphId > 0xFFFF || counts[i].total == 0) {
      result.nextIndex = i + 1;
      continue;
    }
    const std::string bodyPath = JjwxcStore::commentBodiesPath(novelId_.c_str(), chapterId, paragraphId);
    if (Storage.exists(bodyPath.c_str())) {
      ++result.skipped;
      result.nextIndex = i + 1;
      continue;
    }
    if (static_cast<int32_t>(millis() - deadlineMs) >= 0 || result.requests >= requestBudget) {
      LOG_INF(kTag, "comment prefetch budget reached (%u/%u)", (unsigned)i, (unsigned)count);
      result.budgetReached = true;
      break;
    }
    // Consumed from here on: a request that fails for a paragraph-level reason
    // must not be retried on the next call (that would loop on one bad id).
    result.nextIndex = i + 1;

    char url[768];
    const int n =
        snprintf(url, sizeof(url),
                 "https://m.jjwxc.net/app.jjwxc/Pc/comment/getCommentList"
                 "?versionCode=489&novelId=%s&chapterId=%lu&paragraph_id=%lu&offset=0&limit=%u%s%s",
                 novelId_.c_str(), static_cast<unsigned long>(chapterId), static_cast<unsigned long>(paragraphId),
                 static_cast<unsigned>(kPageSize), token[0] ? "&token=" : "", token);
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
    const int http = wap_.get(url, opt, &collectBuffer, &col, res);
    if (http != 200 || res.sawRedirect || col.overflow || col.len == 0) {
      ++result.failed;
      result.lastHttp = http;
      if (http < 0) result.networkStop = true;
      if (res.sawRedirect || http == 301 || http == 302 || http == 303 || http == 307 || http == 308 ||
          strstr(res.finalUrl, "/my/login") != nullptr) {
        result.authStop = true;
      }
      LOG_INF(kTag, "comment prefetch: para=%lu http=%d", (unsigned long)paragraphId, http);
      if (result.authStop || result.networkStop) {
        // A failed request is only "consumed" for auth; a transport failure leaves
        // nextIndex on this paragraph so the caller may reconnect and retry it.
        if (result.networkStop && !result.authStop) result.nextIndex = i;
        break;
      }
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
    jjwxc::parseCommentList(page, textLen, &onPrefetchComment, &ctx);
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
    const int businessCode = responseBusinessCode(page);
    if (businessCode == 1004 || businessCode == 1001 || businessCode == 10) {
      result.authStop = true;
      result.lastHttp = businessCode;
      break;
    }
    if (businessCode >= 0 && businessCode != 200 && businessCode != 0) {
      ++result.failed;
      result.lastHttp = businessCode;
      delay(250);
      continue;
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
