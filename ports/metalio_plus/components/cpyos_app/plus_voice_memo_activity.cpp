#include "plus_voice_memo_activity.h"
#include "plus_wav.h"

#include <Arduino.h>
#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_err.h>
#include <plus_audio.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "I18n.h"
#include "components/UITheme.h"

namespace {

/* Card root, next to the other CPYOS content folders. */
constexpr const char* kDir = "/recordings";
constexpr const char* kTag = "MEMO";
/* The BH1098 clocks a fixed 16 kHz link in mode 1; plus_audio_sample_rate()
 * reports it, and this value is only the fallback when the call is unavailable. */
constexpr uint32_t kFallbackRate = 16000;
constexpr uint32_t kMaxSeconds = 60;
/* Capture granularity: 10 ms per read, so Confirm/Back is felt immediately. */
constexpr int kChunkFrames = 160;
constexpr int kPlayChunkSamples = 256;
/* Internal stack: the worker writes the SD card and must never run from PSRAM. */
constexpr uint32_t kWorkerStackBytes = 8 * 1024;
constexpr int kAudioWaitMs = 4000;
constexpr unsigned long kLongPressMs = 900;
constexpr int kGap = 8;
constexpr size_t kWavHeaderBytes = plus_wav::kHeaderBytes;

enum WorkerError : int {
  kErrNone = 0,
  kErrNotReady, /* the BH1098 never reached mode 1 */
  kErrOpen,     /* the file could not be opened for read/write */
  kErrBadWav,   /* not a RIFF/WAVE file with a data chunk */
  kErrWrite,    /* the card refused a write */
};

/* Worker hand-off lives at file scope, not in the activity: the task self-deletes
 * after publishing its result, so a worker that outlives the page must never
 * dereference freed memory. One page instance exists at a time and startJob()
 * refuses to overlap jobs. */
struct WorkerCtx {
  int job;
  char path[64];
  std::atomic<bool> stop{true};
  std::atomic<bool> done{true};
  std::atomic<int> error{kErrNone};
  std::atomic<uint32_t> frames{0};      /* captured / played frames so far */
  std::atomic<uint32_t> rate{kFallbackRate};
  std::atomic<uint32_t> totalFrames{0}; /* playback only: frames in the file */
};

WorkerCtx g_worker{static_cast<int>(PlusVoiceMemoActivity::Job::None), {}};

uint32_t audioRate() {
  const int rate = plus_audio_sample_rate();
  return rate > 0 ? static_cast<uint32_t>(rate) : kFallbackRate;
}

/* Bring the module to mode 1 (the only mode that clocks the local I2S link).
 * Retried once, because a concurrent Wi-Fi scan can starve the I2S DMA pool and
 * leave the handshake in FAILED for this power cycle (plus_audio.h). */
bool ensureMode1() {
  if (!plus_audio_wait_ready(kAudioWaitMs)) {
    LOG_ERR(kTag, "audio not ready (state %d), retrying once", static_cast<int>(plus_audio_state()));
    if (plus_audio_retry() != ESP_OK && plus_audio_state() != PLUS_AUDIO_READY) return false;
    if (!plus_audio_wait_ready(kAudioWaitMs)) {
      LOG_ERR(kTag, "audio still not ready after one retry");
      return false;
    }
  }
  if (plus_audio_bt_mode() != 1) {
    const esp_err_t err = plus_audio_bt_set_mode(1);
    if (err != ESP_OK) {
      LOG_ERR(kTag, "mode 1 restore failed: %s", esp_err_to_name(err));
      return false;
    }
  }
  return true;
}

/* Capture up to kMaxSeconds of mono audio. Runs on the worker task. */
void runRecord(WorkerCtx& c) {
  if (!ensureMode1()) {
    c.error.store(kErrNotReady);
    return;
  }
  /* A cue that was left playing would be picked up by the microphone. */
  plus_audio_pa(false);

  const uint32_t rate = audioRate();
  c.rate.store(rate);

  HalFile file;
  if (!Storage.openFileForWrite(kTag, c.path, file)) {
    LOG_ERR(kTag, "cannot open %s for write", c.path);
    c.error.store(kErrOpen);
    return;
  }

  uint8_t header[kWavHeaderBytes];
  plus_wav::writeHeader(header, 0, rate); /* sizes patched once the data is in */
  if (file.write(header, sizeof(header)) != sizeof(header)) {
    c.error.store(kErrWrite);
    Storage.remove(c.path);
    return;
  }

  int16_t stereo[2 * kChunkFrames];
  int16_t mono[kChunkFrames];
  const uint32_t limit = rate * kMaxSeconds;
  uint32_t frames = 0;
  while (!c.stop.load() && frames < limit) {
    const int got = plus_audio_read_mic(stereo, kChunkFrames);
    if (got < 0) { /* not ready (mode left / link down) */
      c.error.store(kErrNotReady);
      break;
    }
    if (got == 0) continue; /* 250 ms timeout inside the driver; poll stop again */
    /* L/R are the microphone and the board's reference: their mean is the mono
     * memo, the same reduction the vendor demo page uses. */
    for (int i = 0; i < got; ++i) {
      const int32_t sum = static_cast<int32_t>(stereo[2 * i]) + static_cast<int32_t>(stereo[2 * i + 1]);
      mono[i] = static_cast<int16_t>(sum / 2);
    }
    const size_t bytes = static_cast<size_t>(got) * sizeof(int16_t);
    if (file.write(mono, bytes) != bytes) {
      c.error.store(kErrWrite);
      break;
    }
    frames += static_cast<uint32_t>(got);
    c.frames.store(frames);
  }

  const uint32_t dataBytes = frames * sizeof(int16_t);
  if (dataBytes > 0) {
    /* Patch the two size fields so the file is a valid WAV even after a stop. */
    plus_wav::writeHeader(header, dataBytes, rate);
    if (file.seek(0)) file.write(header, sizeof(header));
  }
  /* flush() fsyncs; the local HalFile closes itself on scope exit. */
  file.flush();
  if (dataBytes == 0) {
    /* Nothing captured: leave no 44-byte stub behind. */
    Storage.remove(c.path);
    LOG_ERR(kTag, "no audio captured for %s", c.path);
  } else {
    LOG_INF(kTag, "saved %s (%u bytes, %u frame(s), err %d)", c.path, static_cast<unsigned>(dataBytes),
            static_cast<unsigned>(frames), c.error.load());
  }
}

/* Stream one WAV back through the speaker. Runs on the worker task. */
void runPlay(WorkerCtx& c) {
  if (!ensureMode1()) {
    c.error.store(kErrNotReady);
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead(kTag, c.path, file)) {
    LOG_ERR(kTag, "cannot open %s", c.path);
    c.error.store(kErrOpen);
    return;
  }

  uint8_t riff[12];
  if (file.read(riff, sizeof(riff)) != sizeof(riff) || std::memcmp(riff, "RIFF", 4) != 0 ||
      std::memcmp(riff + 8, "WAVE", 4) != 0) {
    c.error.store(kErrBadWav);
    return;
  }

  /* Walk the chunk list instead of assuming the payload starts at 44: our own
   * files put "data" there, but a WAV from elsewhere may not. */
  uint32_t remaining = 0;
  uint32_t fileRate = kFallbackRate;
  for (int guard = 0; guard < 32; ++guard) {
    uint8_t chunk[8];
    if (file.read(chunk, sizeof(chunk)) != sizeof(chunk)) break;
    const uint32_t size = plus_wav::get32(chunk + 4);
    if (std::memcmp(chunk, "data", 4) == 0) {
      remaining = size;
      break;
    }
    if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 4) {
      const size_t here = file.position();
      uint8_t rateBytes[4];
      if (file.read(rateBytes, sizeof(rateBytes)) == sizeof(rateBytes)) fileRate = plus_wav::get32(rateBytes);
      if (!file.seek(here + size + (size & 1))) break;
      continue;
    }
    if (!file.seek(file.position() + size + (size & 1))) break;
  }
  const size_t payloadStart = file.position();
  if (remaining == 0 || remaining == 0xFFFFFFFFu) {
    const size_t size = file.size();
    remaining = size > payloadStart ? static_cast<uint32_t>(size - payloadStart) : 0;
  }
  if (remaining < sizeof(int16_t)) {
    c.error.store(kErrBadWav);
    return;
  }
  if (fileRate != audioRate()) {
    /* No resampler here; the link is fixed at the rate plus_audio reports. */
    LOG_INF(kTag, "%s is %u Hz, playing at %u Hz", c.path, static_cast<unsigned>(fileRate),
            static_cast<unsigned>(audioRate()));
  }
  c.totalFrames.store(remaining / sizeof(int16_t));

