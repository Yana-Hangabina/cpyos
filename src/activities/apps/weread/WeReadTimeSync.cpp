#include "WeReadTimeSync.h"

#if defined(ESP_PLATFORM)

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

#include "BackgroundWork.h"
#include "util/TimeUtils.h"
#include "WeReadStore.h"

namespace weread_time_sync {
namespace {

constexpr const char* kTag = "WRTS";
constexpr uint32_t kTaskStackWords = 12 * 1024;  // counts words: 48 KB in PSRAM
#if defined(BOARD_HAS_PSRAM)
constexpr uint32_t kTaskCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
constexpr uint32_t kTaskCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
constexpr uint32_t kUnverifiedBackoffMs = 10 * 60 * 1000;

struct Request {
  char bookId[64] = {};
  WeReadClient::ProgressSyncInput input;
  bool valid = false;
};

struct State {
  bool loaded = false;
  WeReadStore::TimeSyncPrefs prefs;
  SemaphoreHandle_t lock = nullptr;
  TaskHandle_t task = nullptr;
  bool running = false;
  bool foregroundBusy = false;
  uint32_t suppressUntilMs = 0;
  Request request;
  bool workerDone = false;
};

State& state() {
  static State s;
  return s;
}

SemaphoreHandle_t lockFor(State& s) {
  if (!s.lock) s.lock = xSemaphoreCreateMutex();
  return s.lock;
}

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t handle) : handle_(handle) {
    if (handle_) xSemaphoreTake(handle_, portMAX_DELAY);
  }
  ~Lock() {
    if (handle_) xSemaphoreGive(handle_);
  }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

 private:
  SemaphoreHandle_t handle_;
};

void loadLocked(State& s) {
  if (s.loaded) return;
  if (!WeReadStore::loadTimeSyncPrefs(s.prefs)) s.prefs = WeReadStore::TimeSyncPrefs{};
  s.loaded = true;
}

void runUpload(const Request& request, State& s);

void taskMain(void*) {
  auto& s = state();
  for (;;) {
    Request request;
    {
      Lock lock(lockFor(s));
      if (!s.request.valid) {
        s.running = false;
        s.task = nullptr;
        s.workerDone = true;
        break;
      }
      request = s.request;
      s.request.valid = false;
    }
    runUpload(request, s);
  }
  // PSRAM stacks must be released with the matching deleter; nothing else
  // references this handle, so a self-delete is safe and leaks nothing.
  vTaskDeleteWithCaps(nullptr);
}

void runUpload(const Request& request, State& s) {
  background_work::Guard workGuard;
  LOG_INF(kTag, "upload start: book=%s seconds=%u", request.bookId,
          static_cast<unsigned>(request.input.readingSeconds));

  // The operation is ~8 KB; it lives on this task's (PSRAM) stack.
  WeReadClient::Operation operation;
  if (!operation.beginTimeSync(request.bookId, request.input)) {
    const auto error = operation.error();
    operation.reset();
    LOG_INF(kTag, "upload rejected: error=%u", static_cast<unsigned>(error));
    return;
  }

  int guard = 0;
  while (operation.active() && guard++ < 600) {
    const auto event = operation.step();
    if (event == WeReadClient::Operation::Event::Complete ||
        event == WeReadClient::Operation::Event::Failed ||
        event == WeReadClient::Operation::Event::Cancelled) {
      break;
    }
    if (event == WeReadClient::Operation::Event::None) vTaskDelay(pdMS_TO_TICKS(100));
  }

  const auto error = operation.error();
  const auto result = operation.progressSyncResult();
  operation.reset();

  const uint32_t submitted = result.submittedReadingSeconds;
  if (error == WeReadClient::Error::Ok && result.appliedReadingSeconds > 0) {
    settle(request.bookId, result.appliedReadingSeconds, submitted, true);
    const uint32_t epoch = TimeUtils::getCurrentValidTimestamp();
    if (epoch != 0) {
      Lock lock(lockFor(s));
      loadLocked(s);
      s.prefs.lastUploadSeconds = result.appliedReadingSeconds;
      s.prefs.lastUploadEpoch = epoch;
      WeReadStore::saveTimeSyncPrefs(s.prefs);
    }
    LOG_INF(kTag, "upload ok: applied=%u before=%u after=%u", static_cast<unsigned>(result.appliedReadingSeconds),
            static_cast<unsigned>(result.cloudReadingSecondsBefore),
            static_cast<unsigned>(result.cloudReadingSecondsAfter));
    return;
  }

  if (error == WeReadClient::Error::Ok) {
    // The server accepted the report but its counter did not move. Keep the
    // seconds, but do not retry every 30 s: back off so a server that ignores
    // `rt` cannot turn the reader into a request loop.
    {
      Lock lock(lockFor(s));
      s.suppressUntilMs = millis() + kUnverifiedBackoffMs;
    }
    LOG_INF(kTag, "upload unverified: applied=0 before=%u(%u) after=%u(%u); keeping pending",
            static_cast<unsigned>(result.cloudReadingSecondsBefore),
            static_cast<unsigned>(result.cloudReadingBeforeValid),
            static_cast<unsigned>(result.cloudReadingSecondsAfter),
            static_cast<unsigned>(result.cloudReadingAfterValid));
    return;
  }

  // A report that reached the server but could not be verified may still have
  // been applied. Following the WeRead policy, it is never replayed: drop it
  // instead of double counting on a later upload.
  if (result.reportSent) {
    settle(request.bookId, 0, submitted, true);
    LOG_INF(kTag, "upload ambiguous: error=%u; dropping %u second(s) to avoid double counting",
            static_cast<unsigned>(error), static_cast<unsigned>(submitted));
    return;
  }
  LOG_INF(kTag, "upload failed before report: error=%u; keeping pending", static_cast<unsigned>(error));
}

}  // namespace

