#include "JjwxcCacheService.h"

#ifdef BOARD_HAS_PSRAM

#include <Arduino.h>
#include <HalStorage.h>
#include <JjwxcWap.h>
#include <Logging.h>
#include <Memory.h>
#include <NetworkStartup.h>
#include <WiFi.h>
#include <WifiCredentialStore.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>

#include <activities/RenderLock.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <netdb.h>
#include <sys/socket.h>
#include <memory>
#include <new>
#include <string>

#include "BackgroundWork.h"
#include "JjwxcCommentFetcher.h"
#include "JjwxcHttpTransport.h"
#include "JjwxcStore.h"
#include "util/BookCacheUtils.h"

namespace jjwxc_cache_service {
namespace {

constexpr const char* kTag = "JJWXC";
constexpr size_t kScratchBytes = 1024 * 1024;
constexpr uint32_t kStackWords = 32 * 1024;  // xTaskCreatePinnedToCoreWithCaps counts words
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kWifiReconnectWaitMs = 60000;
constexpr uint32_t kRetryCooldownSec = 10 * 60;
constexpr uint32_t kStandbyRetryGapMs = 60 * 1000;
constexpr uint32_t kPollIntervalMs = 500;

uint64_t cookieClock() {
  const time_t now = time(nullptr);
  return now > 1700000000 ? static_cast<uint64_t>(now) : 0;
}

struct Service {
  SemaphoreHandle_t lock = nullptr;
  SemaphoreHandle_t wake = nullptr;
  TaskHandle_t task = nullptr;
  bool started = false;

  JjwxcCacheQueue::Item items[JjwxcCacheQueue::kMaxItems];
  size_t count = 0;
  bool userPaused = false;
  bool armed = false;  // a trigger (enqueue/standby/wake/user) asked for a run
  bool readerForeground = false;
  bool foregroundJob = false;
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> workerActive{false};

  std::atomic<uint32_t> itemIndex{0};
  std::atomic<uint32_t> curOrdinal{0};
  std::atomic<uint32_t> doneChapters{0};
  std::atomic<uint32_t> totalChapters{0};
  std::atomic<uint32_t> doneParagraphs{0};
  std::atomic<uint32_t> totalParagraphs{0};
  std::atomic<uint32_t> requests{0};
  std::atomic<uint32_t> revision{0};
  std::atomic<uint32_t> badgeVersion{0};
  Badge lastBadge = Badge::None;

  uint32_t retryAtEpoch = 0;
  uint32_t lastAttemptEpoch = 0;
  uint32_t lastPollMs = 0;
  bool lastWifiConnected = false;

  bool ackPending = false;
  char ackNovel[16] = {};
  bool pendingEnqueue = false;
  char pendingNovel[16] = {};
  char pendingTitle[96] = {};
  uint32_t pendingFrom = 0;
  uint32_t pendingTo = 0;
  bool pendingClearDone = false;

  // Owned only while a run is active; freeing them is what gives the reader its
  // PSRAM/internal RAM back.
  memory::ByteBuffer scratch;
  memory::ByteBuffer jarBuf;
  memory::ByteBuffer wapIoBuf;
  std::unique_ptr<JjwxcHttpTransport> transport;
  std::unique_ptr<jjwxc::WapClient> wap;
  jjwxc::CookieJar* jar = nullptr;
  jjwxc::WapIo* wapIo = nullptr;
  size_t scratchCap = 0;
  bool radioOwned = false;
};

Service& service() {
  static Service s;
  return s;
}

class Guard {
 public:
  explicit Guard(SemaphoreHandle_t lock) : lock_(lock) {
    if (lock_ != nullptr) xSemaphoreTake(lock_, portMAX_DELAY);
  }
  ~Guard() {
    if (lock_ != nullptr) xSemaphoreGive(lock_);
  }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