  /* Full-scale playback scaled by the user's speaker volume (audio settings). */
  plus_audio_set_volume(SETTINGS.audioVolume);
  plus_audio_pa(true);

  int16_t chunk[kPlayChunkSamples];
  while (!c.stop.load() && remaining >= sizeof(int16_t)) {
    int want = static_cast<int>(sizeof(chunk));
    if (remaining < static_cast<uint32_t>(want)) want = static_cast<int>(remaining);
    const int got = file.read(chunk, static_cast<size_t>(want));
    if (got <= 0) break;
    const int samples = got / static_cast<int>(sizeof(int16_t));
    if (samples <= 0) break;
    if (plus_audio_write_pcm(chunk, samples) < 0) {
      c.error.store(kErrNotReady);
      break;
    }
    remaining -= static_cast<uint32_t>(samples) * sizeof(int16_t);
    c.frames.fetch_add(static_cast<uint32_t>(samples));
  }
  if (remaining == 0 || c.stop.load()) {
    /* 10 ms of silence drains the I2S DMA so the tail is not clipped. */
    int16_t silence[kChunkFrames] = {0};
    plus_audio_write_pcm(silence, kChunkFrames);
  }
  /* Every exit path above lands here with the amplifier muted. */
  plus_audio_pa(false);
  LOG_INF(kTag, "playback %s: %u/%u frame(s), err %d", c.path, static_cast<unsigned>(c.frames.load()),
          static_cast<unsigned>(c.totalFrames.load()), c.error.load());
}

