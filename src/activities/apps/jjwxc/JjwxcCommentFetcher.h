#pragma once

#include <JjwxcCookies.h>
#include <JjwxcWapClient.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <Epub/JjwxcCommentSidecar.h>

// Shared paragraph-comment network path.
//
// The JJWXC activity uses it for chapter caching and the explicit "cache comment
// bodies" pass; the background cache service uses it for the persistent queue.
// Keeping one implementation is what keeps the queue's token/encoding/refusal
// handling identical to the panel's.
class JjwxcCommentFetcher {
 public:
  // Why a comment request stopped, so callers can tell a benign "nothing left"
  // apart from a dead session or a lost radio.
  enum class FetchResult : uint8_t { Ok, Empty, Network, Auth, Error };

  struct PrefetchResult {
    size_t fetched = 0;
    size_t skipped = 0;
    size_t failed = 0;
    size_t requests = 0;
    // Index (into counts[]) one past the last paragraph this call accounted for.
    // A failed paragraph is consumed, so a resumable pass advances past it
    // instead of hammering the same request on every call.
    size_t nextIndex = 0;
    bool budgetReached = false;
    bool networkStop = false;
    bool authStop = false;
    int lastHttp = 0;
  };

  JjwxcCommentFetcher(jjwxc::CookieJar& jar, jjwxc::WapClient& wap, uint8_t* scratch, size_t scratchCap);

  void setNovelId(const char* novelId);
  void setNowUnix(uint64_t nowUnix) { nowUnix_ = nowUnix; }
  // Both flags are optional. A stopped prefetch returns early with the counters
  // reached so far, without marking anything as consumed that it did not touch.
  void setCancelFlags(const std::atomic<bool>* cancel, const std::atomic<bool>* pause);

  // Fetches the paragraph-comment totals for one chapter and writes the JJC1
  // sidecar. `includeBodies` additionally fetches the first page of every
  // commented paragraph. Best effort: the text cache must not fail because the
  // comments did.
  FetchResult fetchCounts(uint32_t chapterId, bool includeBodies, uint32_t prefetchMs = 75000,
                          size_t prefetchRequests = static_cast<size_t>(-1));

  // Fetches the first page of every commented paragraph in `counts`. The caller
  // supplies an absolute deadline and request budget so a whole-book/background
  // pass can stop cleanly between paragraphs; `beginIndex` makes it resumable.
  PrefetchResult prefetchBodies(uint32_t chapterId, const JjwxcCommentSidecar::Count* counts, size_t count,
                                uint32_t deadlineMs, size_t requestBudget, size_t beginIndex = 0);

 private:
  jjwxc::CookieJar& jar_;
  jjwxc::WapClient& wap_;
  uint8_t* scratch_;
  size_t scratchCap_;
  std::string novelId_;
  uint64_t nowUnix_ = 0;
  const std::atomic<bool>* cancel_ = nullptr;
  const std::atomic<bool>* pause_ = nullptr;

  bool stopped() const;
};