 private:
  SemaphoreHandle_t lock_;
};

uint32_t epochNow() { return static_cast<uint32_t>(time(nullptr)); }

bool anyWorkLocked() {
  auto& s = service();
  for (size_t i = 0; i < s.count; ++i) {
    const auto st = static_cast<JjwxcCacheQueue::State>(s.items[i].state);
    if (st == JjwxcCacheQueue::State::Queued || st == JjwxcCacheQueue::State::NeedsWifi) return true;
  }
  return false;
}

Badge computeBadgeLocked() {
  auto& s = service();
  if (s.workerActive.load()) return Badge::Running;
  bool anyDone = false;
  bool anyPending = false;
  for (size_t i = 0; i < s.count; ++i) {
    const auto st = static_cast<JjwxcCacheQueue::State>(s.items[i].state);
    if (st == JjwxcCacheQueue::State::Done) {
      anyDone = true;
    } else {
      anyPending = true;
    }
  }
  if (anyPending || (s.userPaused && s.count > 0)) return Badge::Paused;
  if (anyDone) return Badge::Done;
  return Badge::None;
}

void bumpBadgeLocked() {
  auto& s = service();
  const Badge b = computeBadgeLocked();
  if (b != s.lastBadge) {
    s.lastBadge = b;
    s.badgeVersion.fetch_add(1);
  }
}

void saveQueueLocked() {
  auto& s = service();
  if (!JjwxcStore::saveCacheQueue(s.items, s.count)) LOG_ERR(kTag, "cache queue save failed");
}

void saveQueueCopy() {
  auto& s = service();
  JjwxcCacheQueue::Item copy[JjwxcCacheQueue::kMaxItems];
  size_t n = 0;
  {
    Guard guard(s.lock);
    n = s.count;
    memcpy(copy, s.items, sizeof(JjwxcCacheQueue::Item) * n);
  }
  if (!JjwxcStore::saveCacheQueue(copy, n)) LOG_ERR(kTag, "cache queue save failed");
}

bool shouldRunLocked() {
  auto& s = service();
  if (!s.started || s.workerActive.load()) return false;
  if (!s.armed) return false;
  if (s.userPaused || s.readerForeground || s.foregroundJob) return false;
  if (s.ackPending) return false;
  if (s.stopRequested.load()) return false;
  if (epochNow() < s.retryAtEpoch) return false;
  return anyWorkLocked();
}

uint32_t readingOrdinalForNovel(const char* novelId, bool& valid) {
  valid = false;
  if (novelId == nullptr || novelId[0] == '\0') return 0;
  JjwxcStore::NovelRecord rec;
  if (!JjwxcStore::loadNovel(novelId, rec)) return 0;
  const std::string epubPath = JjwxcStore::finalBookPath(rec);
  const std::string cachePath = bookCachePath(epubPath);
  if (cachePath.empty()) return 0;
  HalFile file;
  if (!Storage.openFileForRead(kTag, cachePath + "/progress.bin", file)) return 0;
  uint8_t data[10] = {};
  const int got = file.read(data, sizeof(data));
  if (got != 4 && got != 6 && got != 10) return 0;
  const uint32_t spine = static_cast<uint32_t>(data[0] | (data[1] << 8));
  JjwxcStore::SpineEntry entry;
  if (!JjwxcStore::readSpineEntry(novelId, spine, entry)) return 0;
  valid = true;
  return entry.ordinal;
}

bool ensureSession(Service& s) {
  const uint64_t now = cookieClock();
  if (!s.scratch) {
    s.scratch = memory::makePsramByteBufferUninitializedNoThrow(kScratchBytes);
    s.scratchCap = s.scratch ? kScratchBytes : 0;
  }
  if (!s.scratch || s.scratchCap < 512 * 1024) {
    LOG_ERR(kTag, "cache service: no PSRAM scratch (%u bytes)", static_cast<unsigned>(s.scratchCap));
    return false;
  }
  if (!s.jarBuf) {
    s.jarBuf = memory::makePsramByteBufferNoThrow(sizeof(jjwxc::CookieJar));
    if (s.jarBuf) s.jar = new (s.jarBuf.get()) jjwxc::CookieJar();
  }
  if (!s.wapIoBuf) {
    s.wapIoBuf = memory::makePsramByteBufferNoThrow(sizeof(jjwxc::WapIo));
    if (s.wapIoBuf) s.wapIo = new (s.wapIoBuf.get()) jjwxc::WapIo();
  }
  if (s.jar == nullptr || s.wapIo == nullptr) {
    LOG_ERR(kTag, "cache service: no PSRAM for session objects");
    return false;
  }
  if (!s.transport) s.transport = makeUniqueNoThrow<JjwxcHttpTransport>();
  if (!s.transport) {
    LOG_ERR(kTag, "cache service: no RAM for transport");
    return false;
  }
  if (!s.wap) {
    s.wap = makeUniqueNoThrow<jjwxc::WapClient>(*s.transport, *s.jar, *s.wapIo);
    if (!s.wap) {
      LOG_ERR(kTag, "cache service: no RAM for WAP client");
      return false;
    }
  }
  if (!JjwxcStore::loadCookies(*s.jar, now) || !JjwxcStore::hasSession(*s.jar, now)) {
    LOG_INF(kTag, "cache service: no WAP session");
    return false;
  }
  s.wap->setNowUnix(now);
  LOG_INF(kTag, "cache service: session restored, %u cookies", static_cast<unsigned>(s.jar->count(now)));
  return true;
}

void releaseSession(Service& s) {
  s.wap.reset();
  s.transport.reset();
  if (s.jar != nullptr) {
    s.jar->~CookieJar();
    s.jar = nullptr;
  }
  if (s.wapIo != nullptr) {
    s.wapIo->~WapIo();
    s.wapIo = nullptr;
  }
  s.jarBuf.reset();
  s.wapIoBuf.reset();
  s.scratch.reset();
  s.scratchCap = 0;
}

bool ensureWifi(Service& s) {
  if (WiFi.status() == WL_CONNECTED) {
    s.radioOwned = false;
    return true;
  }
  std::string ssid;
  std::string pass;
  {
    RenderLock lock;
    if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
    const std::string last = WIFI_STORE.getLastConnectedSsid();
    if (last.empty()) return false;
    const auto cred = WIFI_STORE.findCredential(last);
    if (!cred) return false;
    ssid = cred->ssid;
    pass = cred->password;
  }
  LOG_INF(kTag, "cache service: connecting saved network '%s'", ssid.c_str());
  WiFi.persistent(false);
  if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
  if (pass.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  s.radioOwned = true;
  const uint32_t start = millis();
  while (millis() - start < kWifiConnectTimeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      LOG_INF(kTag, "cache service: Wi-Fi connected");
      return true;
    }
    delay(250);
  }
  LOG_INF(kTag, "cache service: saved network did not connect");
  return false;
}

void stopRadio(Service& s) {
  if (!s.radioOwned) return;
  s.radioOwned = false;
  NetworkStartup::releaseRadio();
}

void markItemState(const uint8_t state, const char* error) {
  auto& s = service();
  uint32_t idx = s.itemIndex.load();
  Guard guard(s.lock);
  if (idx >= s.count || static_cast<JjwxcCacheQueue::State>(s.items[idx].state) == JjwxcCacheQueue::State::Done) {
    idx = s.count;
    for (size_t i = 0; i < s.count; ++i) {
      if (static_cast<JjwxcCacheQueue::State>(s.items[i].state) != JjwxcCacheQueue::State::Done) {
        idx = static_cast<uint32_t>(i);
        break;
      }
    }
    if (idx >= s.count) return;
  }
  s.items[idx].state = state;
  s.items[idx].updatedEpoch = epochNow();
  if (error != nullptr) snprintf(s.items[idx].lastError, sizeof(s.items[idx].lastError), "%s", error);
  else s.items[idx].lastError[0] = '\0';
  if (state == static_cast<uint8_t>(JjwxcCacheQueue::State::NeedsWifi)) {
    s.retryAtEpoch = epochNow() + kRetryCooldownSec;
    s.items[idx].retryAtEpoch = s.retryAtEpoch;
  }
  saveQueueLocked();
  bumpBadgeLocked();
}

void markNeedsWifi() { markItemState(static_cast<uint8_t>(JjwxcCacheQueue::State::NeedsWifi), "wifi"); }
void markNeedsLogin() { markItemState(static_cast<uint8_t>(JjwxcCacheQueue::State::NeedsLogin), "login"); }
void markError() { markItemState(static_cast<uint8_t>(JjwxcCacheQueue::State::Error), "error"); }

bool waitReconnectOrGiveUp() {
  auto& s = service();
  if (WiFi.status() == WL_CONNECTED) return true;
  const uint32_t start = millis();
  while (millis() - start < kWifiReconnectWaitMs) {
    if (s.stopRequested.load() || s.userPaused || s.readerForeground || s.foregroundJob) return false;
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(500);
  }
  return false;
}

enum class ItemResult : uint8_t { Done, Suspended, Network, Auth, Error };

ItemResult runItem(const uint32_t index) {
  auto& s = service();
  JjwxcCacheQueue::Item item;
  size_t count = 0;
  {
    Guard guard(s.lock);
    if (index >= s.count) return ItemResult::Done;
    item = s.items[index];
    count = s.count;
  }
  (void)count;
  JjwxcStore::TocReader toc;
  if (!toc.open(item.novelId)) return ItemResult::Error;
  uint32_t from = item.fromOrdinal;
  uint32_t to = item.toOrdinal;
  if (toc.count() == 0) {
    toc.close();
    return ItemResult::Error;
  }
  if (to >= toc.count()) to = toc.count() - 1;
  if (from > to) {
    toc.close();
    return ItemResult::Error;
  }
  const uint32_t len = to - from + 1;
  if (item.doneChapters >= len) {
    toc.close();
    return ItemResult::Done;
  }
  uint32_t firstStep = item.doneChapters;
  s.itemIndex.store(index);
  s.totalChapters.store(len);
  s.doneChapters.store(firstStep);
  s.totalParagraphs.store(0);
  s.doneParagraphs.store(0);
  s.requests.store(0);
  s.revision.fetch_add(1);

  JjwxcCommentFetcher fetcher(*s.jar, *s.wap, s.scratch.get(), s.scratchCap);
  fetcher.setNovelId(item.novelId);
  fetcher.setNowUnix(cookieClock());
  fetcher.setCancelFlags(&s.stopRequested, nullptr);

  constexpr size_t kMaxCounts = JjwxcCommentSidecar::kMaxEntries;
  JjwxcCommentSidecar::Count counts[kMaxCounts];
  for (uint32_t step = firstStep; step < len; ++step) {
    if (s.stopRequested.load() || s.userPaused || s.readerForeground || s.foregroundJob) {
      toc.close();
      return ItemResult::Suspended;
    }
    const uint32_t ord = JjwxcCacheQueue::ordinalForStep(from, to, item.anchorOrdinal, item.anchorValid(), step);
    s.curOrdinal.store(ord);
    JjwxcStore::TocRecord rec;
    if (!toc.read(ord, rec)) {
      toc.close();
      return ItemResult::Error;
    }
    if (!JjwxcStore::isReadable(rec.state)) {
      s.doneChapters.fetch_add(1);
      s.revision.fetch_add(1);
      continue;
    }

    size_t paragraphCount = 0;
    bool haveCounts =
        JjwxcStore::loadCommentCounts(item.novelId, rec.chapterId, counts, kMaxCounts, paragraphCount);
    if (haveCounts && paragraphCount > kMaxCounts) paragraphCount = kMaxCounts;
    if (!haveCounts) {
      bool retried = false;
      while (true) {
        if (s.stopRequested.load() || s.userPaused || s.readerForeground || s.foregroundJob) {
          toc.close();
          return ItemResult::Suspended;
        }
        s.transport->setRequestTimeoutMs(retried ? 10000 : JjwxcHttpTransport::kDefaultTimeoutMs);
        s.transport->setAllowReconnect(!retried);
        const auto r = fetcher.fetchCounts(rec.chapterId, false);
        s.transport->setAllowReconnect(true);
        s.transport->setRequestTimeoutMs(JjwxcHttpTransport::kDefaultTimeoutMs);
        if (r == JjwxcCommentFetcher::FetchResult::Ok || r == JjwxcCommentFetcher::FetchResult::Empty) break;
        if (r == JjwxcCommentFetcher::FetchResult::Auth) {
          toc.close();
          return ItemResult::Auth;
        }
        if (r == JjwxcCommentFetcher::FetchResult::Network) {
          if (!retried && waitReconnectOrGiveUp()) {
            retried = true;
            s.transport->resetSession();
            delay(800);
            continue;
          }
          toc.close();
          return ItemResult::Network;
        }
        toc.close();
        return ItemResult::Error;
      }
      paragraphCount = 0;
      if (!JjwxcStore::loadCommentCounts(item.novelId, rec.chapterId, counts, kMaxCounts, paragraphCount)) {
        s.doneChapters.fetch_add(1);
        s.revision.fetch_add(1);
        continue;
      }
      if (paragraphCount > kMaxCounts) paragraphCount = kMaxCounts;
    }
    if (paragraphCount == 0) {
      s.doneChapters.fetch_add(1);
      s.revision.fetch_add(1);
      continue;
    }
    s.totalParagraphs.fetch_add(static_cast<uint32_t>(paragraphCount));

    size_t start = 0;
    bool retried = false;
    while (start < paragraphCount) {
      if (s.stopRequested.load() || s.userPaused || s.readerForeground || s.foregroundJob) {
        toc.close();
        return ItemResult::Suspended;
      }
      s.transport->setRequestTimeoutMs(retried ? 10000 : JjwxcHttpTransport::kDefaultTimeoutMs);
      s.transport->setAllowReconnect(!retried);
      const auto r = fetcher.prefetchBodies(rec.chapterId, counts, paragraphCount, millis() + 20000, 1, start);
      s.transport->setAllowReconnect(true);
      s.transport->setRequestTimeoutMs(JjwxcHttpTransport::kDefaultTimeoutMs);
      if (s.stopRequested.load() || s.userPaused || s.readerForeground || s.foregroundJob) {
        toc.close();
        return ItemResult::Suspended;
      }
      if (r.networkStop) {
        if (!retried && waitReconnectOrGiveUp()) {
          retried = true;
          s.transport->resetSession();
          delay(800);
          continue;
        }
        toc.close();
        return ItemResult::Network;
      }
      retried = false;
      s.requests.fetch_add(static_cast<uint32_t>(r.requests));
      s.doneParagraphs.fetch_add(static_cast<uint32_t>(r.fetched));
      s.revision.fetch_add(1);
      if (r.authStop) {
        toc.close();
        return ItemResult::Auth;
      }
      const size_t before = start;
      start = r.nextIndex;
      if (start >= paragraphCount) break;
      if (r.requests == 0 && start <= before) break;
    }
    s.doneChapters.fetch_add(1);
    s.revision.fetch_add(1);
    {
      Guard guard(s.lock);
      if (index < s.count) {
        s.items[index].doneChapters = s.doneChapters.load();
        s.items[index].totalChapters = s.totalChapters.load();
        s.items[index].doneParagraphs = s.doneParagraphs.load();
        s.items[index].totalParagraphs = s.totalParagraphs.load();
        s.items[index].requests = s.requests.load();
        s.items[index].updatedEpoch = epochNow();
      }
      saveQueueLocked();
    }
  }
  toc.close();
  return ItemResult::Done;
}

void runSession() {
  auto& s = service();
  if (s.stopRequested.load() || s.userPaused || s.readerForeground || s.foregroundJob) return;
  s.workerActive.store(true);
  s.stopRequested.store(false);
  background_work::Guard workGuard;
  LOG_INF(kTag, "cache service: run start");
  {
    Guard guard(s.lock);
    for (size_t i = 0; i < s.count; ++i) {
      const auto st = static_cast<JjwxcCacheQueue::State>(s.items[i].state);
      if (st == JjwxcCacheQueue::State::Queued || st == JjwxcCacheQueue::State::NeedsWifi) {
        s.itemIndex.store(static_cast<uint32_t>(i));
        break;
      }
    }
  }

  if (!ensureWifi(s)) {
    // No saved network or the AP is out of range. Do not prompt; park with a
    // cooldown so a later standby/Wi-Fi return can try again.
    markNeedsWifi();
    releaseSession(s);
    stopRadio(s);
    s.workerActive.store(false);
    LOG_INF(kTag, "cache service: no usable Wi-Fi; parked");
    return;
  }
  if (!ensureSession(s)) {
    markNeedsLogin();
    releaseSession(s);
    stopRadio(s);
    s.workerActive.store(false);
    LOG_INF(kTag, "cache service: no session; parked");
    return;
  }
  s.transport->setAllowReconnect(true);
  s.transport->setRequestTimeoutMs(JjwxcHttpTransport::kDefaultTimeoutMs);

  {
    // A NeedsWifi item is auto-resumable now that a link is up. Convert before
    // the item loop, which only processes Queued rows.
    Guard guard(s.lock);
    bool changed = false;
    for (size_t i = 0; i < s.count; ++i) {
      if (static_cast<JjwxcCacheQueue::State>(s.items[i].state) == JjwxcCacheQueue::State::NeedsWifi &&
          !s.userPaused) {
        s.items[i].state = static_cast<uint8_t>(JjwxcCacheQueue::State::Queued);
        s.items[i].retryAtEpoch = 0;
        changed = true;
      }
    }
    if (changed) saveQueueLocked();
  }

  // Warm DNS on this task before the worker connects. The first lookup right
  // after a fresh association races NetworkManager's DNS-cache flush and has
  // aborted inside lwIP's udp_new_ip_type; a cache hit avoids that path.
  {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const int rc = getaddrinfo("m.jjwxc.net", "443", &hints, &resolved);
    if (resolved != nullptr) freeaddrinfo(resolved);
    LOG_INF(kTag, "cache service: dns warm rc=%d", rc);
    if (rc != 0) {
      // Do not fall through into the cold connect path: on this lwIP build that
      // path has aborted inside udp_new_ip_type right after a fresh association.
      markNeedsWifi();
      releaseSession(s);
      stopRadio(s);
      s.workerActive.store(false);
      LOG_INF(kTag, "cache service: DNS unavailable; parked");
      return;
    }
  }

  size_t total = 0;
  {
    Guard guard(s.lock);
    total = s.count;
  }
  for (size_t i = 0; i < total; ++i) {
    uint8_t state = 0;
    {
      Guard guard(s.lock);
      if (i >= s.count) break;
      state = s.items[i].state;
    }
    if (static_cast<JjwxcCacheQueue::State>(state) != JjwxcCacheQueue::State::Queued) continue;
    if (s.stopRequested.load() || s.userPaused || s.readerForeground || s.foregroundJob) break;
    const ItemResult result = runItem(static_cast<uint32_t>(i));
    if (result == ItemResult::Done) {
      Guard guard(s.lock);
      if (i < s.count) {
        s.items[i].state = static_cast<uint8_t>(JjwxcCacheQueue::State::Done);
        s.items[i].doneChapters = s.items[i].totalChapters;
        s.items[i].updatedEpoch = epochNow();
      }
      saveQueueLocked();
      bumpBadgeLocked();
      s.revision.fetch_add(1);
      continue;
    }
    if (result == ItemResult::Suspended) {
      // Keep the item Queued: this is a system pause (reader/foreground/user),
      // not a failure. The next run continues from doneChapters.
      Guard guard(s.lock);
      saveQueueLocked();
      break;
    }
    if (result == ItemResult::Network) {
      markNeedsWifi();
      break;
    }
    if (result == ItemResult::Auth) {
      markNeedsLogin();
      break;
    }
    markError();
    break;
  }

  releaseSession(s);
  stopRadio(s);
  s.workerActive.store(false);
  {
    Guard guard(s.lock);
    bumpBadgeLocked();
    saveQueueLocked();
  }
  LOG_INF(kTag, "cache service: run end");
}

void taskMain(void*) {
  auto& s = service();
  for (;;) {
    xSemaphoreTake(s.wake, portMAX_DELAY);
    if (!s.started) continue;
    bool run = false;
    {
      Guard guard(s.lock);
      run = shouldRunLocked();
    }
    if (!run) {
      Guard guard(s.lock);
      bumpBadgeLocked();
      continue;
    }
    s.lastAttemptEpoch = epochNow();
    runSession();
  }
}

void applyPendingLocked() {
  auto& s = service();
  if (s.ackPending && !s.workerActive.load()) {
    s.ackPending = false;
    size_t out = 0;
    for (size_t i = 0; i < s.count; ++i) {
      const auto st = static_cast<JjwxcCacheQueue::State>(s.items[i].state);
      if (st == JjwxcCacheQueue::State::Done && strcmp(s.items[i].novelId, s.ackNovel) == 0) {
        LOG_INF(kTag, "cache service: acknowledged done item %s", s.items[i].novelId);
        continue;
      }
      s.items[out++] = s.items[i];
    }
    s.count = out;
    saveQueueLocked();
    bumpBadgeLocked();
  }
  if (s.pendingClearDone && !s.workerActive.load()) {
    s.pendingClearDone = false;
    size_t out = 0;
    for (size_t i = 0; i < s.count; ++i) {
      if (static_cast<JjwxcCacheQueue::State>(s.items[i].state) == JjwxcCacheQueue::State::Done) continue;
      s.items[out++] = s.items[i];
    }
    s.count = out;
    saveQueueLocked();
    bumpBadgeLocked();
  }
}

void doPendingEnqueue() {
  auto& s = service();
  char novel[16];
  char title[96];
  uint32_t from = 0;
  uint32_t to = 0;
  {
    Guard guard(s.lock);
    if (!s.pendingEnqueue) return;
    memcpy(novel, s.pendingNovel, sizeof(novel));
    memcpy(title, s.pendingTitle, sizeof(title));
    from = s.pendingFrom;
    to = s.pendingTo;
    s.pendingEnqueue = false;
  }
  enqueue(novel, title, from, to);
}

}  // namespace

void begin() {
  auto& s = service();
  if (s.started) return;
  s.lock = xSemaphoreCreateMutex();
  s.wake = xSemaphoreCreateBinary();
  if (s.lock == nullptr || s.wake == nullptr) {
    LOG_ERR(kTag, "cache service: cannot create sync primitives");
    return;
  }
  JjwxcStore::loadCachePausedPref(s.userPaused);
  size_t count = 0;
  if (!JjwxcStore::loadCacheQueue(s.items, JjwxcCacheQueue::kMaxItems, count)) {
    LOG_ERR(kTag, "cache service: queue load failed; starting empty");
    count = 0;
  }
  if (count > JjwxcCacheQueue::kMaxItems) count = JjwxcCacheQueue::kMaxItems;
  s.count = count;
  for (size_t i = 0; i < s.count; ++i) {
    if (s.items[i].state == static_cast<uint8_t>(JjwxcCacheQueue::State::Running)) {
      s.items[i].state = static_cast<uint8_t>(JjwxcCacheQueue::State::Queued);
    }
    if (s.items[i].totalChapters < s.items[i].doneChapters) {
      s.items[i].totalChapters = s.items[i].doneChapters;
    }
  }
  s.lastWifiConnected = WiFi.status() == WL_CONNECTED;
  s.lastPollMs = millis();
  s.lastBadge = computeBadgeLocked();
  s.started = true;

  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(taskMain, "jjwxcCache", kStackWords, nullptr, tskIDLE_PRIORITY,
                                                  &s.task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) {
    s.task = nullptr;
    LOG_ERR(kTag, "cache service: task creation refused (%d)", static_cast<int>(ok));
  } else {
    LOG_INF(kTag, "cache service ready: %u item(s), paused=%d", static_cast<unsigned>(s.count),
            s.userPaused ? 1 : 0);
  }
}

void poll() {
  auto& s = service();
  if (!s.started) return;
  const uint32_t nowMs = millis();
  if (nowMs - s.lastPollMs < kPollIntervalMs) return;
  s.lastPollMs = nowMs;

  bool needWake = false;
  bool needEnqueue = false;
  {
    Guard guard(s.lock);
    applyPendingLocked();
    needEnqueue = s.pendingEnqueue && !s.workerActive.load();
    if (s.readerForeground || s.foregroundJob) s.stopRequested.store(true);

    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !s.lastWifiConnected) {
      // The link came back. A NeedsWifi item is auto-resumable; a user-paused
      // item is not.
      bool changed = false;
      if (!s.userPaused) {
        s.armed = true;
        for (size_t i = 0; i < s.count; ++i) {
          if (static_cast<JjwxcCacheQueue::State>(s.items[i].state) == JjwxcCacheQueue::State::NeedsWifi) {
            s.items[i].state = static_cast<uint8_t>(JjwxcCacheQueue::State::Queued);
            s.items[i].retryAtEpoch = 0;
            changed = true;
          }
        }
        s.retryAtEpoch = 0;
      }
      if (changed) saveQueueLocked();
    }
    s.lastWifiConnected = connected;
    bumpBadgeLocked();
    needWake = shouldRunLocked();
  }
  if (needEnqueue) doPendingEnqueue();
  if (needWake) xSemaphoreGive(s.wake);
}