void workerTrampoline(void* /*arg*/) {
  const auto job = static_cast<PlusVoiceMemoActivity::Job>(g_worker.job);
  if (job == PlusVoiceMemoActivity::Job::Record) {
    runRecord(g_worker);
  } else if (job == PlusVoiceMemoActivity::Job::Play) {
    runPlay(g_worker);
  }
  g_worker.done.store(true);
  vTaskDelete(nullptr);
}

void formatClock(char* buf, const size_t cap, const uint32_t seconds) {
  std::snprintf(buf, cap, "%02u:%02u", static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
}

}  // namespace

/* ------------------------------------------------------------------ life -- */

void PlusVoiceMemoActivity::onEnter() {
  Activity::onEnter();
  phase_ = Phase::List;
  failedStr_ = StrId::STR_NOT_AVAILABLE;
  notice_ = StrId::STR_NONE_OPT;
  selected_ = kActionRow;
  lastTickSecond_ = 0;
  exiting_ = false;
  /* The page can outlive a previous visit with a mode-2 module; restoring mode 1
   * costs ~1.8 s and belongs on the worker, so startRecording() does it. */
  LOG_INF(kTag, "page enter: audio=%d mode=%d", static_cast<int>(plus_audio_state()), plus_audio_bt_mode());
  loadMemos();
  requestUpdate();
}

void PlusVoiceMemoActivity::onExit() {
  exiting_ = true;
  confirm_.dismiss();
  /* Never leave the speaker driven or a half-written job racing the page. */
  stopWorker();
  plus_audio_pa(false);
  LOG_INF(kTag, "page exit");
  Activity::onExit();
}

void PlusVoiceMemoActivity::prepareForSleep() {
  /* A memo is not worth keeping the card awake for, and a parked card cannot be
   * written: stop at the request boundary and leave a valid WAV behind. */
  stopWorker();
  plus_audio_pa(false);
}