void begin() {
  auto& s = state();
  Lock lock(lockFor(s));
  loadLocked(s);
}

bool enabled() {
  auto& s = state();
  Lock lock(lockFor(s));
  loadLocked(s);
  return s.prefs.enabled != 0;
}

bool consentGiven() {
  auto& s = state();
  Lock lock(lockFor(s));
  loadLocked(s);
  return s.prefs.consent != 0;
}

bool setEnabled(const bool on) {
  auto& s = state();
  Lock lock(lockFor(s));
  loadLocked(s);
  s.prefs.enabled = on ? 1 : 0;
  return WeReadStore::saveTimeSyncPrefs(s.prefs);
}

bool setConsent() {
  auto& s = state();
  Lock lock(lockFor(s));
  loadLocked(s);
  s.prefs.consent = 1;
  s.prefs.enabled = 1;
  return WeReadStore::saveTimeSyncPrefs(s.prefs);
}

uint32_t lastUploadSeconds() {
  auto& s = state();
  Lock lock(lockFor(s));
  loadLocked(s);
  return s.prefs.lastUploadSeconds;
}

uint32_t pendingSeconds(const char* bookId) {
  WeReadStore::TimeSyncState st;
  return WeReadStore::loadTimeSyncState(bookId, st) ? st.pendingSeconds : 0;
}

bool addPending(const char* bookId, const uint32_t seconds) {
  if (!bookId || !bookId[0] || seconds == 0) return true;
  auto& s = state();
  Lock lock(lockFor(s));
  WeReadStore::TimeSyncState st;
  WeReadStore::loadTimeSyncState(bookId, st);
  if (st.magic != WeReadStore::kTimeSyncStateMagic) st = WeReadStore::TimeSyncState{};
  const uint32_t before = st.pendingSeconds;
  st.pendingSeconds = before > UINT32_MAX - seconds ? UINT32_MAX : before + seconds;
  const bool ok = WeReadStore::saveTimeSyncState(bookId, st);
  if (!ok) LOG_ERR(kTag, "failed to persist pending seconds for %s", bookId);
  return ok;
}

bool settle(const char* bookId, const uint32_t appliedSeconds, const uint32_t submittedSeconds,
            const bool reportSent) {
  if (!bookId || !bookId[0]) return false;
  uint32_t remove = appliedSeconds;
  if (remove == 0 && reportSent) remove = submittedSeconds;
  if (remove == 0) return true;
  auto& s = state();
  Lock lock(lockFor(s));
  WeReadStore::TimeSyncState st;
  if (!WeReadStore::loadTimeSyncState(bookId, st)) st = WeReadStore::TimeSyncState{};
  const uint32_t removed = std::min(remove, st.pendingSeconds);
  st.pendingSeconds -= removed;
  st.uploadedSeconds = st.uploadedSeconds > UINT32_MAX - removed ? UINT32_MAX : st.uploadedSeconds + removed;
  const uint32_t epoch = TimeUtils::getCurrentValidTimestamp();
  if (epoch != 0) st.lastUploadEpoch = epoch;
  const bool ok = WeReadStore::saveTimeSyncState(bookId, st);
  if (!ok) LOG_ERR(kTag, "failed to settle seconds for %s", bookId);
  return ok;
}

bool busy() {
  auto& s = state();
  Lock lock(lockFor(s));
  return s.running;
}

void setForegroundBusy(const bool busy) {
  auto& s = state();
  Lock lock(lockFor(s));
  s.foregroundBusy = busy;
}

bool kick(const char* bookId, const WeReadClient::ProgressSyncInput& input) {
#if !defined(BOARD_HAS_PSRAM)
  (void)bookId;
  (void)input;
  return false;  // no PSRAM: never spawn the background TLS worker
#else
  if (!bookId || !bookId[0] || input.readingSeconds == 0) return false;
  auto& s = state();
  {
    Lock lock(lockFor(s));
    loadLocked(s);
    if (s.prefs.enabled == 0 || s.prefs.consent == 0) return false;
    if (s.foregroundBusy) return false;
    if (s.suppressUntilMs != 0 && static_cast<int32_t>(millis() - s.suppressUntilMs) < 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    Request request;
    strncpy(request.bookId, bookId, sizeof(request.bookId) - 1);
    request.input = input;
    request.valid = true;
    s.request = request;
    s.workerDone = false;
    if (s.running) return true;  // the running worker picks up the latest request
    s.running = true;
  }
  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(taskMain, "wrTimeSync", kTaskStackWords, nullptr,
                                                  tskIDLE_PRIORITY, &state().task, 0, kTaskCaps);
  if (ok != pdPASS) {
    LOG_ERR(kTag, "task creation refused (%d)", static_cast<int>(ok));
    auto& st = state();
    Lock lock(lockFor(st));
    st.running = false;
    st.task = nullptr;
    st.request.valid = false;
    return false;
  }
  return true;
#endif
}

}  // namespace weread_time_sync

#else  // !ESP_PLATFORM: desktop/simulator builds have no worker task.

namespace weread_time_sync {
void begin() {}
bool enabled() { return false; }
bool consentGiven() { return false; }
bool setEnabled(bool) { return false; }
bool setConsent() { return false; }
uint32_t lastUploadSeconds() { return 0; }
uint32_t pendingSeconds(const char*) { return 0; }
bool addPending(const char*, uint32_t) { return true; }
bool settle(const char*, uint32_t, uint32_t, bool) { return true; }
bool kick(const char*, const WeReadClient::ProgressSyncInput&) { return false; }
bool busy() { return false; }
void setForegroundBusy(bool) {}
}  // namespace weread_time_sync

#endif  // ESP_PLATFORM