EnqueueResult enqueue(const char* novelId, const char* title, uint32_t fromOrdinal, uint32_t toOrdinal) {
  auto& s = service();
  if (!s.started || novelId == nullptr || novelId[0] == '\0') return EnqueueResult::Invalid;
  if (fromOrdinal > toOrdinal) {
    const uint32_t t = fromOrdinal;
    fromOrdinal = toOrdinal;
    toOrdinal = t;
  }
  bool deferred = false;
  {
    Guard guard(s.lock);
    if (s.workerActive.load()) {
      // The worker owns the queue array until it stops. Record one pending
      // request and ask it to yield; poll() applies this after the stop.
      s.pendingEnqueue = true;
      snprintf(s.pendingNovel, sizeof(s.pendingNovel), "%s", novelId);
      snprintf(s.pendingTitle, sizeof(s.pendingTitle), "%s", title != nullptr ? title : "");
      s.pendingFrom = fromOrdinal;
      s.pendingTo = toOrdinal;
      s.stopRequested.store(true);
      deferred = true;
    }
  }
  if (deferred) {
    xSemaphoreGive(s.wake);
    return EnqueueResult::Ok;
  }

  Guard guard(s.lock);
  uint32_t chapterCount = toOrdinal + 1;
  JjwxcStore::TocSummary summary;
  uint32_t highest = 0;
  if (JjwxcStore::readTocSummary(novelId, summary, highest) && summary.count > 0) chapterCount = summary.count;
  if (chapterCount == 0) return EnqueueResult::NoCatalogue;
  if (toOrdinal >= chapterCount) toOrdinal = chapterCount - 1;
  if (fromOrdinal >= chapterCount) return EnqueueResult::NoCatalogue;

  int slot = -1;
  for (size_t i = 0; i < s.count; ++i) {
    if (strcmp(s.items[i].novelId, novelId) == 0) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot < 0) {
    if (s.count >= JjwxcCacheQueue::kMaxItems) {
      size_t drop = s.count;
      for (size_t i = 0; i < s.count; ++i) {
        if (static_cast<JjwxcCacheQueue::State>(s.items[i].state) == JjwxcCacheQueue::State::Done) {
          drop = i;
          break;
        }
      }
      if (drop == s.count) return EnqueueResult::Full;
      for (size_t i = drop + 1; i < s.count; ++i) s.items[i - 1] = s.items[i];
      --s.count;
    }
    slot = static_cast<int>(s.count++);
    memset(&s.items[slot], 0, sizeof(s.items[slot]));
    snprintf(s.items[slot].novelId, sizeof(s.items[slot].novelId), "%s", novelId);
  }

  JjwxcCacheQueue::Item& item = s.items[slot];
  snprintf(item.title, sizeof(item.title), "%s", (title != nullptr && title[0] != '\0') ? title : novelId);
  item.fromOrdinal = fromOrdinal;
  item.toOrdinal = toOrdinal;
  bool anchorValid = item.anchorValid();
  const uint32_t anchor = readingOrdinalForNovel(novelId, anchorValid);
  item.anchorOrdinal = anchor;
  item.setAnchorValid(anchorValid);
  item.totalChapters = toOrdinal - fromOrdinal + 1;
  item.doneChapters = 0;
  item.doneParagraphs = 0;
  item.totalParagraphs = 0;
  item.requests = 0;
  item.retryAtEpoch = 0;
  item.state = static_cast<uint8_t>(JjwxcCacheQueue::State::Queued);
  item.lastError[0] = '\0';
  item.updatedEpoch = epochNow();
  s.userPaused = false;
  s.armed = true;
  JjwxcStore::saveCachePausedPref(false);
  s.retryAtEpoch = 0;
  s.stopRequested.store(false);
  saveQueueLocked();
  bumpBadgeLocked();
  s.revision.fetch_add(1);
  LOG_INF(kTag, "cache service: enqueued %s ordinal %lu..%lu anchor=%lu (%s)", novelId,
          static_cast<unsigned long>(fromOrdinal), static_cast<unsigned long>(toOrdinal),
          static_cast<unsigned long>(anchor), anchorValid ? "valid" : "none");
  xSemaphoreGive(s.wake);
  return EnqueueResult::Ok;
}