bool PlusVoiceMemoActivity::preventAutoSleep() {
  /* Capture and playback are time-based: dozing mid-memo would truncate it. */
  return phase_ == Phase::Recording || phase_ == Phase::Playing;
}

/* ---------------------------------------------------------------- memos --- */

void PlusVoiceMemoActivity::loadMemos() {
  memoCount_ = 0;
  const std::vector<String> names = Storage.listFiles(kDir, kMaxMemos * 2);
  for (const auto& name : names) {
    if (memoCount_ >= kMaxMemos) break;
    const char* raw = name.c_str();
    if (raw == nullptr) continue;
    const size_t len = std::strlen(raw);
    if (len < 9 || len >= sizeof(Memo::name)) continue;
    if (std::strncmp(raw, "memo_", 5) != 0) continue;
    if (std::strcmp(raw + len - 4, ".wav") != 0) continue;

    char path[64];
    std::snprintf(path, sizeof(path), "%s/%s", kDir, raw);
    HalFile file = Storage.open(path, O_RDONLY);
    if (!file) continue;
    const size_t size = file.size();
    if (size <= kWavHeaderBytes) continue; /* a stub that never got audio */

    Memo& memo = memos_[memoCount_++];
    std::snprintf(memo.name, sizeof(memo.name), "%s", raw);
    memo.epoch = static_cast<uint32_t>(std::strtoul(raw + 5, nullptr, 10));
    memo.bytes = static_cast<uint32_t>(size);
    memo.seconds = static_cast<uint32_t>((size - kWavHeaderBytes) / (2 * kFallbackRate));
  }
  /* Newest first: the names carry the capture epoch. */
  std::sort(memos_, memos_ + memoCount_,
            [](const Memo& a, const Memo& b) { return a.epoch > b.epoch; });
  LOG_INF(kTag, "list: %d memo(s) under %s", memoCount_, kDir);
}

void PlusVoiceMemoActivity::startRecording() {
  if (workerBusy()) return;
  notice_ = StrId::STR_NONE_OPT;
  if (!Storage.ensureDirectoryExists(kDir)) {
    LOG_ERR(kTag, "cannot create %s", kDir);
    failedStr_ = StrId::STR_FAILED_LOWER;
    phase_ = Phase::Failed;
    requestUpdate();
    return;
  }

  /* memo_<epoch>.wav. The clock is the name so the list sorts by capture time;
   * +1 bumps keep names unique when the RTC has not been set yet (the epoch then
   * repeats for every capture). */
  const long now = static_cast<long>(time(nullptr));
  char path[64];
  bool unique = false;
  for (int bump = 0; bump < 1000; ++bump) {
    std::snprintf(path, sizeof(path), "%s/memo_%ld.wav", kDir, now + bump);
    if (!Storage.exists(path)) {
      unique = true;
      break;
    }
  }
  if (!unique) {
    LOG_ERR(kTag, "no free name under %s", kDir);
    failedStr_ = StrId::STR_FAILED_LOWER;
    phase_ = Phase::Failed;
    requestUpdate();
    return;
  }

  std::snprintf(g_worker.path, sizeof(g_worker.path), "%s", path);
  phase_ = Phase::Recording;
  lastTickSecond_ = 0;
  LOG_INF(kTag, "recording %s", path);
  requestUpdate();
  startJob(Job::Record);
}

void PlusVoiceMemoActivity::startPlayback(const int index) {
  if (workerBusy()) return;
  if (index < 0 || index >= memoCount_) return;
  notice_ = StrId::STR_NONE_OPT;
  std::snprintf(g_worker.path, sizeof(g_worker.path), "%s/%s", kDir, memos_[index].name);
  phase_ = Phase::Playing;
  lastTickSecond_ = 0;
  LOG_INF(kTag, "playing %s", g_worker.path);
  requestUpdate();
  startJob(Job::Play);
}

/* --------------------------------------------------------------- worker --- */

bool PlusVoiceMemoActivity::workerBusy() const { return worker_ != nullptr || !g_worker.done.load(); }

void PlusVoiceMemoActivity::startJob(const Job job) {
  if (workerBusy()) return;
  job_ = job;
  g_worker.job = static_cast<int>(job);
  g_worker.stop.store(false);
  g_worker.error.store(kErrNone);
  g_worker.frames.store(0);
  g_worker.totalFrames.store(0);
  g_worker.rate.store(kFallbackRate);
  g_worker.done.store(false);

  /* Internal stack (not PSRAM): the worker writes the SD card. The task deletes
   * itself with vTaskDelete(nullptr), so the handle below is only ever dropped,
   * never passed to vTaskDelete() from the UI task. */
  const BaseType_t ok = xTaskCreatePinnedToCore(workerTrampoline, "plusVoiceMemo",
                                                kWorkerStackBytes, nullptr, tskIDLE_PRIORITY,
                                                &worker_, 0);
  if (ok != pdPASS) {
    worker_ = nullptr;
    job_ = Job::None;
    g_worker.done.store(true);
    LOG_ERR(kTag, "worker task creation failed");
    phase_ = Phase::Failed;
    failedStr_ = StrId::STR_FAILED_LOWER;
    requestUpdate();
  }
}

bool PlusVoiceMemoActivity::reapWorker(const uint32_t timeoutMs) {
  const unsigned long deadline = millis() + timeoutMs;
  while (!g_worker.done.load() && static_cast<int32_t>(deadline - millis()) > 0) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return g_worker.done.load();
}

void PlusVoiceMemoActivity::stopWorker() {
  if (worker_ == nullptr && g_worker.done.load()) return;
  g_worker.stop.store(true);
  if (!reapWorker(3000)) {
    /* The worker is bounded (10 ms capture chunks, 250 ms mic timeouts) but a
     * yanked audio link can stretch one call; its context is static, so it is
     * safe for it to finish after this page is gone. */
    LOG_ERR(kTag, "worker still running; leaving it to exit on its own");
    return;
  }
  finishJob();
}

/* Reaps a finished worker and applies its result. The task has already deleted
 * itself, so the handle is only dropped here. */
void PlusVoiceMemoActivity::finishJob() {
  if (worker_ == nullptr) return;
  if (!g_worker.done.load()) return;
  worker_ = nullptr;
  const Job done = job_;
  job_ = Job::None;

  const int err = g_worker.error.load();
  const uint32_t frames = g_worker.frames.load();
  const uint32_t rate = g_worker.rate.load() > 0 ? g_worker.rate.load() : kFallbackRate;

  if (done == Job::Record) {
    loadMemos();
    if (err == kErrNone && frames > 0) {
      phase_ = Phase::List;
      selected_ = memoCount_ > 0 ? 1 : kActionRow; /* highlight the new memo */
    } else {
      phase_ = Phase::Failed;
      failedStr_ = err == kErrNotReady ? StrId::STR_NOT_AVAILABLE : StrId::STR_FAILED_LOWER;
      LOG_ERR(kTag, "recording failed (err %d, %u frame(s))", err, static_cast<unsigned>(frames));
    }
  } else if (done == Job::Play) {
    phase_ = Phase::List;
    if (err != kErrNone) {
      notice_ = err == kErrNotReady ? StrId::STR_NOT_AVAILABLE : StrId::STR_FAILED_LOWER;
      LOG_ERR(kTag, "playback failed (err %d)", err);
    }
  }

  LOG_INF(kTag, "job %d done: %u frame(s) at %u Hz", static_cast<int>(done), static_cast<unsigned>(frames),
          static_cast<unsigned>(rate));
  if (!exiting_) requestUpdate();
}

/* -------------------------------------------------------------- actions --- */

int PlusVoiceMemoActivity::rowCount() const {
  if (phase_ == Phase::Failed) return 1;
  return 1 + memoCount_;
}

int PlusVoiceMemoActivity::rowHeight() const { return GUI.getListRowStep(true); }

int PlusVoiceMemoActivity::listTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + kGap;
}

int PlusVoiceMemoActivity::listHeight() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return std::max(0, renderer.getScreenHeight() - listTop() - metrics.buttonHintsHeight - kGap);
}