void onStandbyEntered() {
  auto& s = service();
  if (!s.started) return;
  {
    Guard guard(s.lock);
    if (!s.readerForeground && !s.foregroundJob) {
      s.armed = true;
      const uint32_t now = epochNow();
      // One try per standby, but not if an attempt just happened: moving in and
      // out of standby should not turn into a reconnect loop.
      if (now - s.lastAttemptEpoch >= 60) s.retryAtEpoch = 0;
    }
  }
  xSemaphoreGive(s.wake);
}

void setReaderForeground(const bool reader) {
  auto& s = service();
  if (!s.started) return;
  {
    Guard guard(s.lock);
    s.readerForeground = reader;
    s.stopRequested.store(reader);
    if (!reader) {
      s.retryAtEpoch = 0;
      if (!s.userPaused) s.armed = true;
    }
  }
  xSemaphoreGive(s.wake);
}

void setForegroundJob(const bool busy) {
  auto& s = service();
  if (!s.started) return;
  {
    Guard guard(s.lock);
    s.foregroundJob = busy;
    if (busy) {
      s.stopRequested.store(true);
    } else if (!s.readerForeground) {
      s.stopRequested.store(false);
      if (!s.userPaused) s.armed = true;
    }
  }
  xSemaphoreGive(s.wake);
}