void PlusVoiceMemoActivity::moveSelection(const int delta) {
  const int last = rowCount() - 1;
  int next = selected_ + delta;
  if (next < 0) next = 0;
  if (next > last) next = last;
  if (next == selected_) return;
  selected_ = next;
  requestUpdate();
}

void PlusVoiceMemoActivity::activateRow(const int row) {
  if (phase_ == Phase::Failed) { /* Confirm retries the handshake */
    startRecording();
    return;
  }
  if (row == kActionRow) {
    startRecording();
  } else {
    startPlayback(row - 1);
  }
}

void PlusVoiceMemoActivity::longPressRow(const int row) {
  /* Only files can be deleted: the "Add" row has nothing behind it, and a
   * recording in progress owns the file it is writing. */
  if (phase_ != Phase::List || row == kActionRow || row > memoCount_) return;
  const Memo& memo = memos_[row - 1];
  char path[64];
  std::snprintf(path, sizeof(path), "%s/%s", kDir, memo.name);
  const std::string name = memo.name;
  const std::string where = path;
  confirm_.ask(tr(STR_DELETE), name.c_str(), tr(STR_DELETE), [this, where] {
    if (Storage.remove(where.c_str())) {
      LOG_INF(kTag, "deleted %s", where.c_str());
      loadMemos();
      if (selected_ > rowCount() - 1) selected_ = rowCount() - 1;
    } else {
      LOG_ERR(kTag, "cannot delete %s", where.c_str());
      notice_ = StrId::STR_FAILED_LOWER;
    }
    requestUpdate();
  });
}

std::string PlusVoiceMemoActivity::rowTitleOf(const int row) const {
  if (phase_ == Phase::Failed || row == kActionRow) return tr(STR_ADD);
  const int index = row - 1;
  if (index < 0 || index >= memoCount_) return "";
  return memos_[index].name;
}

std::string PlusVoiceMemoActivity::rowSubtitleOf(const int row) const {
  if (phase_ == Phase::Failed || row == kActionRow) return "";
  const int index = row - 1;
  if (index < 0 || index >= memoCount_) return "";
  const Memo& memo = memos_[index];
  char buf[48];
  char clock[16];
  formatClock(clock, sizeof(clock), memo.seconds);
  std::snprintf(buf, sizeof(buf), "%u KB · %s", static_cast<unsigned>(memo.bytes / 1024), clock);
  return buf;
}

/* ------------------------------------------------------------------ loop -- */

void PlusVoiceMemoActivity::loop() {
  /* The inline confirm owns every key while it is up. */
  if (confirm_.active()) {
    confirm_.handleInput(mappedInput, [this] { requestUpdate(); });
    return;
  }

  if (worker_ != nullptr && g_worker.done.load()) finishJob();

  if (phase_ == Phase::Recording) {
    const uint32_t rate = g_worker.rate.load() > 0 ? g_worker.rate.load() : kFallbackRate;
    const uint32_t second = g_worker.frames.load() / rate;
    if (second != lastTickSecond_) {
      lastTickSecond_ = second;
      requestUpdate();
    }
    bool stop = mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
                mappedInput.wasReleased(MappedInputManager::Button::Back);
#if FREEINK_CAP_TOUCH
    int tapX = 0;
    int tapY = 0;
    if (!stop && mappedInput.wasScreenTapped(tapX, tapY)) {
      stop = true; /* stopping is the only action this page has while recording */
    }
#endif
    if (stop) {
      /* Save and return to the list; leaving the page is the second press. */
      stopWorker();
      requestUpdate();
    }
    return;
  }

  if (phase_ == Phase::Playing) {
    const uint32_t rate = g_worker.rate.load() > 0 ? g_worker.rate.load() : kFallbackRate;
    const uint32_t second = g_worker.frames.load() / rate;
    if (second != lastTickSecond_) {
      lastTickSecond_ = second;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      /* Back stops playback and leaves the page. */
      stopWorker();
      activityManager.goToApps();
      return;
    }
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasScreenTapped(tapX, tapY)) {
      stopWorker();
      requestUpdate();
      return;
    }
    return;
  }

  /* List / Failed. */
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }
  /* Long press first: it consumes the Confirm release that would otherwise be a
   * plain "play this file" (MappedInputManager::wasLongPressed suppresses it). */
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, kLongPressMs)) {
    longPressRow(selected_);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) moveSelection(-1);
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) moveSelection(1);
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow(selected_);
    return;
  }

  const int step = rowHeight();
  const auto touch = handleListTouch(selected_, rowCount(), listTop(), listHeight(), true);
  if (touch == ListTouchResult::Activated) {
    activateRow(selected_);
    return;
  }