void acknowledgeBook(const char* novelId) {
  auto& s = service();
  if (!s.started || novelId == nullptr || novelId[0] == '\0') return;
  Guard guard(s.lock);
  s.ackPending = true;
  snprintf(s.ackNovel, sizeof(s.ackNovel), "%s", novelId);
  s.stopRequested.store(true);
  // poll() removes matching Done rows once the worker is idle; shouldRunLocked()
  // also refuses to start a new run while an ack is pending.
}

bool pauseByUser() {
  auto& s = service();
  if (!s.started) return false;
  {
    Guard guard(s.lock);
    if (s.count == 0) return false;
    s.userPaused = true;
    s.stopRequested.store(true);
    JjwxcStore::saveCachePausedPref(true);
    bumpBadgeLocked();
  }
  xSemaphoreGive(s.wake);
  return true;
}

bool resumeByUser() {
  auto& s = service();
  if (!s.started) return false;
  {
    Guard guard(s.lock);
    if (s.count == 0) return false;
    s.userPaused = false;
    s.armed = true;
    JjwxcStore::saveCachePausedPref(false);
    s.retryAtEpoch = 0;
    s.stopRequested.store(false);
    bool queued = false;
    for (size_t i = 0; i < s.count; ++i) {
      const auto st = static_cast<JjwxcCacheQueue::State>(s.items[i].state);
      if (st == JjwxcCacheQueue::State::Paused || st == JjwxcCacheQueue::State::NeedsWifi ||
          st == JjwxcCacheQueue::State::NeedsLogin || st == JjwxcCacheQueue::State::Error) {
        s.items[i].state = static_cast<uint8_t>(JjwxcCacheQueue::State::Queued);
        s.items[i].retryAtEpoch = 0;
        queued = true;
      }
    }
    if (queued) saveQueueLocked();
    bumpBadgeLocked();
  }
  xSemaphoreGive(s.wake);
  return true;
}