#if FREEINK_CAP_TOUCH
  int lx = 0;
  int ly = 0;
  if (step > 0 && mappedInput.wasScreenLongPress(lx, ly) && ly >= listTop() &&
      ly < listTop() + rowCount() * step) {
    longPressRow((ly - listTop()) / step);
  }
#else
  (void)step;
#endif
}

/* ---------------------------------------------------------------- render -- */

void PlusVoiceMemoActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_VOICE_MEMO));

  char status[64];
  const char* subtitle = "";
  if (phase_ == Phase::Recording) {
    const uint32_t rate = g_worker.rate.load() > 0 ? g_worker.rate.load() : kFallbackRate;
    const uint32_t seconds = g_worker.frames.load() / rate;
    char now[16];
    char limit[16];
    formatClock(now, sizeof(now), seconds > kMaxSeconds ? kMaxSeconds : seconds);
    formatClock(limit, sizeof(limit), kMaxSeconds);
    std::snprintf(status, sizeof(status), "%s / %s", now, limit);
    subtitle = status;
  } else if (phase_ == Phase::Playing) {
    const uint32_t rate = g_worker.rate.load() > 0 ? g_worker.rate.load() : kFallbackRate;
    char now[16];
    char total[16];
    formatClock(now, sizeof(now), g_worker.frames.load() / rate);
    formatClock(total, sizeof(total), g_worker.totalFrames.load() / rate);
    std::snprintf(status, sizeof(status), "%s / %s", now, total);
    subtitle = status;
  } else if (phase_ == Phase::Failed) {
    subtitle = I18N.get(failedStr_);
  } else if (notice_ != StrId::STR_NONE_OPT) {
    subtitle = I18N.get(notice_);
  } else {
    uint64_t bytes = 0;
    for (int i = 0; i < memoCount_; ++i) bytes += memos_[i].bytes;
    std::snprintf(status, sizeof(status), "%d · %.1f MB", memoCount_, static_cast<double>(bytes) / 1e6);
    subtitle = status;
  }
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, width, metrics.tabBarHeight},
                    subtitle, nullptr);

  const Rect list{0, listTop(), width, listHeight()};
  auto rowTitle = [this](const int i) { return rowTitleOf(i); };
  auto rowSubtitle = [this](const int i) { return rowSubtitleOf(i); };
  /* Failed shows a single retry row, so a selection left over from the list must
   * not point past it. */
  const int selected = phase_ == Phase::Failed ? kActionRow : std::min(selected_, rowCount() - 1);
  GUI.drawList(renderer, list, rowCount(), selected, rowTitle, rowSubtitle);

  if (phase_ == Phase::List && memoCount_ == 0) {
    /* Below the single "Add" row: the page is usable, it just has no files. */
    const int step = rowHeight();
    const Rect empty{0, list.y + step, width, std::max(0, list.height - step)};
    if (empty.height > 0) GUI.drawEmptyState(renderer, empty, tr(STR_NO_FILES_FOUND));
  }

  const char* confirmLabel = tr(STR_SELECT);
  if (phase_ == Phase::Recording) confirmLabel = tr(STR_DONE);
  if (phase_ == Phase::Failed) confirmLabel = tr(STR_CONFIRM);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  /* The confirm floats over the list, in the same frame (one panel push). */
  if (confirm_.active()) confirm_.paint(renderer);
  renderer.displayBuffer();
}

/* ----------------------------------------------------------------- debug -- */

void PlusVoiceMemoActivity::debugConfirm() { activateRow(selected_); }
void PlusVoiceMemoActivity::debugCancel() { activityManager.goToApps(); }