bool clearDone() {
  auto& s = service();
  if (!s.started) return false;
  {
    Guard guard(s.lock);
    if (s.workerActive.load()) {
      s.pendingClearDone = true;
      return true;
    }
    size_t out = 0;
    for (size_t i = 0; i < s.count; ++i) {
      if (static_cast<JjwxcCacheQueue::State>(s.items[i].state) == JjwxcCacheQueue::State::Done) continue;
      s.items[out++] = s.items[i];
    }
    if (out == s.count) return false;
    s.count = out;
    saveQueueLocked();
    bumpBadgeLocked();
    s.revision.fetch_add(1);
  }
  return true;
}

bool clearAll() {
  auto& s = service();
  if (!s.started) return false;
  {
    Guard guard(s.lock);
    s.count = 0;
    s.userPaused = false;
    s.pendingEnqueue = false;
    s.pendingClearDone = false;
    s.stopRequested.store(true);
    JjwxcStore::saveCachePausedPref(false);
    saveQueueLocked();
    bumpBadgeLocked();
    s.revision.fetch_add(1);
  }
  xSemaphoreGive(s.wake);
  return true;
}

bool snapshot(JjwxcCacheQueue::Item* out, const size_t maxOut, size_t& count, Status& status) {
  auto& s = service();
  count = 0;
  status = Status{};
  if (!s.started) return false;
  Guard guard(s.lock);
  count = s.count < maxOut ? s.count : maxOut;
  if (out != nullptr && count > 0) memcpy(out, s.items, sizeof(JjwxcCacheQueue::Item) * count);
  status.badge = computeBadgeLocked();
  status.workerActive = s.workerActive.load();
  status.userPaused = s.userPaused;
  status.itemIndex = s.itemIndex.load();
  status.curOrdinal = s.curOrdinal.load();
  status.doneChapters = s.doneChapters.load();
  status.totalChapters = s.totalChapters.load();
  status.doneParagraphs = s.doneParagraphs.load();
  status.totalParagraphs = s.totalParagraphs.load();
  status.requests = s.requests.load();
  status.revision = s.revision.load();
  return true;
}

Badge badge() {
  auto& s = service();
  if (!s.started) return Badge::None;
  Guard guard(s.lock);
  return computeBadgeLocked();
}

uint32_t badgeVersion() { return service().badgeVersion.load(); }

}  // namespace jjwxc_cache_service

#endif  // BOARD_HAS_PSRAM
