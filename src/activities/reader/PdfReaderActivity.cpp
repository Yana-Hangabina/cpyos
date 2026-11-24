#if defined(CROSSPOINT_MUPDF)

#include "PdfReaderActivity.h"

#include "PdfFailCardLayout.h"
#include "PdfFailStreak.h"
#include "PdfReaderUi.h"
#include "PdfJobSlot.h"
#include "PdfJobWait.h"
#include "ReflowScale.h"  // the width-scale rule, host-tested in test/reflow_scale

#include "AnnotationAnchor.h"
#include "QuoteAnchor.h"
#include "AnnotationExport.h"
#if defined(CROSSPOINT_PDF_XIP)
#include "activities/settings/PdfEngineUpdateActivity.h"
#endif

#include <BookCachePrune.h>
#include <Utf8.h>
#include <Bitmap.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#if defined(CROSSPOINT_PDF_XIP)
#include "NetworkStartup.h"
#include "components/StatusCards.h"
#include <WiFi.h>
#include "pdf/PdfModuleRuntime.h"
#endif
#include <Memory.h>
#include <PinyinDb.h>

#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_system.h>  // esp_restart(), the escape hatch of a wedged worker
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "CrossPointSettings.h"
#include "Epub/FirstLineIndent.h"
#include "Epub/hyphenation/HyphenationStore.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "EpubReaderMenuActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "EmojiFont.h"
#include "SdCardFontSystem.h"
#include "SystemFontPack.h"
#include "Typography.h"
#include "fontIds.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "UiFontRefresh.h"
#include "ExternalFontPackStorage.h"
#include "ReadingStatsStore.h"
#include "ReaderActivity.h"
#include "activities/ActivityResult.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "Typography.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/ScreenshotUtil.h"
#include "util/SelectionText.h"
#include "util/DictionaryRegistry.h"
#include "DictionaryWordSelectActivity.h"
#include "pdf/PdfLineMerge.h"

namespace {
constexpr uint32_t kCacheMagic = 0x50444643u;  // "PDFC"
constexpr uint16_t kCacheVersion = 3;

struct CacheMeta {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t fileSize;
  uint32_t mtime;
};

constexpr char kFullCacheMagic[4] = {'P', 'D', 'F', 'F'};
constexpr uint16_t kFullCacheVersion = 1;

struct FullCacheMeta {
  char magic[4];
  uint16_t version;
  uint16_t reserved;
  uint32_t pageCount;
};

// MuPDF's display-list construction and deep page resources do not fit in the
// 8 KB Arduino loopTask stack.  WhiteOS runs PDF work in a dedicated worker;
// this is the same shape, with the stack in PSRAM so internal RAM is untouched.
struct PdfRenderJob {
  enum class Kind { Open, Render, ExtractText, HasText, ReflowProbe };
  PdfDocument* doc = nullptr;
  Kind kind = Kind::Render;
  int page = 0;
  std::string path;
  int width = 0;
  int height = 0;
  bool cover = false;
  // Text jobs write into the caller-owned containers while it waits below.
  std::vector<std::string>* paragraphs = nullptr;
  bool* hasImages = nullptr;
  // Reflow probe results, published together with done.
  int probeUsable = 0;
  int probeCounted = 0;
  std::atomic<bool> done{false};
  volatile bool ok = false;

  // The slot is reused across jobs, and `done` is the handshake: a job left
  // marked done would let the next waiter return before the worker started.
  // There is no assignment operator to lean on, because of that atomic.
  void reset(PdfDocument& document, const Kind jobKind, const int pageNumber) {
    doc = &document;
    kind = jobKind;
    page = pageNumber;
    path.clear();
    width = height = 0;
    cover = false;
    paragraphs = nullptr;
    hasImages = nullptr;
    probeUsable = probeCounted = 0;
    ok = false;
    done.store(false);
  }
};

TaskHandle_t gPdfWorkerTask = nullptr;
QueueHandle_t gPdfWorkerQueue = nullptr;
// The reflow probe is posted without blocking the UI, so its job object must
// outlive the call.  There is only ever one PDF reader activity.
PdfRenderJob gReflowProbeJob;
// Synchronous jobs live here, not on the caller's stack.  A wait that gives up
// must never leave the worker holding a pointer into a dead frame -- that is a
// heap-corruption fix waiting to happen, and the text kinds also write into
// caller-owned containers.  One PDF reader exists at a time and the UI task
// enqueues-then-waits, so a single slot is the whole requirement.
PdfRenderJob gSyncJob;
std::atomic<bool> gSyncJobInFlight{false};

void pdfRenderWorker(void* arg) {
  // The queue comes from the task argument, never from the global.  A job that
  // outlived its wait has its task LEAKED on purpose (stopPdfWorker), and that
  // task returns to this loop after finishing inside MuPDF.  Reading the global
  // would then point it at the NEXT session's queue: two workers running MuPDF,
  // possibly on the same document, which is the corruption the leak avoids.
  auto* queue = static_cast<QueueHandle_t>(arg);
  for (;;) {
    PdfRenderJob* job = nullptr;
    if (xQueueReceive(queue, &job, portMAX_DELAY) != pdTRUE) continue;
    if (!job) continue;
    switch (job->kind) {
      case PdfRenderJob::Kind::Open:
        // Encrypted PDFs run AES/xref setup in MuPDF; the 8 KB UI task stack
        // is not enough for that path.  Opening on the PSRAM worker also keeps
        // every MuPDF call on one thread.
        job->ok = job->doc->load();
        break;
      case PdfRenderJob::Kind::Render:
        job->ok = job->doc->renderPageToBmp(job->page, job->path, job->width, job->height, job->cover);
        break;
      case PdfRenderJob::Kind::ExtractText:
        // Stext runs the font engine and its frame depth does not fit the 8 KB
        // loopTask stack; the PSRAM worker is already the PDF compute thread.
        job->ok = job->paragraphs && job->hasImages &&
                  job->doc->extractText(job->page, *job->paragraphs, *job->hasImages);
        break;
      case PdfRenderJob::Kind::HasText:
        job->ok = job->doc->pageHasText(job->page);
        break;
      case PdfRenderJob::Kind::ReflowProbe: {
        // Same heuristic as before, but off the UI thread: opening a book no
        // longer freezes for the 5-20 s three pages of stext cost.
        const int total = job->doc->pageCount();
        const int samples[3] = {std::min(1, total - 1), total / 2, std::max(0, total - 2)};
        job->probeUsable = 0;
        job->probeCounted = 0;
        for (const int page : samples) {
          std::vector<std::string> paras;
          bool images = false;
          if (!job->doc->extractText(page, paras, images)) continue;
          ++job->probeCounted;
          size_t chars = 0;
          for (const auto& para : paras) chars += para.size();
          if (!images && chars >= 400) ++job->probeUsable;
        }
        job->ok = true;
        break;
      }
    }
    job->done.store(true);
  }
}

// Returns false when the worker never came back.  The caller must not resume
// using its own containers in that case -- see gSyncJob above for why the
// escape hatch is a restart and not a return.
// Called from the wait loop (render task) when a job has been running long
// enough to be worth saying so.  The reader installs it around a page render so
// the duck-wave card can add elapsed seconds instead of sitting motionless for
// 30 s, which reads as a dead device.
void (*gPdfWaitTick)(void* ctx, uint32_t elapsedMs) = nullptr;
void* gPdfWaitTickCtx = nullptr;

bool waitPdfJob(const PdfRenderJob& job, const uint32_t timeoutMs = pdf_job_wait::kDefaultTimeoutMs) {
  const uint32_t startedAt = millis();
  unsigned announced = 0;
  for (;;) {
    if (job.done.load()) return true;
    const uint32_t now = millis();
    const pdf_job_wait::Progress progress = pdf_job_wait::decide(now, startedAt, timeoutMs, announced);
    if (progress.announce) {
      ++announced;
      const uint32_t freeInternal =
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const uint32_t largestInternal =
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      LOG_ERR("PDF", "pdf worker waiting: kind=%d page=%d at %ums free=%u largest=%u",
              static_cast<int>(job.kind), job.page, static_cast<unsigned>(progress.elapsed),
              static_cast<unsigned>(freeInternal), static_cast<unsigned>(largestInternal));
    }
    if (progress.announce && gPdfWaitTick != nullptr) gPdfWaitTick(gPdfWaitTickCtx, progress.elapsed);
    if (progress.timedOut) {
      // Return, do not restart.  This is only safe because the job lives in a
      // static slot and the slot stays owned until the worker finishes with it
      // (syncSlotFree()): the late completion writes into storage that still
      // exists, and no new job can overwrite it underneath the worker.  Text
      // kinds write into caller containers, so the caller must not touch those
      // until the slot is released -- which is why the reader marks the page
      // failed instead of trying to use partial output.
      //
      // The alternative (restart) turned a slow page into a reboot: for a user
      // that is a worse outcome than "this page cannot be shown, turn past it",
      // and the card that appears now offers exactly that.
      LOG_ERR("PDF", "pdf worker slow: kind=%d page=%d exceeded %ums; page marked failed",
              static_cast<int>(job.kind), job.page, static_cast<unsigned>(progress.elapsed));
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

bool stopPdfWorker();  // false = a job outlived its wait and the task was leaked

bool startPdfWorker() {
  if (gPdfWorkerTask) return true;
  gPdfWorkerQueue = xQueueCreate(4, sizeof(PdfRenderJob*));
  if (!gPdfWorkerQueue) {
    stopPdfWorker();
    return false;
  }
  // Run at idle priority: MuPDF's encrypted AES/xref path and long page
  // renders are CPU-bound and never yield.  At priority 1 they starved IDLE0
  // on core 0 and tripped TASK_WDT (reproduced by opening an encrypted PDF).
  const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      pdfRenderWorker, "PdfRender", 65536 / sizeof(StackType_t), gPdfWorkerQueue, tskIDLE_PRIORITY,
      &gPdfWorkerTask, 0,
      MALLOC_CAP_SPIRAM);
  if (created != pdPASS) {
    LOG_ERR("PDF", "persistent PDF worker create failed");
    gPdfWorkerTask = nullptr;
    stopPdfWorker();
    return false;
  }
  LOG_ERR("PDF", "persistent PDF worker ready");
  return true;
}

bool stopPdfWorker() {
  if (gPdfWorkerTask) {
    // A timed-out job may still be inside MuPDF when the user leaves the reader,
    // and deleting the task under it is the heap corruption this project has
    // paid for twice.  Wait bounded; if it is still running, LEAK the task and
    // its queue (both become inert: the task blocks on a queue nobody sends to)
    // and let the next session start a fresh pair.  A leaked 128 KB PSRAM stack
    // until reboot is the cheap side of this trade.
    const auto state = [&] { return pdf_job_slot::State{gSyncJobInFlight.load(), gSyncJob.done.load()}; };
    // Short bound on purpose: a job that has not finished by now is the one that
    // already blew the 75 s render budget, and making the user stare at a frozen
    // reader for another minute buys nothing.  Leaking the task is the safe side.
    const uint32_t started = millis();
    while (!pdf_job_slot::mayStopWorker(state()) && millis() - started < 5000) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!pdf_job_slot::mayStopWorker(state())) {
      LOG_ERR("PDF", "worker not stopped: a synchronous job is still running after %ums; "
                     "leaking the task and its queue instead of deleting them mid-page",
              static_cast<unsigned>(millis() - started));
      gPdfWorkerTask = nullptr;
      gPdfWorkerQueue = nullptr;
      return false;
    }
    vTaskDeleteWithCaps(gPdfWorkerTask);
    gPdfWorkerTask = nullptr;
  }
  if (gPdfWorkerQueue) {
    vQueueDelete(gPdfWorkerQueue);
    gPdfWorkerQueue = nullptr;
  }
  return true;
}


// True when the slot may be reused.  A timed-out wait leaves the flag set on
// purpose: the worker may still be inside MuPDF with a pointer into gSyncJob.
// Ownership returns when the job's own `done` is observed, not when the waiter
// gives up -- that distinction is the whole safety argument.
bool syncSlotFree() {
  // The decision itself is host-tested in test/pdf_job_slot: recycling on a
  // timeout corrupts the heap, never recycling refuses every later render.
  const pdf_job_slot::State state{gSyncJobInFlight.load(), gSyncJob.done.load()};
  if (pdf_job_slot::submit(state) == pdf_job_slot::Verdict::Refuse) return false;
  if (state.inFlight && state.done) gSyncJobInFlight.store(false);  // finished: recycle
  return true;
}

// A page turn right after a 75 s timeout used to be refused outright: the
// previous job still owns the slot, renderPdfPage returned false, and the user
// saw "this page cannot be shown" for a page that never got a chance.  Ownership
// returns when the worker's own `done` is observed (never by the waiter giving
// up), so the honest thing is to wait for it under the same wait card.
bool awaitSyncSlot(const uint32_t timeoutMs = pdf_job_wait::kDefaultTimeoutMs) {
  const uint32_t startedAt = millis();
  unsigned announced = 0;
  for (;;) {
    if (syncSlotFree()) return true;
    const pdf_job_wait::Progress progress =
        pdf_job_wait::decide(millis(), startedAt, timeoutMs, announced);
    if (progress.announce) {
      ++announced;
      LOG_ERR("PDF", "waiting %ums for the previous job to release the sync slot",
              static_cast<unsigned>(progress.elapsed));
      if (gPdfWaitTick) gPdfWaitTick(gPdfWaitTickCtx, progress.elapsed);
    }
    if (progress.timedOut) {
      LOG_ERR("PDF", "previous job did not release the sync slot within %ums",
              static_cast<unsigned>(progress.elapsed));
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// The ONLY way to release the slot after a wait.  A timed-out wait must leave
// the flag set: the worker still owns gSyncJob (it may be inside MuPDF with a
// pointer into it), and reusing the slot under it is the heap corruption this
// rule exists to prevent (test/pdf_job_slot).  renderPdfPage used to clear it
// unconditionally, which reopened exactly that window after every timeout.
void releaseSlotAfterWait(const bool completed) {
  if (pdf_job_slot::slotReturnedByWait(completed)) gSyncJobInFlight.store(false);
}

bool enqueuePdfJob(PdfRenderJob& job) {
  if (!gPdfWorkerQueue) {
    LOG_ERR("PDF", "worker queue missing; job dropped (kind=%d page=%d)",
            static_cast<int>(job.kind), job.page);
    return false;
  }
  PdfRenderJob* ptr = &job;
  if (xQueueSend(gPdfWorkerQueue, &ptr, 0) != pdTRUE) {
    // Silent drops are how a page turn looks like "nothing happened": the UI
    // task goes on to wait on a job the worker was never told about.
    LOG_ERR("PDF", "worker queue full; job dropped (kind=%d page=%d)",
            static_cast<int>(job.kind), job.page);
    return false;
  }
  return true;
}

bool loadPdfDocument(PdfDocument& doc) {
  if (!gPdfWorkerTask) return doc.load();
  // Opening happens before any page card exists, so a leaked slot would freeze
  // the reader with nothing on screen.  Bound it much tighter than the page
  // path: the stale job belongs to the previous document and finishes within the
  // ladder budget, and failing here lands on the load card, which is visible.
  if (!awaitSyncSlot(30000)) {
    LOG_ERR("PDF", "open refused: a previous synchronous job never released the slot");
    return false;
  }
  gSyncJobInFlight.store(true);
  PdfRenderJob& job = gSyncJob;
  job.reset(doc, PdfRenderJob::Kind::Open, 0);
  if (!enqueuePdfJob(job)) {
    gSyncJobInFlight.store(false);
    return false;
  }
  const bool completed = waitPdfJob(job);
  releaseSlotAfterWait(completed);
  return completed && job.ok;
}

bool renderPdfPage(PdfDocument& doc, int page, const std::string& path, int width, int height, bool cover,
                   const uint32_t timeoutMs = 75000) {
  if (!gPdfWorkerTask) return doc.renderPageToBmp(page, path, width, height, cover);
  if (!awaitSyncSlot(timeoutMs)) {
    LOG_ERR("PDF", "render refused: a previous synchronous job never released the slot (page=%d)", page);
    return false;
  }
  gSyncJobInFlight.store(true);
  PdfRenderJob& job = gSyncJob;
  job.reset(doc, PdfRenderJob::Kind::Render, page);
  job.path = path;
  job.width = width;
  job.height = height;
  job.cover = cover;
  if (!enqueuePdfJob(job)) {
    gSyncJobInFlight.store(false);
    return false;
  }
  const bool completed = waitPdfJob(job, timeoutMs);
  doc.setUseDisplayList(false);
  releaseSlotAfterWait(completed);  // NOT unconditional: see the helper
  return completed && job.ok;
}

// Text extraction must not run on the loopTask: MuPDF's stext/FreeType frames
// overflowed its 8 KB stack and corrupted the PSRAM heap (Bad tail).  The
// worker owns all MuPDF calls, which also serialises them with rendering.
bool extractPdfText(PdfDocument& doc, int page, std::vector<std::string>& paragraphs, bool& hasImages) {
  if (!gPdfWorkerTask) return doc.extractText(page, paragraphs, hasImages);
  if (!syncSlotFree()) {
    LOG_ERR("PDF", "extract refused: a previous synchronous job is still running (page=%d)", page);
    return false;
  }
  gSyncJobInFlight.store(true);
  // `paragraphs`/`hasImages` belong to the caller and are written by the worker
  // while this waits, so the caller must not return early -- which is why an
  // expired wait restarts instead of returning false.
  PdfRenderJob& job = gSyncJob;
  job.reset(doc, PdfRenderJob::Kind::ExtractText, page);
  job.paragraphs = &paragraphs;
  job.hasImages = &hasImages;
  if (!enqueuePdfJob(job)) {
    gSyncJobInFlight.store(false);
    return false;
  }
  const bool completed = waitPdfJob(job);
  releaseSlotAfterWait(completed);
  return completed && job.ok;
}

bool pdfPageHasText(PdfDocument& doc, int page) {
  if (!gPdfWorkerTask) return doc.pageHasText(page);
  if (!syncSlotFree()) {
    LOG_ERR("PDF", "hasText refused: a previous synchronous job is still running (page=%d)", page);
    return false;
  }
  gSyncJobInFlight.store(true);
  PdfRenderJob& job = gSyncJob;
  job.reset(doc, PdfRenderJob::Kind::HasText, page);
  if (!enqueuePdfJob(job)) {
    gSyncJobInFlight.store(false);
    return false;
  }
  const bool completed = waitPdfJob(job);
  releaseSlotAfterWait(completed);
  return completed && job.ok;
}

bool probePdfReflowAsync(PdfDocument& doc) {
  if (!gPdfWorkerQueue) return false;
  gReflowProbeJob.reset(doc, PdfRenderJob::Kind::ReflowProbe, 0);
  return enqueuePdfJob(gReflowProbeJob);
}

}  // namespace

namespace {
size_t utf8Next(const std::string& s, size_t i, uint32_t& cp) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) {
    cp = c;
    return 1;
  }
  if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
    cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
    return 2;
  }
  if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
    cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
         (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
    return 3;
  }
  if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
    cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
         ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
    return 4;
  }
  cp = '?';
  return 1;
}
// Reflow text must draw with a face that covers ordinary CJK.  With the
// unified system pack that is the pack's reader face: it is streamed on demand
// and stays loaded even though loadBook() released the SD font families.  The
// settings SD family would resolve to an unloaded id here and fall back to the
// small built-in face (visible as missing-glyph boxes in reflowed PDFs).
int reflowFontId() { return SETTINGS.getReaderFontId(); }
}  // namespace

int PdfReaderActivity::debugInitialPage = -1;
bool PdfReaderActivity::debugReflowEnabled = true;
int PdfReaderActivity::debugSeedNotePage = -1;
int PdfReaderActivity::debugSeedHighlightPage = -1;
bool PdfReaderActivity::debugSelfTest = false;

bool PdfReaderActivity::loadBook() {
  LOG_ERR("PDF", "loadBook enter: %s", bookPath.c_str());
  reflowBookEnabled_ = false;
  reflowProbeStarted_ = false;
  reflowProbePending_ = false;
  pdfPasswordRequired_ = false;
  auto loaded = makeUniqueNoThrow<PdfDocument>(bookPath);
  if (!loaded) {
    LOG_ERR("PDF", "OOM: PdfDocument");
    return false;
  }
  LOG_ERR("PDF", "loadBook: PdfDocument created");
  // Free the reader fonts before MuPDF opens the document.  Encrypted PDFs
  // allocate their xref/encryption structures during load(); doing that while
  // the SD family, full emoji TTF, pinyin and hyphenation tables are resident
  // left only a few hundred KB (and sometimes 5 KB) of PSRAM.
  releaseExternalFontPack();
  useBuiltinFontFallback();
  {
    const RenderLock lock;
    sdFontSystem.releaseSdFamilies(renderer);
    if (auto* fontCache = renderer.getFontCacheManager()) fontCache->releaseSdFontCaches();
    emoji_font::release(renderer);
  }
  pinyin::PinyinDb::shared().release();
  // Keep the already-preloaded hyphenation tries when PDF reflow will use
  // them.  Releasing disables lazy loading for the boot, and loadBook() runs
  // on the PSRAM-stacked activity/render task, so it cannot preload them again
  // (that would read internal flash from the wrong stack).
  if (!SETTINGS.hyphenationEnabled) {
    hyphenation::HyphenationStore::shared().releaseAll();
  }
  LOG_ERR("PDF", "reader caches released before PDF open: psram free=%u largest=%u",
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#if defined(CROSSPOINT_PDF_XIP)
  // The engine loader reads internal flash with esp_partition_read.  Do it on
  // the UI task's internal stack; doing it from the PSRAM worker trips IDF's
  // esp_task_stack_is_sane_cache_disabled() assert.
  if (!PdfDocument::preloadEngine()) {
    LOG_ERR("PDF", "PDF engine preload failed");
    return false;
  }
#endif
  // Open on the worker from the start: password-protected documents use the
  // encryption/xref path, whose stack usage does not fit the 8 KB UI task.
  startPdfWorker();
  loaded->setPassword(pdfPassword_);
  // One option, both directions: the engine splits the blocks MuPDF fused, the app merges
  // the lines the source hard-wrapped.  Set before load() so the first page already honours
  // it (load() forwards it to the engine after opening).
  loaded->setParagraphSplit(SETTINGS.pdfSmartLineMerge != 0);
  if (!loadPdfDocument(*loaded)) {
    if (loaded->needsPassword()) {
      pdfPasswordRequired_ = true;
      pdfPasswordWrong_ = loaded->wrongPassword();
      pdfPassword_.clear();
      LOG_ERR("PDF", "password required for %s%s", bookPath.c_str(), pdfPasswordWrong_ ? " (rejected)" : "");
    } else {
      pdfPassword_.clear();
      LOG_ERR("PDF", "failed to load %s", bookPath.c_str());
    }
    return false;
  }
  pdfPasswordRequired_ = false;
  pdfPasswordWrong_ = false;
  pdfPassword_.clear();
  LOG_ERR("PDF", "loadBook: loaded pages=%d", loaded->pageCount());
  pdf = std::move(loaded);
  // The tries were preloaded by main.cpp at boot; when hyphenation is enabled
  // loadBook() deliberately did not release them.  Selecting English here only
  // touches the PSRAM cache, which is safe from this PSRAM-stacked task.
  Hyphenator::setPreferredLanguage(SETTINGS.hyphenationEnabled ? "en" : "");

  cachePath_ = pdfCachePath(bookPath);
  if (cachePath_.empty()) {
    LOG_ERR("PDF", "no cache path for %s", bookPath.c_str());
    return false;
  }
  Storage.mkdir(cachePath_.c_str(), true);
  if (!ensureCacheValid()) {
    LOG_ERR("PDF", "cannot validate cache dir %s", cachePath_.c_str());
    return false;
  }
  // The reflow decision is a per-file property.  Persisting it avoids spending
  // three async stext samples on every open; a changed PDF invalidates the
  // whole cache directory (ensureCacheValid).
  {
    HalFile decision;
    if (Storage.openFileForRead("PDF", (cachePath_ + "/reflow.bin").c_str(), decision)) {
      uint8_t value = 0;
      if (decision.read(&value, 1) == 1) {
        reflowBookEnabled_ = value != 0;
        reflowProbeStarted_ = true;
        LOG_ERR("PDF", "reflow decision cached: %s", reflowBookEnabled_ ? "on" : "off");
      }
      decision.close();
    }
  }

  loadProgress();
  if (debugInitialPage >= 0 && pdf && pdf->pageCount() > 0) {
    currentPage = std::min(debugInitialPage, pdf->pageCount() - 1);
  }
  loadBookmarks();
  loadPageNotes();
  if (debugSeedNotePage >= 0 && debugSeedNotePage < pdf->pageCount()) {
    annotations::Annotation note{};
    note.spineIndex = static_cast<uint16_t>(debugSeedNotePage);
    note.startOffset = 0;
    note.endOffset = 1;
    note.kind = static_cast<uint8_t>(annotations::Kind::Note);
    note.note = "seeded note";
    note.chapterTitle = annotations::clampUtf8(getBookTitle(), annotations::kMaxTitleBytes);
    pageNotes_.push_back(std::move(note));
    annotations::normalize(pageNotes_);
    savePageNotes();
    LOG_ERR("PDF", "seeded note on page %d (%u total)", debugSeedNotePage + 1,
            static_cast<unsigned>(pageNotes_.size()));
    debugSeedNotePage = -1;
  }
  if (debugSeedHighlightPage >= 0 && debugSeedHighlightPage < pdf->pageCount()) {
    annotations::Annotation mark{};
    mark.spineIndex = static_cast<uint16_t>(debugSeedHighlightPage);
    mark.startOffset = 0;
    mark.endOffset = 12;
    mark.kind = static_cast<uint8_t>(annotations::Kind::Highlight);
    mark.chapterTitle = annotations::clampUtf8(getBookTitle(), annotations::kMaxTitleBytes);
    pageNotes_.push_back(std::move(mark));
    annotations::normalize(pageNotes_);
    savePageNotes();
    LOG_ERR("PDF", "seeded highlight on page %d", debugSeedHighlightPage + 1);
    debugSeedHighlightPage = -1;
  }
  fullCacheComplete_ = fullCacheMarkerValid();
  const int total = pdf->pageCount();
  const uint8_t startPercent =
      total > 0 ? static_cast<uint8_t>(std::min(100, (currentPage + 1) * 100 / total)) : 0;
  READING_STATS.beginSession(bookPath, getBookTitle(), "", "", startPercent, "", 0);
  return true;
}

bool PdfReaderActivity::handleLoadFailure() {
#if defined(CROSSPOINT_PDF_XIP)
  // An absent or incompatible engine is a dependency, not a broken file.  Resolve
  // it the way the EPUB reader resolves an unverifiable loan date: name the fix
  // and run it in place, then continue opening the same book.  The update screen
  // connects to the saved network itself and shows progress; only a failure or a
  // missing network leaves the user on this card.
  const pdf_module::Refusal refusal = pdf_module::lastRefusal();
  if (refusal != pdf_module::Refusal::None) {
    engineProblem_ = pdf_module::encodeRefusal(refusal);
    LOG_ERR("PDF", "engine refused (reason=%u)", static_cast<unsigned>(refusal));
    if (pdf_module::refusalIsDownloadable(refusal) && !engineUpdateAttempted_) {
      engineUpdateAttempted_ = true;
      openEngineUpdate();
      return true;
    }
    requestUpdate();
    return true;
  }
#endif
  if (!pdfPasswordRequired_) {
    // Not the engine, not a password: the file itself did not load.  Stay, say
    // so, and leave the way out to Back (loop() handles it).
    loadFailed_ = true;
    requestUpdate();
    LOG_ERR("PDF", "document did not load; showing the unreadable card");
    return true;
  }
  promptPdfPassword();
  return true;
}

void PdfReaderActivity::promptPdfPassword() {
  const char* title = pdfPasswordWrong_ ? tr(STR_PDF_PASSWORD_WRONG) : tr(STR_PASSWORD);
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        READING_STATS.resumeSession();
        if (result.isCancelled) {
          pdfPassword_.clear();
          pdfPasswordRequired_ = false;
          finish();
          return;
        }
        const auto& kb = std::get<KeyboardResult>(result.data);
        pdfPassword_ = kb.text;
        if (loadBook()) {
          pdfPassword_.clear();
          pdfPasswordRequired_ = false;
          pdfPasswordWrong_ = false;
          requestUpdate();
        } else if (pdfPasswordRequired_) {
          promptPdfPassword();
        } else {
          pdfPassword_.clear();
          finish();
        }
      },
      title, "", 128, InputType::Password);
}

std::string PdfReaderActivity::getBookTitle() const {
  std::string name = bookPath;
  const auto slash = name.find_last_of('/');
  if (slash != std::string::npos) name.erase(0, slash + 1);
  const auto dot = name.find_last_of('.');
  if (dot != std::string::npos) name.erase(dot);
  return name;
}

std::string PdfReaderActivity::pageCachePath(const int page) const {
  return cachePath_ + "/page_" + std::to_string(page) + ".bmp";
}

bool PdfReaderActivity::ensureCacheValid() {
  HalFile pdfFile;
  if (!Storage.openFileForRead("PDF", bookPath, pdfFile)) return false;
  const uint32_t size = static_cast<uint32_t>(pdfFile.fileSize());
  const uint32_t mtime = pdfFile.modificationTime();
  pdfFile.close();

  CacheMeta meta{};
  HalFile metaFile;
  bool valid = false;
  if (Storage.openFileForRead("PDF", cachePath_ + "/meta.bin", metaFile)) {
    CacheMeta old{};
    if (metaFile.read(&old, sizeof(old)) == static_cast<int>(sizeof(old)) && old.magic == kCacheMagic &&
        old.version == kCacheVersion && old.fileSize == size && old.mtime == mtime) {
      valid = true;
    }
    metaFile.close();
  }
  if (valid) return true;

  // The PDF changed (or this is the first open): drop the page BMPs and every other
  // derived blob, but keep annotations/bookmarks/progress -- they are the reader's work,
  // and a PDF that gained or lost a page cannot invalidate a quoted highlight anyway
  // (QuoteAnchor re-matches it against the CURRENT lines and simply stops drawing it when
  // the text is gone).  Progress is clamped to pageCount() on load.
  book_cache::pruneRegenerable(cachePath_);
  Storage.mkdir(cachePath_.c_str(), true);
  cachedPages_.clear();
  fullCacheComplete_ = false;
  fullCacheActive_ = false;

  HalFile out;
  if (Storage.openFileForWrite("PDF", cachePath_ + "/meta.bin", out)) {
    CacheMeta fresh{kCacheMagic, kCacheVersion, 0, size, mtime};
    out.write(&fresh, sizeof(fresh));
    out.close();
  }
  return true;
}

void PdfReaderActivity::rememberCachedPage(const int page) {
  if (fullCacheComplete_) return;
  const auto it = std::find(cachedPages_.begin(), cachedPages_.end(), page);
  if (it != cachedPages_.end()) cachedPages_.erase(it);
  cachedPages_.push_back(page);
  if (fullCacheActive_) return;
  while (cachedPages_.size() > kMaxCachedPages) {
    const int old = cachedPages_.front();
    cachedPages_.pop_front();
    Storage.remove(pageCachePath(old).c_str());
  }
}

bool PdfReaderActivity::fullCacheMarkerValid() const {
  if (!pdf || cachePath_.empty()) return false;
  HalFile file;
  if (!Storage.openFileForRead("PDF", cachePath_ + "/full.cache", file)) return false;
  FullCacheMeta meta{};
  const int n = file.read(&meta, sizeof(meta));
  file.close();
  if (n != static_cast<int>(sizeof(meta)) || std::memcmp(meta.magic, kFullCacheMagic, 4) != 0 ||
      meta.version != kFullCacheVersion || meta.pageCount != static_cast<uint32_t>(pdf->pageCount())) {
    return false;
  }
  for (int i = 0; i < pdf->pageCount(); ++i) {
    if (!Storage.exists(pageCachePath(i).c_str())) return false;
  }
  return true;
}

void PdfReaderActivity::writeFullCacheMarker() {
  if (!pdf || cachePath_.empty()) return;
  HalFile out;
  if (!Storage.openFileForWrite("PDF", cachePath_ + "/full.cache", out)) return;
  FullCacheMeta meta{};
  std::memcpy(meta.magic, kFullCacheMagic, 4);
  meta.version = kFullCacheVersion;
  meta.pageCount = static_cast<uint32_t>(pdf->pageCount());
  out.write(&meta, sizeof(meta));
  out.close();
}

void PdfReaderActivity::drawCacheProgress(const int done, const int total) {
  char text[32];
  std::snprintf(text, sizeof(text), "PDF %d/%d", done, total);
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, text, true, EpdFontFamily::BOLD);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool PdfReaderActivity::ensureFullCache(const int usableWidth, const int pageHeight) {
  if (!pdf) return false;
  if (fullCacheMarkerValid()) {
    fullCacheComplete_ = true;
    return true;
  }

  fullCacheActive_ = true;
  const int total = pdf->pageCount();
  for (int i = 0; i < total; ++i) {
    const std::string path = pageCachePath(i);
    if (Storage.exists(path.c_str())) continue;
    // A page that already failed the whole fallback ladder is remembered by an
    // empty marker next to its cache entry.  Retrying it costs ~50 s of PSRAM
    // thrash and has never succeeded; the marker is removed when the reader
    // opens the page deliberately, which gives the retry it deserves.
    const std::string failed = path + ".failed";
    if (Storage.exists(failed.c_str())) {
      pdf->writeBlankBmp(path, usableWidth, pageHeight);
      continue;
    }
    const bool hasText = pdf->pageCount() <= 32 ? pdfPageHasText(*pdf, i) : false;
    const bool cover = pdf->pageCount() <= 32 && !hasText;
    // Prefetch runs while the user is reading *another* page, and its cost is
    // paid by the whole UI: this render holds the render lock, so serial input
    // and key presses wait for it (measured: a page that grinds for 70 s made
    // every bench command arrive 70 s late).  A page the user did not ask for is
    // not worth that: give it a quarter of the on-demand budget and let the
    // failure marker skip it.  Turning to the page deliberately still removes the
    // marker and gets the full budget.
    // 8 s, not 20: a prefetch holds the render task, and a turn that arrives
    // during it shows the previous page until the prefetch *or its budget*
    // ends.  The reader cannot paint a card for that window (the render task
    // owns the frame), so the window itself has to stay short.  Pages that need
    // longer still render on demand with the full 75 s budget -- the marker
    // this path writes only skips the prefetch, not the page.
    constexpr uint32_t kPrefetchTimeoutMs = 8000;
    if (!renderPdfPage(*pdf, i, path, usableWidth, pageHeight, cover, kPrefetchTimeoutMs)) {
      // renderPageToBmp already retries the whole fallback ladder (context
      // reset + trim + bands + text-only); a second call here just ran that
      // ladder again and doubled the cost of every unrenderable page.
      LOG_ERR("PDF", "cache page %d failed; writing placeholder", i + 1);
      HalFile marker;
      if (Storage.openFileForWrite("PDF", failed.c_str(), marker)) marker.close();
      pdf->writeBlankBmp(path, usableWidth, pageHeight);
    }
    fullCacheActive_ = false;
    return true;
  }
  writeFullCacheMarker();
  fullCacheActive_ = false;
  fullCacheComplete_ = true;
  LOG_ERR("PDF", "full cache complete pages=%d", total);
  return true;
}

void PdfReaderActivity::loadPageNotes() {
  pageNotes_.clear();
  if (cachePath_.empty()) return;
  HalFile file;
  if (!Storage.openFileForRead("PDF", cachePath_ + "/annotations.bin", file)) return;
  const int size = static_cast<int>(file.fileSize());
  if (size <= 0 || size > 64 * 1024) {
    file.close();
    return;
  }
  std::vector<uint8_t> raw(static_cast<size_t>(size));
  const int n = file.read(raw.data(), size);
  file.close();
  if (n != size) return;
  // Same codec the EPUB reader uses; a damaged file is rejected whole.
  annotations::deserialize(raw.data(), raw.size(), pageNotes_);
}

bool PdfReaderActivity::savePageNotes() {
  if (cachePath_.empty()) return false;
  const auto bytes = annotations::serialize(pageNotes_);
  const std::string path = cachePath_ + "/annotations.bin";
  const std::string tmp = path + ".part";
  HalFile file;
  if (!Storage.openFileForWrite("PDF", tmp.c_str(), file)) return false;
  const bool written = file.write(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
  file.close();
  if (!written || !Storage.replaceFile(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  // One observable count for every annotation mutation, because the failures this
  // file has had were all "an annotation quietly disappeared": two selections
  // collapsing in annotations::normalize(), and editPageNote() wiping the page.
  size_t onPage = 0;
  for (const auto& a : pageNotes_) {
    if (a.spineIndex == static_cast<uint16_t>(currentPage)) ++onPage;
  }
  LOG_INF("PDF", "annotations persisted: total=%u onPage=%d", static_cast<unsigned>(pageNotes_.size()),
          static_cast<unsigned>(onPage));
  return true;
}

const annotations::Annotation* PdfReaderActivity::noteForPage(const int page) const {
  // The whole-page marker is the (0,1) line range -- see the page-note path that
  // writes it.  Matching on the page alone would let a *selection* note answer
  // here, which then prefilled the page-note editor with someone's highlight text.
  for (const auto& note : pageNotes_) {
    if (note.spineIndex == static_cast<uint16_t>(page) &&
        anno_anchor::isWholePageMarker(note.startOffset, note.endOffset)) {
      return &note;
    }
  }
  return nullptr;
}

void PdfReaderActivity::editPageNote() {
  if (!pdf) return;
  const annotations::Annotation* existing = noteForPage(currentPage);
  const std::string initial = existing ? existing->note : std::string();
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        READING_STATS.resumeSession();
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        const auto& kb = std::get<KeyboardResult>(result.data);
        std::string text = annotations::clampUtf8(kb.text, annotations::kMaxNoteBytes);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
        // Only the page marker is replaced. Removing every annotation on the page
        // -- which is what this did -- deleted the reader's highlights and
        // selection notes as a side effect of writing a page note.
        pageNotes_.erase(std::remove_if(pageNotes_.begin(), pageNotes_.end(),
                                        [this](const annotations::Annotation& a) {
                                          return a.spineIndex == static_cast<uint16_t>(currentPage) &&
                                                 anno_anchor::isWholePageMarker(a.startOffset, a.endOffset);
                                        }),
                         pageNotes_.end());
        if (!text.empty()) {
          annotations::Annotation note{};
          note.spineIndex = static_cast<uint16_t>(currentPage);
          note.startOffset = anno_anchor::kPageMarkerStart;
          note.endOffset = anno_anchor::kPageMarkerEnd;  // whole page: the marker covers this page
          note.kind = static_cast<uint8_t>(annotations::Kind::Note);
          // The page's first line makes the note recognisable once listed.
          note.quote = reflowLines_.empty() ? std::string() : annotations::clampUtf8(reflowLines_.front(), annotations::kMaxQuoteBytes);
          note.note = text;
          note.chapterTitle = annotations::clampUtf8(getBookTitle(), annotations::kMaxTitleBytes);
          pageNotes_.push_back(std::move(note));
        }
        annotations::normalize(pageNotes_);
        savePageNotes();
        requestUpdate();
      },
      tr(STR_PDF_PAGE_NOTE), initial, 400, InputType::Text);
}

void PdfReaderActivity::toggleScreenHighlight() {
  if (!reflowMode_ || reflowLines_.empty()) {
    // A bitmap page has no text lines to underline; the note entry still works.
    return;
  }
  // For PDF, annotation offsets are LINE indices (the EPUB reader uses text
  // offsets; a PDF page has no stable text coordinate across re-renders).
  const int firstLine = reflowScreen_ < static_cast<int>(reflowScreenStarts_.size()) ? reflowScreenStarts_[reflowScreen_] : 0;
  const int lastLine = reflowScreen_ + 1 < static_cast<int>(reflowScreenStarts_.size())
                           ? reflowScreenStarts_[reflowScreen_ + 1]
                           : static_cast<int>(reflowLines_.size());
  const uint32_t start = static_cast<uint32_t>(firstLine);
  const uint32_t end = static_cast<uint32_t>(lastLine);
  pageNotes_.erase(std::remove_if(pageNotes_.begin(), pageNotes_.end(),
                                  [this, start](const annotations::Annotation& a) {
                                    return a.spineIndex == static_cast<uint16_t>(currentPage) &&
                                           a.kind == static_cast<uint8_t>(annotations::Kind::Highlight) &&
                                           a.startOffset == start;
                                  }),
                   pageNotes_.end());
  annotations::Annotation mark{};
  mark.spineIndex = static_cast<uint16_t>(currentPage);
  mark.startOffset = start;
  mark.endOffset = end;
  mark.kind = static_cast<uint8_t>(annotations::Kind::Highlight);
  std::string quote;
  for (int i = firstLine; i < lastLine && i < static_cast<int>(reflowLines_.size()); ++i) {
    if (!quote.empty()) quote.push_back(' ');
    quote += reflowLines_[static_cast<size_t>(i)];
    if (quote.size() >= annotations::kMaxQuoteBytes) break;
  }
  mark.quote = annotations::clampUtf8(quote, annotations::kMaxQuoteBytes);
  mark.chapterTitle = annotations::clampUtf8(getBookTitle(), annotations::kMaxTitleBytes);
  pageNotes_.push_back(std::move(mark));
  annotations::normalize(pageNotes_);
  savePageNotes();
  requestUpdate();
}

void PdfReaderActivity::loadBookmarks() {
  bookmarks_.clear();
  if (cachePath_.empty() || !pdf) return;
  HalFile file;
  if (!Storage.openFileForRead("PDF", cachePath_ + "/bookmarks.bin", file)) return;
  struct Header {
    char magic[4];
    uint16_t version;
    uint16_t count;
  };
  Header header{};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) return;
  if (std::memcmp(header.magic, "PDFB", 4) != 0 || header.version != 1) return;
  const int count = std::min<int>(header.count, 500);
  for (int i = 0; i < count; ++i) {
    uint32_t page = 0;
    if (file.read(&page, sizeof(page)) != static_cast<int>(sizeof(page))) break;
    if (page < static_cast<uint32_t>(pdf->pageCount()) &&
        std::find(bookmarks_.begin(), bookmarks_.end(), static_cast<int>(page)) == bookmarks_.end()) {
      bookmarks_.push_back(static_cast<int>(page));
    }
  }
  std::sort(bookmarks_.begin(), bookmarks_.end());
}

bool PdfReaderActivity::saveBookmarks() {
  if (cachePath_.empty()) return false;
  struct Header {
    char magic[4];
    uint16_t version;
    uint16_t count;
  };
  Header header{{'P', 'D', 'F', 'B'}, 1, static_cast<uint16_t>(std::min<size_t>(bookmarks_.size(), 500))};
  const std::string path = cachePath_ + "/bookmarks.bin";
  const std::string tmp = path + ".part";
  HalFile out;
  if (!Storage.openFileForWrite("PDF", tmp.c_str(), out)) return false;
  bool ok = out.write(&header, sizeof(header)) == static_cast<int>(sizeof(header));
  for (size_t i = 0; ok && i < header.count; ++i) {
    const uint32_t page = static_cast<uint32_t>(bookmarks_[i]);
    ok = out.write(&page, sizeof(page)) == static_cast<int>(sizeof(page));
  }
  out.close();
  if (!ok || !Storage.replaceFile(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

void PdfReaderActivity::toggleBookmark(const int page) {
  if (!pdf || page < 0 || page >= pdf->pageCount()) return;
  const auto it = std::find(bookmarks_.begin(), bookmarks_.end(), page);
  if (it != bookmarks_.end()) {
    bookmarks_.erase(it);
  } else {
    bookmarks_.push_back(page);
    std::sort(bookmarks_.begin(), bookmarks_.end());
  }
  saveBookmarks();
}

std::vector<PdfReaderActivity::BookmarkRow> PdfReaderActivity::bookmarkRows() const {
  std::vector<BookmarkRow> rows;
  rows.reserve(bookmarks_.size() + pageNotes_.size());
  for (const int page : bookmarks_) rows.push_back({page, false});
  for (size_t i = 0; i < pageNotes_.size(); ++i) {
    const auto& note = pageNotes_[i];
    rows.push_back({static_cast<int>(note.spineIndex), true, i, note.kind});
  }
  return rows;
}

bool PdfReaderActivity::isPageBookmarked(const int page) const {
  return std::find(bookmarks_.begin(), bookmarks_.end(), page) != bookmarks_.end();
}

int PdfReaderActivity::currentBookmarkIndex() const {
  const auto it = std::find(bookmarks_.begin(), bookmarks_.end(), currentPage);
  return it == bookmarks_.end() ? -1 : static_cast<int>(it - bookmarks_.begin());
}

int PdfReaderActivity::currentOutlineIndex() const {
  if (!pdf) return -1;
  const auto& outline = pdf->outline();
  int best = -1;
  int bestPage = -1;
  for (size_t i = 0; i < outline.size(); ++i) {
    const int page = outline[i].page;
    if (page <= currentPage && page >= bestPage) {
      bestPage = page;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void PdfReaderActivity::onExit() {
  READING_STATS.endSession();
  discardToolbarPage();
  pdfPassword_.clear();
  if (reflowProbePending_) {
    // The probe job holds pdf and runs on the worker; deleting the task now
    // would free the document under MuPDF.  Three sampled pages take a few
    // seconds each on this hardware, so wait with a generous bound.
    const uint32_t started = millis();
    while (!gReflowProbeJob.done.load() && millis() - started < 40000) vTaskDelay(pdMS_TO_TICKS(10));
    reflowProbePending_ = false;
    LOG_ERR("PDF", "reflow probe joined in %ums", static_cast<unsigned>(millis() - started));
  }
#if defined(CROSSPOINT_PDF_XIP)
  const bool workerStopped = stopPdfWorker();
#else
  stopPdfWorker();  // no engine module to unmap in this build
#endif
  // The MuPDF context still owns several megabytes of page resources while
  // this object is alive.  Drop the document before reloading the SD family,
  // the external CJK pack, hyphenation tables and the emoji TTF: otherwise
  // those reloads run against a fragmented PSRAM heap and silently fall back
  // to the built-in faces (observed "not enough PSRAM for the SD emoji font"
  // right after leaving a PDF).
  pdf.reset();
  resetReflow();
  if (selectionBase_ != nullptr) {
    heap_caps_free(selectionBase_);
    selectionBase_ = nullptr;
    selectionBaseBytes_ = 0;
  }
#if defined(CROSSPOINT_PDF_XIP)
  // Resident only while reading (the user's rule): the engine is 1.8 MB of code
  // and rodata mapped from its partition plus its .data/.bss in PSRAM (~54 KB),
  // and none of that is needed with the document closed.  Every consumer calls
  // ensureLoaded() first, so the next open re-maps the same image.  Skipped
  // when a worker was leaked above: its document may still be live.
  // ★ 2026-11-22: the unmap was disabled after a reproducible `Cache error` on
  // the NEXT open (mmu unmap + re-map cycle, 2/2 reproductions after this change
  // and none before it in the same workload).  "Resident only while reading" is
  // not worth a panic; the mapping and ~54 KB of .data/.bss stay for the session.
  // ★ 2026-11-23 (S31): the same experiment was repeated on the Plus with the
  // deferred flush running in the shell's main loop.  The unmap itself
  // succeeded ("PSRAM free 13,826,160") and the next open caused an abort with
  // a misaligned load before the reader repainted: unmapping invalidates
  // caches that live PSRAM state (framebuffer/page cache) is still using.
  // Both targets therefore keep the engine mapped for the session; the
  // deferred-unload plumbing stays wired for a future safe implementation.
  constexpr bool kUnmapEngineOnExit = false;
  if (kUnmapEngineOnExit && workerStopped && pdf_module::openDocuments() == 0) {
    // onExit usually runs on the render task, whose stack is in PSRAM; unmapping
    // external memory flushes/invalidates caches, which corrupts exactly that
    // stack (this branch has the Double-exception tee-shirt).  Defer to the main
    // loop's internal stack.
    if (esp_ptr_external_ram(esp_cpu_get_sp())) {
      pdf_module::scheduleUnload();
      LOG_INF("PDF", "engine unmap deferred: onExit ran on a PSRAM stack");
    } else {
      pdf_module::unload();
      LOG_INF("PDF", "engine module unmapped; PSRAM free %u",
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
  } else {
    LOG_ERR("PDF", "engine module kept mapped: worker stopped=%d documents=%u", workerStopped ? 1 : 0,
            static_cast<unsigned>(pdf_module::openDocuments()));
  }
#endif
  // loadBook() released the hyphenation tries to make room for MuPDF.  Reload
  // them only on an internal-RAM stack: preloadAll() reads the data partition,
  // and an internal-flash read from a PSRAM stack trips the IDF cache guard.
  if (!esp_ptr_external_ram(esp_cpu_get_sp())) {
    hyphenation::HyphenationStore::shared().preloadAll();
  } else {
    LOG_ERR("PDF", "hyphenation reload skipped on PSRAM stack; next boot will reload");
  }
  // Restore the external CJK pack that loadBook() released to make room for
  // MuPDF.  If the reload fails, refreshExternalFontFaces() leaves the
  // built-in face installed rather than dangling pointers.
  refreshExternalFontFaces();
  // loadBook() also dropped the SD font family (the core CJK TTF alone holds
  // 2.5 MB of PSRAM) and the emoji TTF.  Reload emoji first: it needs one
  // 0.9 MB contiguous block, which the 2.5 MB core family would fragment away.
  emoji_font::reload(renderer, SETTINGS.fontPointSize);
  if (!esp_ptr_external_ram(esp_cpu_get_sp())) {
    sdFontSystem.ensureLoaded(renderer);
  } else {
    LOG_ERR("PDF", "SD font reload skipped on PSRAM stack; next reader will reload");
  }
  ReaderActivity::onExit();
}

int PdfReaderActivity::progressPercent() const {
  if (!pdf || pdf->pageCount() <= 0) return 0;
  return std::clamp((currentPage + 1) * 100 / pdf->pageCount(), 0, 100);
}

bool PdfReaderActivity::usesToolbarMenu() const {
  return SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

void PdfReaderActivity::discardToolbarPage() {
  if (toolbarPageStored_) {
    renderer.discardStoredBwBuffer();
    toolbarPageStored_ = false;
  }
  toolbarStoredPage_ = -1;
}

void PdfReaderActivity::openToolbar() {
  if (!pdf || toolbarOpen) return;
  toolbarOpen = true;
  toolbarPanel = false;
  focusedTool = 0;
  if (!toolbarUi) toolbarUi = makeUniqueNoThrow<ReaderToolbarUi>(renderer);
  if (!toolbarUi) {
    LOG_ERR("PDF", "OOM allocating reader toolbar");
    toolbarOpen = false;
    return;
  }
  toolbarUi->begin();
  const int start = !pdf->outline().empty() ? currentOutlineIndex() : currentPage;
  toolbarUi->nav().reset(std::max(0, start));
  // The page is already on screen.  Snapshot it and paint the toolbar on top
  // with one FAST push; a full renderPage() here reflows/rasterises the page
  // again and is the source of the PDF toolbar flicker.
  {
    RenderLock lock;
    discardToolbarPage();
    toolbarPageStored_ = renderer.storeBwBuffer();
    toolbarStoredPage_ = currentPage;
    renderToolbarOverlay();
    if (toolbarPageStored_) {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
      return;
    }
  }
  requestUpdate();
}

void PdfReaderActivity::closeToolbar() {
  const bool samePage = toolbarPageStored_ && toolbarStoredPage_ == currentPage;
  toolbarOpen = false;
  toolbarPanel = false;
  if (samePage) {
    RenderLock lock;
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    toolbarPageStored_ = false;
    toolbarStoredPage_ = -1;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  discardToolbarPage();
  requestUpdate();
}

void PdfReaderActivity::invalidatePageCache() {
  if (!pdf || cachePath_.empty()) return;
  // Font, line spacing and screen margins change the bitmap's fitted size and
  // crop decision too, so cached page BMPs from the old settings cannot be
  // reused.
  for (int i = 0; i < pdf->pageCount(); ++i) {
    const std::string path = pageCachePath(i);
    Storage.remove(path.c_str());
    Storage.remove((path + ".failed").c_str());
  }
  Storage.remove((cachePath_ + "/full.cache").c_str());
  fullCacheComplete_ = false;
}

void PdfReaderActivity::openTextSettings() {
  closeToolbar();
  startActivityForResultWith<TextSettingsActivity>(
      [this](const ActivityResult&) {
        READING_STATS.resumeSession();
        resetReflow();
        invalidatePageCache();
        requestUpdate();
      },
      &sdFontSystem.registry(), TextSettingsActivity::Tab::Layout);
}

int PdfReaderActivity::toolbarItemCount() const {
  if (!pdf) return 0;
  switch (focusedTool) {
    case 0: return pdf->outline().empty() ? pdf->pageCount() : static_cast<int>(pdf->outline().size());
    case 1: return 1;
    case 2: return bookmarks_.empty() ? 1 : static_cast<int>(bookmarks_.size());
    case 3: return 7;
    default: return 0;
  }
}

void PdfReaderActivity::activateToolbarRow(const int index) {
  if (!pdf) return;
  if (focusedTool == 0) {
    if (!pdf->outline().empty()) {
      if (index >= 0 && index < static_cast<int>(pdf->outline().size())) {
        currentPage = pdf->outline()[static_cast<size_t>(index)].page;
      }
    } else if (pdf->pageCount() > 0) {
      currentPage = std::clamp(index, 0, pdf->pageCount() - 1);
    }
    closeToolbar();
    return;
  }
  if (focusedTool == 1) {
    openTextSettings();
    return;
  }
  if (focusedTool == 2) {
    const auto rows = bookmarkRows();
    if (index >= 0 && index < static_cast<int>(rows.size())) {
      currentPage = std::clamp(rows[static_cast<size_t>(index)].page, 0, std::max(0, pdf->pageCount() - 1));
      closeToolbar();
    }
    return;
  }
  if (focusedTool != 3) return;

  switch (index) {
    case 0:
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % 4);
      SETTINGS.saveToFile();
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
      requestUpdate();
      break;
    case 1:
      closeToolbar();
      onReaderMenuConfirm(static_cast<int>(EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT));
      break;
    case 2:
      closeToolbar();
      onReaderMenuConfirm(static_cast<int>(EpubReaderMenuActivity::MenuAction::GO_TO_PAGE));
      break;
    case 3:
      closeToolbar();
      onReaderMenuConfirm(static_cast<int>(EpubReaderMenuActivity::MenuAction::SCREENSHOT));
      break;
    case 4:
      onGoHome();
      break;
    case 5:
      clearBookCache(bookPath);
      onGoHome();
      break;
    case 6:
      // PDF-specific typography toggle: rejoin hard-wrapped lines. Rebuild the
      // current page's reflow so it takes effect immediately.
      applySmartLineMerge(SETTINGS.pdfSmartLineMerge == 0);
      break;
    case 7:
      closeToolbar();
      exportAnnotations();
      break;
    default:
      break;
  }
}

void PdfReaderActivity::exportAnnotations() {
  std::string path;
  const auto result =
      annotation_export::write("PDF", cachePath_, getBookTitle(), pageNotes_, path);
  if (result == annotation_export::Result::Written || result == annotation_export::Result::ShortWrite) {
    toast_.show(path.c_str(), millis());
  } else {
    toast_.show(tr(STR_NOT_AVAILABLE), millis());
  }
  requestUpdate();
}

void PdfReaderActivity::handleToolbarInput() {
  if (!toolbarUi) {
    closeToolbar();
    return;
  }

  const auto routed = toolbarUi->route(mappedInput);
  if (routed.routed) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeToolbar();
        return;
      case ReaderToolbarUi::Event::Tool:
        if (routed.value >= 0 && routed.value < 4) {
          focusedTool = routed.value;
          toolbarPanel = true;
          const int start = (focusedTool == 0 && !pdf->outline().empty())
                                ? currentOutlineIndex()
                                : (focusedTool == 0 ? currentPage : 0);
          toolbarUi->nav().reset(std::max(0, start));
          requestUpdate();
        }
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        pageTurn(false);
        requestUpdate();
        return;
      case ReaderToolbarUi::Event::NextChapter:
        pageTurn(true);
        requestUpdate();
        return;
      case ReaderToolbarUi::Event::Scrub:
        if (routed.permille >= 0 && pdf && pdf->pageCount() > 0) {
          currentPage = std::clamp(routed.permille * pdf->pageCount() / 1000, 0, pdf->pageCount() - 1);
          requestUpdate();
        }
        return;
      case ReaderToolbarUi::Event::Row:
        if (toolbarPanel) activateToolbarRow(routed.value);
        return;
      case ReaderToolbarUi::Event::BookmarkAction:
        if (focusedTool == 2) {
          toggleBookmark(currentPage);
          requestUpdate();
        }
        return;
      case ReaderToolbarUi::Event::ChapterAction:
        if (focusedTool == 2) {
          const auto rows = bookmarkRows();
          if (routed.value >= 0 && routed.value < static_cast<int>(rows.size())) {
            const auto& row = rows[static_cast<size_t>(routed.value)];
            if (row.note) {
              // Delete only the selected annotation.  A page can hold both a
              // note and several highlights; the old code erased every
              // annotation on that page from one row's delete button.
              if (row.noteIndex < pageNotes_.size()) {
                pageNotes_.erase(pageNotes_.begin() + static_cast<std::ptrdiff_t>(row.noteIndex));
                annotations::normalize(pageNotes_);
                savePageNotes();
              }
            } else {
              bookmarks_.erase(std::remove(bookmarks_.begin(), bookmarks_.end(), row.page), bookmarks_.end());
              saveBookmarks();
            }
            requestUpdate();
          }
        }
        return;
      default:
        requestUpdate();
        return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeToolbar();
    return;
  }

  auto& nav = toolbarUi->nav();
  if (!toolbarPanel) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      toolbarPanel = true;
      nav.reset(focusedTool == 0 ? currentPage : 0);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + 3) % 4;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % 4;
      requestUpdate();
    }
    return;
  }

  const int count = toolbarItemCount();
  if (count <= 0) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateToolbarRow(nav.selected.load());
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    nav.reset(std::max(0, nav.selected.load() - 1));
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    nav.reset(std::min(count - 1, nav.selected.load() + 1));
    requestUpdate();
  }
}

void PdfReaderActivity::renderToolbarOverlay() {
  if (!toolbarUi || !pdf) return;

  ReaderToolbarUi::Model model;
  model.activeTool = toolbarPanel ? focusedTool : -1;

  std::string title = getBookTitle();
  std::string pageInfo = std::to_string(currentPage + 1) + "/" + std::to_string(pdf->pageCount()) + "   " +
                         std::to_string(progressPercent()) + "%";

  if (!toolbarPanel) {
    model.chapterTitle = title.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = pdf->pageCount() > 0 ? (currentPage + 1) * 1000 / pdf->pageCount() : 0;
  } else {
    model.panel = true;
    model.denseRows = !mappedInput.hasTouch();
    model.selectedIndex = mappedInput.hasTouch() ? -1 : toolbarUi->nav().selected.load();
    model.itemCount = toolbarItemCount();
    if (focusedTool == 0) {
      model.panelTitle = tr(STR_TOOL_CONTENTS);
      if (!pdf->outline().empty()) {
        model.itemCount = static_cast<int>(pdf->outline().size());
        model.rowText = [this](int i) {
          const auto& entry = pdf->outline()[static_cast<size_t>(i)];
          return entry.title.empty() ? std::to_string(entry.page + 1) : entry.title;
        };
        model.rowValue = [this](int i) {
          char percent[8];
          snprintf(percent, sizeof(percent), "%d%%",
                   (pdf->outline()[static_cast<size_t>(i)].page + 1) * 100 / std::max(1, pdf->pageCount()));
          return std::string(percent);
        };
        model.currentChapterIndex = currentOutlineIndex();
      } else {
        model.itemCount = pdf->pageCount();
        model.rowText = [](int i) { return std::to_string(i + 1); };
        model.rowValue = [](int) { return std::string(); };
        model.currentChapterIndex = currentPage;
      }
    } else if (focusedTool == 1) {
      model.panelTitle = tr(STR_TOOL_TYPESETTING);
      model.textGrid = false;
      model.itemCount = 1;
      model.rowText = [](int) { return std::string(tr(STR_TEXT_SETTINGS)); };
      model.rowValue = [](int) { return std::to_string(SETTINGS.fontPointSize) + "pt"; };
    } else if (focusedTool == 2) {
      model.panelTitle = tr(STR_BOOKMARKS);
      model.bookmarkAction = true;
      model.bookmarkActionRemove = isPageBookmarked(currentPage);
      const auto rows = bookmarkRows();
      model.itemCount = rows.empty() ? 1 : static_cast<int>(rows.size());
      model.rowText = [this, rows](int i) {
        if (rows.empty()) return std::string(tr(STR_NO_BOOKMARKS));
        const auto& row = rows[static_cast<size_t>(i)];
        if (row.note && row.noteIndex < pageNotes_.size() && !pageNotes_[row.noteIndex].quote.empty()) {
          return std::to_string(row.page + 1) + "  " + pageNotes_[row.noteIndex].quote;
        }
        return std::to_string(row.page + 1);
      };
      model.rowValue = [this, rows](int i) {
        if (rows.empty()) return std::string();
        const auto& row = rows[static_cast<size_t>(i)];
        if (row.note) {
          const char* label =
              row.kind == static_cast<uint8_t>(annotations::Kind::Note) ? tr(STR_PDF_PAGE_NOTE) : tr(STR_PDF_HIGHLIGHT);
          return std::string(label);
        }
        char percent[8];
        snprintf(percent, sizeof(percent), "%d%%", (row.page + 1) * 100 / std::max(1, pdf->pageCount()));
        return std::string(percent);
      };
      model.rowTail = [rows](int i) -> ReaderToolbarUi::RowTail {
        return {ReaderToolbarUi::TailBadge::None,
                (i < static_cast<int>(rows.size())) ? ReaderToolbarUi::TailAction::Delete
                                                    : ReaderToolbarUi::TailAction::None};
      };
      model.currentChapterIndex = [&]() {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
          if (!rows[static_cast<size_t>(i)].note && rows[static_cast<size_t>(i)].page == currentPage) return i;
        }
        return -1;
      }();
    } else {
      model.panelTitle = tr(STR_TOOL_MORE);
      model.itemCount = 8;
      model.rowText = [](int i) -> std::string {
        switch (i) {
          case 0: return std::string(tr(STR_ORIENTATION));
          case 1: return std::string(tr(STR_GO_TO_PERCENT));
          case 2: return std::string(tr(STR_GO_TO_PAGE));
          case 3: return std::string(tr(STR_SCREENSHOT_BUTTON));
          case 4: return std::string(tr(STR_GO_HOME_BUTTON));
          case 5: return std::string(tr(STR_DELETE_CACHE));
          case 6: return std::string(tr(STR_PDF_SMART_MERGE));
          default: return std::string(tr(STR_EXPORT_ANNOTATIONS));
        }
      };
      model.rowValue = [](int i) -> std::string {
        if (i == 6) return std::string(SETTINGS.pdfSmartLineMerge ? tr(STR_ON) : tr(STR_OFF));
        if (i != 0) return {};
        switch (SETTINGS.orientation) {
          case 0: return std::string(tr(STR_PORTRAIT));
          case 1: return std::string(tr(STR_LANDSCAPE_CW));
          case 2: return std::string(tr(STR_INVERTED));
          default: return std::string(tr(STR_LANDSCAPE_CCW));
        }
      };
    }
  }

  toolbarUi->setModel(model);
  toolbarUi->render();
}

bool PdfReaderActivity::handleFormatInput() {
  READING_STATS.tickActiveSession();
  if (!pdf) return false;
  if (toolbarOpen) {
    handleToolbarInput();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    openReaderMenu();
    return true;
  }
  return false;
}

void PdfReaderActivity::openReaderMenu() {
  if (!pdf) return;
  LOG_INF("PDF", "reader menu requested (toolbar=%d)", usesToolbarMenu() ? 1 : 0);
  if (usesToolbarMenu()) {
    openToolbar();
    if (toolbarOpen) return;
  }
  startActivityForResultWith<EpubReaderMenuActivity>(
      [this](const ActivityResult& result) {
        READING_STATS.resumeSession();
        const auto& menu = std::get<MenuResult>(result.data);
        if (menu.orientation != SETTINGS.orientation) {
          SETTINGS.orientation = menu.orientation;
          SETTINGS.saveToFile();
          ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
        }
        if (!result.isCancelled) {
          onReaderMenuConfirm(menu.action);
        } else {
          requestUpdate();
        }
      },
      getBookTitle(), currentPage + 1, pdf->pageCount(), progressPercent(), SETTINGS.orientation, false, false,
      false, selectedMenuAction, true);
}

void PdfReaderActivity::onReaderMenuConfirm(const int action) {
  using MenuAction = EpubReaderMenuActivity::MenuAction;
  switch (static_cast<MenuAction>(action)) {
    case MenuAction::PDF_PAGE_NOTE: {
      editPageNote();
      return;
    }
    case MenuAction::PDF_HIGHLIGHT_SCREEN: {
      toggleScreenHighlight();
      return;
    }
    case MenuAction::GO_TO_PAGE: {
      const std::string initial = std::to_string(currentPage + 1);
      startActivityForResultWith<KeyboardEntryActivity>(
          [this](const ActivityResult& result) {
            READING_STATS.resumeSession();
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& kb = std::get<KeyboardResult>(result.data);
            const char* text = kb.text.c_str();
            char* end = nullptr;
            const long value = std::strtol(text, &end, 10);
            if (end == text || *end != '\0' || !pdf || pdf->pageCount() <= 0) return;
            currentPage = std::clamp(static_cast<int>(value) - 1, 0, pdf->pageCount() - 1);
            requestUpdate();
          },
          tr(STR_GO_TO_PAGE), initial, 7, InputType::Ascii);
      break;
    }
    case MenuAction::GO_TO_PERCENT: {
      startActivityForResultWith<EpubReaderPercentSelectionActivity>(
          [this](const ActivityResult& result) {
            READING_STATS.resumeSession();
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const int percent = std::get<PercentResult>(result.data).percent;
            const int total = pdf ? pdf->pageCount() : 0;
            currentPage = total > 0 ? std::clamp(percent * total / 100, 0, total - 1) : 0;
            requestUpdate();
          },
          progressPercent());
      break;
    }
    case MenuAction::TOGGLE_BOOKMARK:
      toggleBookmark(currentPage);
      requestUpdate();
      break;
    case MenuAction::SCREENSHOT:
      pendingScreenshot = true;
      requestUpdate();
      break;
    case MenuAction::GO_HOME:
      onGoHome();
      break;
    case MenuAction::DELETE_CACHE:
      clearBookCache(bookPath);
      onGoHome();
      break;
    case MenuAction::ROTATE_SCREEN:
    default:
      // The menu already applied the orientation in-place; redraw the page.
      requestUpdate();
      break;
  }
}

void PdfReaderActivity::applyInitialOrientation() {
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
}

void PdfReaderActivity::drawStatusBar() const {
  if (!pdf) return;
  const auto sb = SETTINGS.statusBarSpec();
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0 || sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_HIDE) return;

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const bool bottom = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_BOTTOM;

  int clearY = marginTop;
  int paddingBottom = 0;
  if (bottom) {
    clearY = std::max(0, screenHeight - marginBottom - statusBarHeight - 4);
  } else {
    paddingBottom = screenHeight - statusBarHeight - marginBottom - marginTop - 4;
  }
  const int clearHeight = bottom ? screenHeight - marginBottom - clearY : statusBarHeight + 4;
  if (clearHeight > 0) renderer.fillRect(0, clearY, screenWidth, clearHeight, false);

  const float progress =
      pdf->pageCount() > 0 ? (currentPage + 1) * 100.0f / pdf->pageCount() : 0.0f;
  GUI.drawStatusBar(renderer, progress, currentPage + 1, pdf->pageCount(), getBookTitle(), paddingBottom);
}

void PdfReaderActivity::renderBook() {
#if defined(CROSSPOINT_PDF_XIP)
  if (engineProblem_ != 0) {
    renderer.clearScreen();
    // Naming the tap that fixes it, not just the failure: the card is otherwise
    // indistinguishable from the "leave" card for an unrecoverable refusal.
    const std::string downloadHint = std::string(engineProblemText()) + " " + tr(STR_TAP_TO_RETRY);
    status_card::paintMessage(renderer, tr(STR_PDF_ENGINE_UPDATE),
                              engineProblemIsDownloadable() ? downloadHint.c_str() : engineProblemText());
    renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
    return;
  }
#endif
  if (loadFailed_) {
    status_card::paintMessage(renderer, tr(STR_PDF_UNREADABLE), tr(STR_PAGE_LOAD_ERROR));
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }
  if (!pdf || currentPage < 0 || currentPage >= pdf->pageCount()) return;
  if (toolbarOpen && toolbarPageStored_ && toolbarStoredPage_ == currentPage) {
    // Panel cursor/tab movement: restore the clean page, re-snapshot it for
    // the next transition, paint the chrome and push one FAST frame.  No PDF
    // page render or second panel scan is involved.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    toolbarPageStored_ = renderer.storeBwBuffer();
    renderToolbarOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
    return;
  }
  if (toolbarOpen) discardToolbarPage();
  renderPage();
  if (wifiHintPending_ && !renderFailed_) {
    // Painted on the render task's own frame: the page underneath stays as the
    // background, and the 6 s dismissal in loop() re-renders it.
    // Real copy, not two reused strings: the first-run notice has to explain
    // *why* the radio went away and that it comes back (the reused pair read
    // "WiFi idle timeout / Not enough memory", which explains nothing).
    status_card::paintMessage(renderer, tr(STR_PDF_WIFI_OFF_TITLE), tr(STR_PDF_WIFI_OFF_BODY));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
  }
  saveProgress();
  // One prefetch page per frame, on the render task.  This used to run from
  // loop() -- the UI task -- where a heavy prefetch page froze input and serial
  // for up to 20 s each time it ran.  Same guards as before; the extra one is
  // that the page just shown must have rendered (don't prefetch around a card).
  if (!renderFailed_ && !reflowMode_ && !reflowBookEnabled_ && !fullCacheComplete_ && pdf &&
      !toolbarOpen && debugInitialPage < 0 && lastUsableWidth_ > 0) {
    ensureFullCache(lastUsableWidth_, lastPageHeight_);
    if (!fullCacheComplete_) requestUpdate();  // chain the next page
  }
  if (toolbarOpen) {
    toolbarPageStored_ = renderer.storeBwBuffer();
    toolbarStoredPage_ = currentPage;
    renderToolbarOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
  }
  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void PdfReaderActivity::runSelfTestStep() {
  if (!pdf) return;
  const int total = pdf->pageCount();
  if (selfTestPage_ >= total) {
    if (selfTestPage_ == total) {
      // Raw serial line, printed three times: LOG_ERR right at the end of a
      // long run is sometimes dropped in transit (see pdf-rendering.md).
      for (int i = 0; i < 3; ++i) {
        logSerial.printf("[PDF] SELFTEST pages=%d reflow=%d bitmap=%d failed=%d list=%s\n", total,
                         selfTestReflow_, selfTestBitmap_, selfTestFailed_, selfTestFailures_.c_str());
        delay(60);
      }
      selfTestPage_ = total + 1;  // print once
    }
    return;
  }

  const int page = selfTestPage_++;
  std::vector<std::string> paras;
  bool images = false;
  const bool hasText = extractPdfText(*pdf, page, paras, images) && !images;
  size_t chars = 0;
  if (hasText) {
    for (const auto& p : paras) chars += p.size();
  }
  if (hasText && chars >= 400) {
    ++selfTestReflow_;
    return;
  }

  const auto sb = SETTINGS.statusBarSpec();
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const bool showStatus = statusBarHeight > 0 && sb.xtcMode != CrossPointSettings::XTC_STATUS_BAR_HIDE;
  const bool statusTop = showStatus && sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_TOP;
  const bool statusBottom = showStatus && sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_BOTTOM;
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int screenMargin = SETTINGS.screenMargin;
  marginTop += screenMargin;
  marginRight += screenMargin;
  marginBottom += screenMargin;
  marginLeft += screenMargin;
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int usableWidth = std::max(1, screenWidth - marginLeft - marginRight);
  const int pageTop = marginTop + (statusTop ? statusBarHeight + 4 : 0);
  const int pageBottom = screenHeight - marginBottom - (statusBottom ? statusBarHeight + 4 : 0);
  const int pageHeight = std::max(1, pageBottom - pageTop);

  const std::string path = pageCachePath(page) + ".selftest.bmp";
  if (renderPdfPage(*pdf, page, path, usableWidth, pageHeight, false)) {
    ++selfTestBitmap_;
  } else {
    ++selfTestFailed_;
    if (selfTestFailures_.size() < 180) {
      selfTestFailures_ += std::to_string(page + 1);
      selfTestFailures_ += ',';
    }
  }
  Storage.remove(path.c_str());
}

#if defined(CROSSPOINT_PDF_XIP)
const char* PdfReaderActivity::engineProblemText() const {
  switch (pdf_module::decodeRefusal(engineProblem_)) {
    // Anything a matching image can fix reads as "needs an update"; the build
    // vocabulary stays in the serial log.
    case pdf_module::Refusal::Incompatible:
    case pdf_module::Refusal::NoPartition:
    case pdf_module::Refusal::BadHeader:
      return tr(STR_PDF_ENGINE_MISMATCH);
    // This boot's memory state: an engine download cannot help, a restart can.
    case pdf_module::Refusal::MapFailed:
    case pdf_module::Refusal::NoPsram:
      return tr(STR_MEMORY_ERROR);
    default:
      return tr(STR_PDF_ENGINE_NOT_FOUND);
  }
}

bool PdfReaderActivity::engineProblemIsDownloadable() const {
  return pdf_module::refusalIsDownloadable(pdf_module::decodeRefusal(engineProblem_));
}

void PdfReaderActivity::openEngineUpdate() {
  // The engine is bound to this firmware's ELF by SHA-256, so every app-only update orphans
  // it by design -- and until now the only way out was to find 设置 → PDF 引擎 by hand while
  // holding a book that would not open.  This is the same in-place remedy the EPUB reader
  // already offers for a loan whose date could not be verified (handleLoadFailure() →
  // 「同步时间」 → reopen): name the fix, and let the reader run it from where they are.
  startActivityForResultWith<PdfEngineUpdateActivity>([this](const ActivityResult&) {
    engineProblem_ = 0;
    if (!loadBook()) handleLoadFailure();
    requestUpdate();
  }, /*returnOnSuccess=*/true);
}
#endif

void PdfReaderActivity::onEnter() {
  // This reader never needs the network, and the Wi-Fi stack is the biggest
  // consumer of exactly what page rendering depends on: internal RAM and PSRAM
  // contiguity.  Measured with an idle STA association left over from another
  // feature, the reader sat at Free ~7 KB / MaxAlloc ~2.9 KB and every turn took
  // ~6 s; releasing the radio here gives the render back a normal heap.
  //
  // Safe from this point in the lifecycle: no socket of ours is open (every
  // network feature resets its own client on exit), and AP mode is left alone --
  // if a file transfer is serving, tearing the AP down would break it for no
  // benefit.  ★ onEnter() is NOT the UI task (renderTaskLoop on the push path),
  // which is one more reason to stay with the association-only teardown that is
  // proven on this hardware (AGENTS §85.13/§85.16).
  const wifi_mode_t mode = WiFi.getMode();
  if ((mode & WIFI_MODE_STA) != 0 && (mode & WIFI_MODE_AP) == 0) {
    wifiWasOn_ = 1;
    // ★ The deep teardown is what actually drops the radio; the light
    // releaseRadio() measured status=6 forever on device (disconnect did not
    // take).  The reader cannot run the deep one itself -- onEnter is on the
    // render task (PSRAM stack) and that path panicked once -- so it SCHEDULES
    // it and the main loop performs it on internal RAM.  The panic that looked
    // like "the teardown" was the engine-unmap cycle, which is now disabled.
    NetworkStartup::requestDeepRelease();
    LOG_INF("PDF", "deep radio release scheduled for reader entry (was STA); free=%u largest=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  }
  selectionHold_.reset();
  selectionTouchArmed_ = false;
  selectionDropTouch_ = true;
  selectionDropSinceMs_ = 0;  // the first UI loop starts the grace window
  ReaderActivity::onEnter();
}


pdf_reader_ui::Input PdfReaderActivity::readReaderInput(const pdf_reader_ui::State& ui) {
  // ★ Nothing may be read when no surface owns the frame: wasScreenTapped()
  // CONSUMES the tap, so calling it on a normal page ate the page-turn tap
  // before handleSelectionTouch()/the base reader could see it.  (Found by
  // re-reading the wiring after the table refactor -- the table itself was fine.)
  if (!ui.loadFailed && !ui.hintPending && !ui.renderFailed) return pdf_reader_ui::Input::None;
  mappedInput.update();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) return pdf_reader_ui::Input::Back;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return pdf_reader_ui::Input::Confirm;
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) return pdf_reader_ui::Input::Next;
  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) return pdf_reader_ui::Input::Prev;
  int tapX = 0, tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    if (!ui.renderFailed) return pdf_reader_ui::Input::TapOther;
    const pdf_fail_card::Layout pills{{failRetryX_, failRetryY_, failRetryW_, failRetryH_},
                                      {failSkipX_, failSkipY_, failSkipW_, failSkipH_}};
    switch (pdf_fail_card::hitTest(pills, tapX, tapY)) {
      case pdf_fail_card::Target::Retry: return pdf_reader_ui::Input::TapRetry;
      case pdf_fail_card::Target::Skip: return pdf_reader_ui::Input::TapSkip;
      default: return pdf_reader_ui::Input::TapOther;
    }
  }
  if (ui.hintPending && millis() - wifiHintShownMs_ > 10000) return pdf_reader_ui::Input::HintTimeout;
  return pdf_reader_ui::Input::None;
}

void PdfReaderActivity::loop() {
#if defined(CROSSPOINT_PDF_XIP)
  if (engineProblem_ != 0) {
    mappedInput.update();
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasScreenTapped(tapX, tapY) || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // A missing or stale engine is fixable from right here; anything else is
      // this boot's memory state, so the acknowledgement just leaves.
      if (engineProblemIsDownloadable()) {
        openEngineUpdate();
      } else {
        finish();
      }
    }
    return;
  }
#endif
  // Drop the contact left over from the tap that opened the book.  Loading a
  // reflow page takes seconds; by the time this loop runs the finger is long
  // gone, but the panel can still report it as held, and the shared hold timer
  // would then select the first word under that stale point.  Wait for a frame
  // with no contact (or a release) before letting selection see input.
  if (selectionDropTouch_) {
    int hx = 0;
    int hy = 0;
    const uint32_t now = millis();
    if (selectionDropSinceMs_ == 0) selectionDropSinceMs_ = now;
    const bool held = mappedInput.isScreenTouchHeld(hx, hy);
    const bool ended = mappedInput.wasScreenTouchReleased();
    if (!held || ended || static_cast<uint32_t>(now - selectionDropSinceMs_) > 800) {
      selectionHold_.reset();
      mappedInput.suppressScreenTouch();
      selectionDropTouch_ = false;
      LOG_DBG("PDF", "selection: dropped the contact left over from opening the book");
    } else {
      return;
    }
  }
  // Who owns this frame's input, and what the input does there: one table
  // (PdfReaderUi.h, host-tested) instead of three inline blocks whose ordering
  // had already caused two bugs.
  const pdf_reader_ui::State ui{loadFailed_, wifiHintPending_, renderFailed_, failEscalated_};
  // Decide once: reading the input twice would consume the same release edge.
  const pdf_reader_ui::Action action = pdf_reader_ui::decide(ui, readReaderInput(ui));
  const bool skipForward = action == pdf_reader_ui::Action::SkipForward;
  switch (action) {
    case pdf_reader_ui::Action::Swallow:
      return;
    case pdf_reader_ui::Action::DismissHint:
      wifiHintPending_ = false;
      requestUpdate();
      return;
    case pdf_reader_ui::Action::RetryPage:
      renderFailed_ = false;
      requestUpdate();  // opening the page on purpose clears its .failed marker
      return;
    case pdf_reader_ui::Action::SkipForward:
    case pdf_reader_ui::Action::SkipBack:
      // No page in that direction (a one-page file, or the last page): stay on
      // the card.  Dropping the card here would re-render the page that just
      // failed, which reads as "the button retried and broke again".
      if (pdf_reader_ui::afterSkip(pageTurn(skipForward)) == pdf_reader_ui::SkipResult::StayOnCard) {
        LOG_INF("PDF", "failure card: no page %s to skip to", skipForward ? "after" : "before");
        return;
      }
      requestUpdate();
      return;
    case pdf_reader_ui::Action::LeaveReader:
      LOG_INF("PDF", "leaving the reader from %s",
              loadFailed_ ? "a failed load" : renderFailed_ ? "the failure card" : "the notice");
      finish();
      return;
    case pdf_reader_ui::Action::Base:
      break;
  }
  if (wifiWasOn_ != 0 && SETTINGS.pdfWifiHintSeen == 0 && firstPagePresented_ && !wifiHintPending_) {
    // Say the side effect out loud the first time it happens (same one-shot
    // shape as the reader's gesture hint).  The radio really is off now, and a
    // user who came here from a download is owed that explanation.
    SETTINGS.pdfWifiHintSeen = 1;
    SETTINGS.saveToFile();  // loop() is the UI task; the render task must not touch NVS
    wifiHintPending_ = true;
    wifiHintShownMs_ = millis();
    // Only a state change here.  Drawing from loop() pushes a frame without the
    // render lock, and the first build of this hint did exactly that: the UI task
    // blocked inside status_card::showMessage, so serial commands and the [MEM]
    // heartbeat both stopped (a reader that looks dead with no reboot).  The
    // card is painted by renderBook(), on the frame it already owns.
    requestUpdate();
    LOG_INF("PDF", "wifi-off hint armed (radio was on; released for page rendering memory)");
  }
  if (handleSelectionTouch()) return;
  ReaderActivity::loop();
  pollReflowProbe();
  if (firstPagePresented_) startReflowProbe();
  if (debugSelfTest) {
    runSelfTestStep();
    return;
  }
}

void PdfReaderActivity::startReflowProbe() {
  if (reflowProbeStarted_ || !pdf || pdf->pageCount() <= 0) return;
  reflowProbeStarted_ = true;
  reflowProbePending_ = probePdfReflowAsync(*pdf);
  LOG_ERR("PDF", "reflow probe started%s", reflowProbePending_ ? "" : " (no worker)");
}

void PdfReaderActivity::pollReflowProbe() {
  if (!reflowProbePending_) return;
  if (!gReflowProbeJob.done.load()) return;
  reflowProbePending_ = false;
  const int usable = gReflowProbeJob.probeUsable;
  const int counted = gReflowProbeJob.probeCounted;
  // A page counts as usable when it yields a body's worth of text and draws no
  // figure/table: reflowing a mixed page would silently drop the figure.
  const bool enabled = counted > 0 && usable * 2 >= counted;
  LOG_ERR("PDF", "reflow support: %d/%d sampled pages usable -> %s", usable, counted, enabled ? "on" : "off");
  if (counted > 0) {
    HalFile decision;
    if (Storage.openFileForWrite("PDF", (cachePath_ + "/reflow.bin").c_str(), decision)) {
      const uint8_t value = enabled ? 1 : 0;
      decision.write(&value, 1);
      decision.close();
    }
  }
  if (enabled == reflowBookEnabled_) return;
  reflowBookEnabled_ = enabled;
  resetReflow();
  requestUpdate(true);
}

bool PdfReaderActivity::findQuoteLineRange(const std::string& quote, int& firstLine,
                                           int& lastLine) const {
  // The matching passes live in QuoteAnchor.h, where a host test can pin them
  // (test/quote_anchor/).
  bool relaxed = false;
  const bool found =
      quote_anchor::findLineRange(reflowLines_, quote, firstLine, lastLine, &relaxed);
  if (found && relaxed) {
    LOG_INF("PDF", "annotation re-anchored without spaces: lines %d..%d", firstLine, lastLine - 1);
  }
  return found;
}

bool PdfReaderActivity::annotationSpanOnLine(const size_t index, const int line, int& startByte,
                                             int& endByte, bool* isNoteOut) const {
  startByte = -1;
  endByte = -1;
  if (line < 0 || line >= static_cast<int>(reflowLines_.size())) return false;
  if (index >= pageNotes_.size()) return false;
  const annotations::Annotation& a = pageNotes_[index];
  if (a.spineIndex != static_cast<uint16_t>(currentPage)) return false;
  // Both kinds resolve.  Filtering Kind::Note out here made a selection note
  // COMPLETELY invisible on the page: it drew no rule, and nothing else marks it
  // (the corner square is the separate whole-page marker).  The EPUB reader underlines a
  // note exactly like a highlight -- its bubble is what distinguishes them, and that
  // affordance is still missing here (see docs/engineering/pdf-xip.md).
  if (isNoteOut) *isNoteOut = (a.kind == static_cast<uint8_t>(annotations::Kind::Note));
  const std::string& text = reflowLines_[static_cast<size_t>(line)];
  if (!a.quote.empty()) {
    // One matcher for drawing and for hit-testing, in QuoteAnchor.h: a whole-word search
    // that also reports where the quote starts and ends INSIDE the boundary lines.  The
    // fast path used to be a bare `text.find(quote)` here, so a quote like "and extra"
    // matched inside "band extraordinary" and underlined a word nobody selected; and a
    // multi-line quote painted its first and last line end to end instead of the exact
    // span, which is what the EPUB reader never does.
    // The stored line range is the tiebreaker: `a.startOffset` is the line the selection
    // started on, so a repeated sentence resolves to the passage the reader marked instead
    // of to whichever copy the page happens to hold first. Out-of-range means the record
    // predates this anchor shape (or is corrupt), and guessing from a stale number is worse
    // than the old first-match behaviour -- so fall back to no hint.
    const int hint = a.startOffset < reflowLines_.size() ? static_cast<int>(a.startOffset) : -1;
    const int matched = quote_anchor::spanForLine(reflowLines_, a.quote, line, startByte, endByte, hint);
    if (matched == quote_anchor::kHere) return true;
    if (matched == quote_anchor::kElsewhere) return false;
    // kAbsent: fall through to the stored line offsets, exactly as before.
  }
  if (line >= static_cast<int>(a.startOffset) && line < static_cast<int>(a.endOffset)) {
    startByte = 0;
    endByte = static_cast<int>(text.size());
    return true;
  }
  return false;
}

bool PdfReaderActivity::annotationLineRange(const std::string& quote, uint32_t& start,
                                            uint32_t& end) const {
  int firstLine = 0;
  int lastLine = 0;
  if (findQuoteLineRange(quote, firstLine, lastLine) && lastLine > firstLine) {
    start = static_cast<uint32_t>(firstLine);
    end = static_cast<uint32_t>(lastLine);
    return true;
  }
  // Not resolvable on this screen (a bitmap page, or a quote truncated away from
  // the page text): mark the whole page, the same range the page-note path uses.
  start = anno_anchor::kPageMarkerStart;
  end = anno_anchor::kPageMarkerEnd;
  return false;
}

bool PdfReaderActivity::lineHighlightSpan(const int line, int& startByte, int& endByte,
                                         bool* isNoteOut) const {
  if (isNoteOut) *isNoteOut = false;
  for (size_t i = 0; i < pageNotes_.size(); ++i) {
    if (annotationSpanOnLine(i, line, startByte, endByte, isNoteOut)) return true;
  }
  startByte = -1;
  endByte = -1;
  if (isNoteOut) *isNoteOut = false;
  return false;
}

bool PdfReaderActivity::pageHasLineHighlight(const int line) const {
  int start = 0;
  int end = 0;
  return lineHighlightSpan(line, start, end);
}

int PdfReaderActivity::annotationCoveringWord(const size_t wordIndex, int& firstWordOut,
                                              int& lastWordOut) const {
  // The interval logic lives in AnnotationAnchor.h so a host test can pin it;
  // this only supplies the per-word span, from the same matcher that draws the
  // underline, so drawing and hit-testing cannot drift apart.
  std::vector<anno_anchor::Word> words;
  words.reserve(reflowWords_.size());
  for (const auto& w : reflowWords_) {
    words.push_back(anno_anchor::Word{w.line, w.start, w.end});
  }
  const int annotation = anno_anchor::rangeForWord(
      words, wordIndex,
      [this](const size_t i) {
        const ReflowWord& w = reflowWords_[i];
        for (size_t a = 0; a < pageNotes_.size(); ++a) {
          int startByte = -1;
          int endByte = -1;
          if (!annotationSpanOnLine(a, static_cast<int>(w.line), startByte, endByte)) continue;
          if (anno_anchor::overlaps(w.start, w.end, startByte, endByte)) return static_cast<int>(a);
        }
        return -1;
      },
      firstWordOut, lastWordOut);
  return annotation;
}

void PdfReaderActivity::deleteSelectionAnnotation(const size_t index) {
  if (index >= pageNotes_.size()) return;
  pageNotes_.erase(pageNotes_.begin() + static_cast<std::ptrdiff_t>(index));
  annotations::normalize(pageNotes_);
  invalidateSelectionBase();  // the underline belongs to the page, not the overlay
  // No success toast: the highlight disappearing is the feedback, which is what
  // the EPUB reader does, and the EN i18n blob has no room for another string.
  if (!savePageNotes()) {
    LOG_ERR("PDF", "annotation delete not persisted");
    toast_.show(tr(STR_NOT_AVAILABLE), millis());
  }
}

void PdfReaderActivity::clearSelection() {
  selectionActive_ = false;
  selectionMenuActive_ = false;
  selectionMenuChoice_ = 0;
  selectionEditingIndex_ = -1;
  selectionDragging_ = false;
  selectionDragHandle_ = 0;
  selectionTouchArmed_ = false;
  selectionRange_ = {};
  selectionHandlesLogged_ = false;
  selectionAnchorLine_ = 0;
  selectionAnchorWord_ = 0;
  for (int i = 0; i < 2; ++i) selectionHandleW_[i] = 0;
}

// The selected text, joined with the same gap/glue rule the EPUB reader uses so
// a Latin quote keeps its inter-word spaces and a CJK run does not gain any.
// The words come from the current screen's layout, so the quote matches what is
// drawn. Built directly (not via joinSelection) because reflowLines_ substrings
// are not NUL-terminated at the word end.
std::string PdfReaderActivity::selectionQuote(size_t* anchorPrefixBytesOut) const {
  std::string out;
  bool first = true;
  bool anchorRecorded = false;
  uint32_t previousLast = 0;
  for (const auto& w : reflowWords_) {
    if (!selection_geom::covers(selectionRange_, w.line, w.word)) continue;
    const std::string& text = reflowLines_[static_cast<size_t>(w.line)];
    const auto start = static_cast<size_t>(w.start);
    const auto end = static_cast<size_t>(w.end);
    if (end > text.size() || start > end) continue;
    const std::string_view piece(text.data() + start, end - start);
    if (piece.empty()) continue;
    if (!first && w.gapBefore &&
        !(selection_text::gluesToNeighbour(previousLast) &&
          selection_text::gluesToNeighbour(selection_text::firstCodepoint(piece)))) {
      out.push_back(' ');
    }
    // The byte offset where the originally-pressed (anchor) word begins inside
    // the joined quote is the lookup anchor; mirrors EPUB's anchorPrefixBytes so
    // a dragged multi-word selection looks up the tapped word, not the range
    // head.  Captured right before the anchor word is appended.
    if (!anchorRecorded && w.line == selectionAnchorLine_ && w.word == selectionAnchorWord_) {
      if (anchorPrefixBytesOut) *anchorPrefixBytesOut = out.size();
      anchorRecorded = true;
    }
    out.append(piece);
    first = false;
    previousLast = selection_text::lastCodepoint(piece);
  }
  if (!anchorRecorded && anchorPrefixBytesOut) *anchorPrefixBytesOut = 0;
  return out;
}

void PdfReaderActivity::activateSelectionChoice(const int choice) {
  if (selectionEditingIndex_ >= 0) {
    // Editing an existing highlight: the bar offers Delete then Note, matching
    // the EPUB reader.  Deleting must also drop the selection, or the next
    // tap would still be inside a range that no longer exists.
    if (choice == 0) {
      const size_t index = static_cast<size_t>(selectionEditingIndex_);
      clearSelection();
      selectionClosedMs_ = millis();
      deleteSelectionAnnotation(index);
      requestUpdate();
      return;
    }
    promptSelectionNote();
    return;
  }
  if (choice == 0) {
    // The bar closes and the selection stays: exactly the state the EPUB reader
    // leaves after a lookup, so a phrase that was too short can be widened with
    // the keys and looked up again without starting over.
    selectionMenuActive_ = false;
    startPhraseLookup();
  } else if (choice == 1) {
    commitSelection(false);
  } else {
    promptSelectionNote();
  }
}

void PdfReaderActivity::widenSelectionTo(const uint16_t line, const uint16_t word, const int centerX) {
  // Takes the three fields the rule needs, not a rectangle type: the PDF page keeps
  // its own ReflowWord list and the EPUB reader keeps AnnoWordRect, and the shared
  // selection_geom helpers are templated for the same reason.
  // The anchored end never slides: whichever side the target falls on grows, and the
  // anchor moves to the far (fixed) end so the next press extends from it. Same rule
  // as the EPUB reader, in the shared geometry.
  const bool growStart = selection_geom::before(line, word, selectionAnchorLine_, selectionAnchorWord_);
  selection_geom::extendTo(selectionRange_, selectionAnchorLine_, selectionAnchorWord_, line, word);
  if (growStart) {
    selectionAnchorLine_ = selectionRange_.endLine;
    selectionAnchorWord_ = selectionRange_.endWord;
  } else {
    selectionAnchorLine_ = selectionRange_.startLine;
    selectionAnchorWord_ = selectionRange_.startWord;
  }
  selectionAnchorX_ = static_cast<int16_t>(centerX);
  LOG_DBG("PDF", "selection: widen to line %u word %u", line, word);
  requestUpdate();
}

void PdfReaderActivity::showSelectionMenu() {
  if (!selectionActive_) return;
  selectionMenuActive_ = true;
  selectionMenuChoice_ = 0;
  selectionMenuOpenedMs_ = millis();
  requestUpdate();
}

void PdfReaderActivity::beginSelection(const int x, const int y) {
  if (!reflowMode_ || reflowLines_.empty() || reflowScreen_ < 0 ||
      reflowScreen_ >= static_cast<int>(reflowScreenStarts_.size())) {
    return;
  }
  // reflowWords_ was filled by the last renderReflow() of this exact screen, so
  // the rectangles are the drawn ones. A press anywhere snaps to the nearest
  // word (selection_geom::nearest) rather than silently doing nothing.
  if (reflowWords_.empty()) return;
  const int hit = selection_geom::nearest(reflowWords_, x, y);
  if (hit < 0) return;
  const auto& w = reflowWords_[static_cast<size_t>(hit)];
  // Long-pressing inside an existing highlight re-selects its whole range and
  // turns the first chip into Delete.  Without this, a highlight created from
  // the selection bar could never be removed: it carries no offsets, so the
  // line-range delete path could not see it either.
  int firstWord = -1;
  int lastWord = -1;
  selectionEditingIndex_ = annotationCoveringWord(static_cast<size_t>(hit), firstWord, lastWord);
  if (selectionEditingIndex_ >= 0 && firstWord >= 0) {
    const auto& first = reflowWords_[static_cast<size_t>(firstWord)];
    const auto& last = reflowWords_[static_cast<size_t>(lastWord)];
    selectionRange_ = {first.line, first.word, last.line, last.word};
  } else {
    selectionRange_ = {w.line, w.word, w.line, w.word};
  }
  selectionAnchorLine_ = w.line;
  selectionAnchorWord_ = w.word;
  selectionAnchorX_ = static_cast<int16_t>(w.x + w.w / 2);
  selectionActive_ = true;
  selectionClosedMs_ = 0;
  showSelectionMenu();
}

void PdfReaderActivity::applySmartLineMerge(const bool on) {
  SETTINGS.pdfSmartLineMerge = on ? 1 : 0;
  // Tell the open document as well as the app-side merge: the split happens while the
  // engine extracts text, so the re-extract below only reflects the new value if the engine
  // heard about it first.  This is also why CMD:PDFMERGE calls THIS function -- the bench
  // hook used to write SETTINGS and stop, which left the running engine splitting paragraphs
  // behind an option that had been switched off, so a serial A/B measured a state no user
  // can ever be in.
  if (pdf) pdf->setParagraphSplit(on);
  // The shipped default is on, and a default is only verifiable if the value is observable,
  // not just the change.
  LOG_INF("PDF", "smart line merge -> %d (engine notified=%d)", static_cast<int>(on), pdf ? 1 : 0);
  SETTINGS.saveToFile();
  resetReflow();
  requestUpdate();
}

// CMD:PDFENGINE: the engine update story rests on the installed engine and the running
// firmware being the paired build; until now the only evidence of that on the device was
// "PDF works" or "PDF does not".
void PdfReaderActivity::debugEngineStatus() {
  uint8_t tag[32] = {};
  const bool haveTag = pdf_module::runningBuildId(tag);
  char hex[14] = {};
  static const char digits[] = "0123456789abcdef";
  if (haveTag) {
    for (int i = 0; i < 6; ++i) {
      hex[i * 2] = digits[tag[i] >> 4];
      hex[i * 2 + 1] = digits[tag[i] & 0x0F];
    }
    hex[12] = 0;
  }
  LOG_ERR("PDF", "engine status: loaded=%d abi=%u appHostApi=%u.%u layout=0x%08x buildId=%s open=%u",
          static_cast<int>(pdf_module::ops() != nullptr), static_cast<unsigned>(pdf_module::loadedAbi()),
          static_cast<unsigned>(kPdfModuleHostApiMajor), static_cast<unsigned>(kPdfModuleHostApiMinor),
          static_cast<unsigned>(kPdfModuleLayoutKey), haveTag ? hex : "(none)",
          static_cast<unsigned>(pdf_module::openDocuments()));
}

void PdfReaderActivity::debugSelectionAction(const int choice) {
  // 100..103 widen the selection the way the four direction keys do, through the
  // same shared step rule. This is the console path for the state a dictionary
  // lookup leaves the selection in (bar closed, range alive), which the chip codes
  // below deliberately refuse.
  if (choice >= 100 && choice <= 103) {
    if (!selectionActive_) {
      LOG_INF("PDF", "sel widen ignored: no selection");
      return;
    }
    const selection_geom::KeyStep step = static_cast<selection_geom::KeyStep>(choice - 100);
    const int focus = selection_geom::indexOf(reflowWords_, selectionRange_.endLine, selectionRange_.endWord);
    const int next = selection_geom::stepWord(reflowWords_, focus, step, selectionAnchorX_);
    LOG_INF("PDF", "sel widen: step=%d focus=%d next=%d range=[%u:%u..%u:%u] words=%u", choice - 100, focus, next,
            selectionRange_.startLine, selectionRange_.startWord, selectionRange_.endLine, selectionRange_.endWord,
            static_cast<unsigned>(reflowWords_.size()));
    if (next >= 0) {
      const auto& w = reflowWords_[static_cast<size_t>(next)];
      widenSelectionTo(w.line, w.word, w.x + w.w / 2);
    }
    return;
  }
  if (!selectionMenuActive_) {
    LOG_INF("PDF", "sel action ignored: no action bar open");
    return;
  }
  const int chipCount = selectionEditingIndex_ >= 0 ? 2 : 3;
  LOG_INF("PDF", "sel action: chip=%d of %d editing=%d", choice, chipCount, selectionEditingIndex_);
  if (choice < 0 || choice >= chipCount) return;
  activateSelectionChoice(choice);
}

// CMD:PDFSEL bench hook: resolve a word at (x,y) through the shared geometry and
// log the range + captured quote, so the PDF selection path is verifiable from
// the serial console against the real reflow words (no finger required).  When a
// second point (dragX,dragY >= 0) is given it also drags the END handle there and
// logs the widened range + the anchor prefix, exercising the drag clamp and the
// EPUB-parity lookup anchor on real words.
void PdfReaderActivity::debugSelectionAt(const int x, const int y, const int dragX, const int dragY,
                                       const int dragHandle) {
  if (reflowWords_.empty()) {
    // Say which state we are actually in. "bitmap page / not reflowed" was a guess
    // written when this line had one cause; with the reader resuming mid-document,
    // in a reflowed file, on a screen with no text, it was indistinguishable from a
    // real failure -- and a bench line nobody can interpret is how a harness starts
    // being ignored.
    LOG_INF("PDF", "sel probe: no reflow words (mode=%s screen=%d/%d lines=%u)",
            reflowMode_ ? "reflow" : "paged", static_cast<int>(reflowScreen_),
            static_cast<int>(reflowScreens_), static_cast<unsigned>(reflowLines_.size()));
    return;
  }
  beginSelection(x, y);
  if (!selectionActive_) {
    // Report where the text actually is, so a bench coordinate can be chosen (or a
    // stale one recognized as stale) without a screenshot.
    int minX = reflowWords_.front().x, maxX = minX;
    int minY = reflowWords_.front().y, maxY = minY;
    for (const auto& word : reflowWords_) {
      minX = std::min(minX, static_cast<int>(word.x));
      maxX = std::max(maxX, static_cast<int>(word.x + word.w));
      minY = std::min(minY, static_cast<int>(word.y));
      maxY = std::max(maxY, static_cast<int>(word.y + word.h));
    }
    LOG_INF("PDF", "sel probe: no word at %d,%d (words=%u text spans x=%d..%d y=%d..%d)", x, y,
            static_cast<unsigned>(reflowWords_.size()), minX, maxX, minY, maxY);
    return;
  }
  if (dragX >= 0 && dragY >= 0 && (dragHandle == 1 || dragHandle == 2)) {
    // Drive the production path: this is the same function the finger reaches
    // through handleSelectionHandles(), so a bench result means the same thing.
    selectionDragHandle_ = dragHandle;
    updateSelectionDragTo(dragX, dragY);
    selectionDragHandle_ = 0;
  }
  size_t anchorPrefixBytes = 0;
  std::string quote = selectionQuote(&anchorPrefixBytes);
  if (quote.size() > 48) quote.resize(48);  // UTF-8 may split a codepoint; bench only
  // `editing=` is the proof that a press inside an existing highlight re-selected
  // its range instead of just one word: the chip bar then shows Delete/Note, and
  // the range widens from one word to the whole annotation.
  // `anchor=` names the pressed word explicitly.  Without it a bench script can
  // only infer the anchor from `anchorBytes`, and inference is what made an
  // earlier assertion wrong in both directions: it failed on correct firmware
  // (a forward drag legitimately has the anchor at the range head) and would
  // have passed on a firmware that always looked up the range head.  With the
  // coordinate printed, the script can assert the real rule: the anchor is the
  // word that was pressed, wherever the drag took the other handle, and
  // `anchorBytes` is 0 exactly when that word is the range head.
  LOG_INF("PDF",
          "sel probe: words=%u range=[%u:%u..%u:%u] anchor=%u:%u anchorBytes=%u editing=%d drag=%d "
          "quote=\"%s\"",
          static_cast<unsigned>(reflowWords_.size()), static_cast<unsigned>(selectionRange_.startLine),
          static_cast<unsigned>(selectionRange_.startWord), static_cast<unsigned>(selectionRange_.endLine),
          static_cast<unsigned>(selectionRange_.endWord), static_cast<unsigned>(selectionAnchorLine_),
          static_cast<unsigned>(selectionAnchorWord_), static_cast<unsigned>(anchorPrefixBytes),
          selectionEditingIndex_, dragX >= 0 ? dragHandle : 0, quote.c_str());
}

// Move whichever end the finger is dragging, clamped by the shared geometry so
// the start never crosses the end. Mirrors the EPUB reader's drag rule exactly.
void PdfReaderActivity::updateSelectionDragTo(const int x, const int y) {
  if (!selectionActive_ || selectionDragHandle_ == 0 || reflowWords_.empty()) return;
  // A handle press is 24 px wide and the word it belongs to is 4 px away; without
  // this gate a plain grab would snap the range to the nearest word under the
  // handle's centre before the finger has moved at all.
  if (std::abs(x - selectionDragGrabX_) < 3 && std::abs(y - selectionDragGrabY_) < 3) return;
  const int hit = selection_geom::nearest(reflowWords_, x, y);
  if (hit < 0) return;
  const auto& w = reflowWords_[static_cast<size_t>(hit)];
  if (selectionDragHandle_ == 1) {
    if (!selection_geom::moveStartTo(selectionRange_, w.line, w.word)) return;
  } else {
    if (!selection_geom::moveEndTo(selectionRange_, w.line, w.word)) return;
  }
  // Keep the anchor the key-widening rule needs pointing at the end that did NOT
  // move, and at this word's column -- the EPUB reader maintains the same pair here,
  // otherwise a drag followed by a key press would widen from a stale anchor.
  selectionAnchorLine_ = selectionDragHandle_ == 1 ? selectionRange_.endLine : selectionRange_.startLine;
  selectionAnchorWord_ = selectionDragHandle_ == 1 ? selectionRange_.endWord : selectionRange_.startWord;
  selectionAnchorX_ = static_cast<int16_t>(w.x + w.w / 2);
  LOG_DBG("PDF", "selection: drag handle %d -> line %u word %u (range %u:%u..%u:%u)", selectionDragHandle_,
          static_cast<unsigned>(w.line), static_cast<unsigned>(w.word),
          static_cast<unsigned>(selectionRange_.startLine), static_cast<unsigned>(selectionRange_.startWord),
          static_cast<unsigned>(selectionRange_.endLine), static_cast<unsigned>(selectionRange_.endWord));
  const uint32_t now = millis();
  if (!selection_hold::repaintDue(now, selectionLastRedrawMs_)) return;
  selectionLastRedrawMs_ = now;
  requestUpdate();
}

bool PdfReaderActivity::handleSelectionTouch() {
  if (selectionActive_) {
    // A selection that just closed must not start a new one under the same
    // finger (the tap that confirmed Highlight would otherwise re-open it).
    if (millis() - selectionClosedMs_ < selection_hold::kClosedGuardMs) return true;
    // Handles first: dragging must keep working while the action bar is up.
    if (handleSelectionHandles()) return true;
    if (selectionMenuActive_) {
      // Two chips while editing an existing annotation (Delete, Note), three
      // for a fresh selection (Lookup, Highlight, Note) -- the EPUB reader's
      // shape, and the reason the chip count cannot be a literal here.
      const int chipCount = selectionEditingIndex_ >= 0 ? 2 : 3;
      // Checked first, like the EPUB reader: cancelling must work on the very
      // first press, even one that lands inside the gesture that opened the bar.
      // Without this the bar could only be left by choosing a chip or tapping away.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        clearSelection();
        selectionClosedMs_ = millis();
        requestUpdate();
        return true;
      }
      // The release of the gesture that opened this bar arrives as a screen tap.  The EPUB
      // reader swallowed it for 250 ms; this reader acted on it, so a long press could land
      // straight on chip 0 while the bar was still appearing.  Back above stays exempt on
      // purpose -- cancelling must work on the very first press.
      if (!selection_hold::barAcceptsInput(millis(), selectionMenuOpenedMs_)) return true;
      int tx = 0;
      int ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty)) {
        const int hit = selection_bar::hitTest(selectionBars_, chipCount, tx, ty);
        if (hit >= 0) {
          activateSelectionChoice(hit);
        } else {
          LOG_DBG("PDF", "selection: tap outside the bar clears it");
          clearSelection();
          selectionClosedMs_ = millis();
          requestUpdate();
        }
        return true;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        activateSelectionChoice(std::min(selectionMenuChoice_, chipCount - 1));
        return true;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        selectionMenuChoice_ = (selectionMenuChoice_ + chipCount - 1) % chipCount;
        requestUpdate();
        return true;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        selectionMenuChoice_ = (selectionMenuChoice_ + 1) % chipCount;
        requestUpdate();
        return true;
      }
      return true;
    }
    // Bar closed, selection still open -- the state a dictionary lookup leaves.
    // The keys widen it here (the shared rule), a tap or Back ends it, and Confirm
    // brings the bar back. Nothing may fall through: the old code cleared the
    // selection on the first frame that was not a chip tap.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      clearSelection();
      selectionClosedMs_ = millis();
      requestUpdate();
      return true;
    }
    int sx = 0;
    int sy = 0;
    if (mappedInput.wasScreenTapped(sx, sy)) {
      clearSelection();
      selectionClosedMs_ = millis();
      requestUpdate();
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      showSelectionMenu();
      return true;
    }
    // Read every key once: wasReleased() is an edge query, so calling it twice in a
    // condition chain is not safe.
    const bool keyLeft = mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool keyRight = mappedInput.wasReleased(MappedInputManager::Button::Right);
    const bool keyUp = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool keyDown = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (!keyLeft && !keyRight && !keyUp && !keyDown) return true;
    const selection_geom::KeyStep step = keyLeft   ? selection_geom::KeyStep::Left
                                         : keyRight ? selection_geom::KeyStep::Right
                                         : keyUp    ? selection_geom::KeyStep::Up
                                                    : selection_geom::KeyStep::Down;
    const int focus = selection_geom::indexOf(reflowWords_, selectionRange_.endLine, selectionRange_.endWord);
    const int next = selection_geom::stepWord(reflowWords_, focus, step, selectionAnchorX_);
    // -1 is both a page edge and a word the reflow no longer contains after a
    // rebuild; moving to index 0 there would yank the selection to the page start.
    if (next >= 0) {
      const auto& w = reflowWords_[static_cast<size_t>(next)];
      widenSelectionTo(w.line, w.word, w.x + w.w / 2);
    }
    return true;
  }
  if (toolbarOpen || !reflowMode_) {
    selectionHold_.reset();
    return false;
  }

  // One judgement, in the shared tracker, exactly as the EPUB reader makes it --
  // including the two branches this reader used to lack: a single frame without a
  // touch sample is a dropped reading, not a lift, and a finger the panel stopped
  // reporting still fires at its last known point.  Without those, a long press on
  // this hardware silently did nothing here while it worked in the EPUB reader.
  int x = 0;
  int y = 0;
  int downX = 0;
  int downY = 0;
  int tapX = 0;
  int tapY = 0;
  const bool down = mappedInput.wasScreenTouchDown(downX, downY);
  if (down) selectionTouchArmed_ = true;
  const bool held = mappedInput.isScreenTouchHeld(x, y);
  const bool ended = mappedInput.wasScreenTouchReleased() || mappedInput.wasScreenTapped(tapX, tapY);
  if (ended) selectionTouchArmed_ = false;
  // A held contact with no down edge in this activity is the ghost left by the
  // tap that opened the book; do not start a timer on it.  Once a real press is
  // tracking, keep feeding it even if the down edge came on an earlier frame.
  const bool allowHeld = selectionTouchArmed_ || selectionHold_.tracking();
  const selection_hold::Contact contact{/*ended=*/ended, /*held=*/held && allowHeld,
                                        down ? downX : x, down ? downY : y};
  const uint32_t now = millis();
  const selection_hold::Verdict verdict = selectionHold_.update(contact, now);
  if (verdict == selection_hold::Verdict::CancelledLift) {
    LOG_DBG("PDF", "selection: press ended by release");
    return false;
  }
  if (verdict == selection_hold::Verdict::CancelledTravel) {
    LOG_DBG("PDF", "selection: press cancelled (travel)");
    return false;
  }
  if (verdict != selection_hold::Verdict::Firing) return false;
  LOG_DBG("PDF", "selection: hold fired %d,%d", selectionHold_.fireX(), selectionHold_.fireY());
  beginSelection(selectionHold_.fireX(), selectionHold_.fireY());
  // Consume the contact once the timer fired, even when no word was found:
  // otherwise the still-down finger restarts the timer and the "no word" path
  // repeats every kPressMs.  A deliberate hold on blank space doing nothing is
  // safer than a surprise page turn on the lift.
  mappedInput.suppressScreenTouch();
  if (selectionActive_) {
    selectionDragging_ = false;
    selectionDragHandle_ = 0;
    selectionLastRedrawMs_ = now;
  }
  return true;  // consume the hold even when no word was found
}

// The two endpoint handles are the only way to change the range. A press that
// lands on a handle starts a drag; a press elsewhere is left to the action-bar
// / clear logic (a drag off-handle does nothing, matching the EPUB reader).
bool PdfReaderActivity::handleSelectionHandles() {
  int hx = 0;
  int hy = 0;
  bool held = mappedInput.isScreenTouchHeld(hx, hy);
  // A fast touch-down can be reported as a down edge in a frame where the held
  // query is already false again (the panel samples faster than the UI loop).
  // Accept that edge as the start of a handle drag, or the handles look dead to
  // a quick press even though they are drawn.
  if (!held && !selectionDragging_ && mappedInput.wasScreenTouchDown(hx, hy)) held = true;
  if (!held) {
    selectionDragging_ = false;
    selectionDragHandle_ = 0;
    return false;
  }
  if (!selectionDragging_) {
    selection_geom::Handles handles;
    for (int i = 0; i < 2; ++i) {
      handles.x[i] = selectionHandleX_[i];
      handles.y[i] = selectionHandleY_[i];
      handles.w[i] = selectionHandleW_[i];
      handles.h[i] = selectionHandleH_[i];
    }
    const int hit = selection_geom::hitHandle(handles, hx, hy);
    if (hit >= 0) {
      selectionDragging_ = true;
      selectionDragHandle_ = hit + 1;
      selectionDragGrabX_ = static_cast<int16_t>(hx);
      selectionDragGrabY_ = static_cast<int16_t>(hy);
      selectionLastRedrawMs_ = millis();
      LOG_DBG("PDF", "selection: drag handle %d at %d,%d", hit + 1, hx, hy);
      return true;
    }
    return false;  // not on a handle: let the action-bar logic run
  }
  updateSelectionDragTo(hx, hy);
  return true;
}

void PdfReaderActivity::commitSelection(const bool asNote) {
  if (!selectionActive_) {
    clearSelection();
    requestUpdate();
    return;
  }
  const std::string quote = annotations::clampUtf8(selectionQuote(), annotations::kMaxQuoteBytes);
  if (quote.empty()) {
    clearSelection();
    requestUpdate();
    return;
  }
  annotations::Annotation mark{};
  mark.spineIndex = static_cast<uint16_t>(currentPage);
  // Line indices, not 0/0: two 0/0 records on one page are indistinguishable to
  // annotations::normalize() and one of them was silently dropped.
  annotationLineRange(quote, mark.startOffset, mark.endOffset);
  mark.kind = static_cast<uint8_t>(asNote ? annotations::Kind::Note : annotations::Kind::Highlight);
  mark.quote = quote;
  mark.chapterTitle = annotations::clampUtf8(getBookTitle(), annotations::kMaxTitleBytes);
  pageNotes_.push_back(std::move(mark));
  annotations::normalize(pageNotes_);
  savePageNotes();
  invalidateSelectionBase();  // next frame is a full page: the underline is permanent
  clearSelection();
  selectionClosedMs_ = millis();
  requestUpdate();
}

void PdfReaderActivity::startPhraseLookup() {
  if (!selectionActive_) {
    clearSelection();
    requestUpdate();
    return;
  }
  std::string dictionaryBase;
  if (!DictionaryRegistry::resolveBasePath(SETTINGS.dictionaryName, dictionaryBase)) {
    // The name survives in settings after the folder is deleted, so say which
    // guard fired rather than leaving the capsule as the only clue.
    LOG_INF("PDF", "lookup declined: no usable dictionary behind the saved name");
    clearSelection();
    selectionClosedMs_ = millis();
    toast_.show(tr(STR_DICT_NO_DICT_SET), millis());
    requestUpdate();
    return;
  }
  size_t anchorPrefixBytes = 0;
  const std::string raw = selectionQuote(&anchorPrefixBytes);
  selection_text::PhraseQuery query;
  query.text = selection_text::normalizePhrase(raw);
  if (!selection_text::hasLookupContent(query.text)) {
    // Length only: the selection itself is book text and is never logged.
    LOG_INF("PDF", "phrase: nothing to look up (%u bytes raw)", static_cast<unsigned>(raw.size()));
    clearSelection();
    selectionClosedMs_ = millis();
    toast_.show(tr(STR_NOT_AVAILABLE), millis());
    requestUpdate();
    return;
  }
  // Look up from the pressed word within a dragged multi-word selection, the
  // same anchor EPUB computes (normalize the raw prefix before measuring).
  const std::string prefix =
      selection_text::normalizePhrase(std::string_view(raw).substr(0, anchorPrefixBytes));
  query.anchorOffset = std::min(prefix.size(), query.text.size());
  // Sizes only -- the phrase is book text, not a diagnostic. Without this line
  // a declined lookup is indistinguishable on the console from a swallowed
  // keypress, which cost a full debugging round.
  LOG_INF("PDF", "phrase: %u bytes, anchor at %u", static_cast<unsigned>(query.text.size()),
          static_cast<unsigned>(query.anchorOffset));
  clearSelection();
  selectionClosedMs_ = millis();
  const bool started = startActivityForResultWith<DictionaryWordSelectActivity>(
      [this](const ActivityResult&) {
        READING_STATS.resumeSession();
        requestUpdate();
      },
      std::move(query));
  if (!started) {
    toast_.show(tr(STR_NOT_AVAILABLE), millis());
    requestUpdate();
  }
}

void PdfReaderActivity::promptSelectionNote() {
  if (!selectionActive_) {
    clearSelection();
    requestUpdate();
    return;
  }
  // Editing an existing annotation starts from its own text. An empty field would
  // silently discard what the reader wrote earlier, and they cannot see it from here.
  std::string existing;
  if (selectionEditingIndex_ >= 0 &&
      static_cast<size_t>(selectionEditingIndex_) < pageNotes_.size()) {
    existing = pageNotes_[static_cast<size_t>(selectionEditingIndex_)].note;
  }
  const std::string quote = annotations::clampUtf8(selectionQuote(), annotations::kMaxQuoteBytes);
  // Resolved now, while this page's lines are on screen: the keyboard callback runs
  // later, and the range must describe the page the quote was taken from.
  uint32_t markStart = 0;
  uint32_t markEnd = 0;
  annotationLineRange(quote, markStart, markEnd);
  clearSelection();
  selectionClosedMs_ = millis();
  startActivityForResultWith<KeyboardEntryActivity>(
      [this, quote, markStart, markEnd](const ActivityResult& result) {
        READING_STATS.resumeSession();
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        std::string text = annotations::clampUtf8(std::get<KeyboardResult>(result.data).text, annotations::kMaxNoteBytes);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
        if (text.empty()) {
          requestUpdate();
          return;
        }
        annotations::Annotation mark{};
        mark.spineIndex = static_cast<uint16_t>(currentPage);
        mark.startOffset = markStart;
        mark.endOffset = markEnd;
        mark.kind = static_cast<uint8_t>(annotations::Kind::Note);
        mark.quote = quote;
        mark.note = std::move(text);
        mark.chapterTitle = annotations::clampUtf8(getBookTitle(), annotations::kMaxTitleBytes);
        pageNotes_.push_back(std::move(mark));
        annotations::normalize(pageNotes_);
        savePageNotes();
        requestUpdate();
      },
      tr(STR_PDF_PAGE_NOTE), existing, annotations::kMaxNoteBytes, InputType::Text);
}

void PdfReaderActivity::debugOpenPanel(const int tool) {
  if (!pdf) return;
  if (tool == 4) {
    beginSelection(renderer.getScreenWidth() / 2, renderer.getScreenHeight() / 3);
    return;
  }
  if (tool == 5) {
    commitSelection(false);
    return;
  }
  if (!toolbarOpen) openToolbar();
  if (!toolbarOpen || !usesToolbarMenu()) return;
  focusedTool = std::clamp(tool, 0, 3);
  toolbarPanel = true;
  toolbarUi->nav().reset(0);
  requestUpdate();
}

void PdfReaderActivity::debugMoveRing(const int direction) {
  if (!toolbarOpen || !toolbarPanel || !toolbarUi) return;
  const int count = toolbarItemCount();
  if (count <= 0) return;
  const int next = std::clamp(toolbarUi->nav().selected.load() + direction, 0, count - 1);
  toolbarUi->nav().reset(next);
  requestUpdate();
}

void PdfReaderActivity::debugConfirm() {
  if (!toolbarOpen) {
    openToolbar();
    return;
  }
  if (!toolbarPanel) {
    toolbarPanel = true;
    toolbarUi->nav().reset(0);
    requestUpdate();
    return;
  }
  activateToolbarRow(toolbarUi->nav().selected.load());
}

void PdfReaderActivity::debugBack() {
  if (!isAtEndOfBook()) return;
  // Exactly what the end-of-book menu does on a short Back press.
  onReturnFromEndOfBook();
  requestUpdate();
}

void PdfReaderActivity::resetReflow() {
  clearSelection();
  invalidateSelectionBase();
  reflowMode_ = false;
  reflowPage_ = -1;
  reflowScreen_ = 0;
  reflowScreens_ = 0;
  reflowLines_.clear();
  reflowLineIndent_.clear();
  reflowLineExtra_.clear();
  reflowLineParaEnd_.clear();
  reflowLineFont_.clear();
  reflowScreenStarts_.clear();
  reflowWords_.clear();
}

void PdfReaderActivity::buildReflow(const int usableWidth, const int pageHeight, const bool allowMixed) {
  // Consumed here even on rejection paths: a stale request must never fling a
  // later, ordinary page to its bottom.
  const bool landAtEnd = reflowEndScreenPending_;
  reflowEndScreenPending_ = false;
  resetReflow();
  reflowPage_ = currentPage;
  if (!pdf || !reflowBookEnabled_ || usableWidth <= 0 || pageHeight <= 0) return;

  std::vector<std::string> paragraphs;
  bool hasImages = false;
  if (!extractPdfText(*pdf, currentPage, paragraphs, hasImages)) return;
  // A page with a figure or table keeps the bitmap path: reflowing would drop
  // the figure, and the user's rule is "text pages reflow, everything else
  // stays a picture".
  if (hasImages && !allowMixed) return;
  // PDF-specific typography option.  extractText() already joins the lines of a
  // single MuPDF block (and de-hyphenates inside it), so a visual break that
  // survives to here means MuPDF split those lines into SEPARATE blocks -- the
  // OCR / hard-wrapped case.  pdf_merge rejoins across those paragraph entries
  // (de-hyphenating, and treating a blank entry as a real paragraph break).  On by
  // default; a well-formed one-block-per-paragraph extraction is unchanged, and a
  // short line (poetry, a table of contents) still counts as a break instead of
  // getting glued to its neighbour.
  if (SETTINGS.pdfSmartLineMerge) {
    const size_t before = paragraphs.size();
    paragraphs = pdf_merge::merge(paragraphs, pdf_merge::Options{});
    // Makes the effect measurable on a real page (and testable from the serial
    // console via CMD:PDFMERGE without a finger): how many separate MuPDF blocks
    // collapsed into how many rejoined paragraphs.
    LOG_INF("PDF", "reflow merge: %u blocks -> %u paragraphs", static_cast<unsigned>(before),
            static_cast<unsigned>(paragraphs.size()));
  }

  // Reflow draws with the SD reader family, which the bitmap/MuPDF path
  // released. Reload it only now: extraction needed that memory.
  sdFontSystem.ensureLoaded(renderer);
  const int fontId = reflowFontId();
  // Resolve one face per paragraph from the whole paragraph text.  Measuring
  // each candidate/character against the raw role id lets resolveTextFontId
  // pick a different fallback (and width) for a one-character string, which
  // makes reflow lines overflow the screen.
  int pageFont = fontId;
  int lineHeight = std::max(1, renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression()));
  int indentPx = 0;
  if (!paragraphs.empty()) {
    pageFont = renderer.chooseFontForText(fontId, paragraphs.front().c_str());
    lineHeight = std::max(1, renderer.getLineHeight(pageFont, SETTINGS.getReaderLineCompression()));
  }
  if (SETTINGS.firstLineIndent == FirstLineIndent::Indent) {
    indentPx = renderer.getSpaceWidth(pageFont) * SETTINGS.paragraphIndentSpaces + SETTINGS.firstLineIndentPx;
    indentPx = std::max(0, std::min(indentPx, usableWidth / 2));
  }
  static constexpr float kSpacingFactors[] = {0.0f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f};
  const int paragraphGap =
      SETTINGS.extraParagraphSpacing < sizeof(kSpacingFactors) / sizeof(kSpacingFactors[0])
          ? static_cast<int>(lineHeight * kSpacingFactors[SETTINGS.extraParagraphSpacing])
          : static_cast<int>(lineHeight * 1.5f);
  LOG_ERR("PDF", "reflow layout: font=%d width=%d height=%d margin=%d line=%d indent=%d paraGap=%d",
          fontId, usableWidth, pageHeight, SETTINGS.screenMargin, lineHeight, indentPx, paragraphGap);
  // PDFs rarely expose a language for reflow.  loadBook() warms the English
  // Liang patterns on the UI task; this render task only consults the cached
  // hyphenator.  CJK still breaks by character.
  for (const auto& para : paragraphs) {
    // A paragraph of only spaces / U+3000 / zero-width characters would otherwise be
    // laid out as a blank line -- the "多段连续空行" shape OCR leaves behind.  Checked
    // here as well as in pdf_merge so the drawing is right even with 智能合并断行 off.
    if (utf8IsBlankText(para)) continue;
    const int paraFont = renderer.chooseFontForText(fontId, para.c_str());
    std::string line;
    bool firstLine = true;
    auto flush = [&](const bool paragraphEnd) {
      if (line.empty()) return;
      reflowLines_.push_back(line);
      reflowLineIndent_.push_back(static_cast<int16_t>(firstLine ? indentPx : 0));
      reflowLineExtra_.push_back(static_cast<int16_t>(paragraphEnd ? paragraphGap : 0));
      reflowLineParaEnd_.push_back(paragraphEnd ? 1 : 0);
      reflowLineFont_.push_back(paraFont);
      line.clear();
      firstLine = false;
    };
    // Measure every distinct codepoint once; the whole-string call stays.
    //
    // What actually cost the time was *not* the whole-string measurement.  The
    // old code memoized ASCII only, so a CJK paragraph called
    // `getTextWidth(font, oneChar)` once per character -- and the uncounted
    // whole-string call hid inside the same timer, which is how I came to
    // attribute 17-36 ms to "a single measurement" and to build a sampling rule
    // to avoid it.  Measured after memoizing every codepoint: a 686-codepoint
    // paragraph now costs 4 ms and an 873-codepoint one costs 20 ms *including*
    // the whole-string measure.  So the exact factor is cheap and there is no
    // reason to estimate it.
    //
    // The sampling rule that briefly lived here was also a memory regression:
    // its byte-offset window table was a third per-codepoint array in a function
    // that already carries `rawWidths` and `chars`, all three on the *internal*
    // heap (plain std::vector), and after one page turn the device sat at
    // `Free 10155 / MaxAlloc 2932` and never recovered.  Per-codepoint arrays
    // here are radio budget, not free memory.
    const uint32_t measureStartedAt = millis();
    unsigned measureCalls = 0;
    reflow_metrics::AdvanceMemo memo;
    std::vector<int> rawWidths;
    rawWidths.reserve(para.size());
    int rawSum = 0;
    size_t j = 0;
    while (j < para.size()) {
      uint32_t cp = 0;
      const size_t n = utf8Next(para, j, cp);
      int raw = 0;
      if (!memo.find(cp, raw)) {
        char cbuf[5];
        std::memcpy(cbuf, para.data() + j, n);
        cbuf[n] = '\0';
        raw = renderer.getTextWidth(paraFont, cbuf);
        ++measureCalls;
        memo.insert(cp, raw);
      }
      rawWidths.push_back(raw);
      rawSum += raw;
      j += n;
    }
    // The factor between the sum of per-character advances and the drawn string
    // (measured 594 px vs 682 px for one line): without it lines fit the model
    // but overflow the panel.
    const size_t count = rawWidths.size();
    const float widthScale = reflow_metrics::fromWhole(
        renderer.getTextWidth(paraFont, para.c_str()), rawSum);
    LOG_INF("PDF", "reflow measure: %u calls in %u ms (chars=%u scale=%.3f)",
            measureCalls, static_cast<unsigned>(millis() - measureStartedAt),
            static_cast<unsigned>(count), static_cast<double>(widthScale));

    struct CharInfo {
      uint32_t cp = 0;
      uint32_t off = 0;
      uint8_t len = 0;
      int width = 0;
    };
    std::vector<CharInfo> chars;
    chars.reserve(para.size());
    {
      size_t at = 0;
      for (size_t rawIndex = 0; at < para.size(); ++rawIndex) {
        uint32_t cp = 0;
        const size_t n = utf8Next(para, at, cp);
        const int raw = rawIndex < rawWidths.size() ? rawWidths[rawIndex] : 0;
        chars.push_back({cp, static_cast<uint32_t>(at), static_cast<uint8_t>(n),
                         std::max(1, static_cast<int>(raw * widthScale + 0.5f))});
        at += n;
      }
    }
    const int spaceWidth =
        std::max(1, static_cast<int>(renderer.getTextWidth(paraFont, " ") * widthScale + 0.5f));
    const int hyphenWidth =
        std::max(1, static_cast<int>(renderer.getTextWidth(paraFont, "-") * widthScale + 0.5f));
    size_t ci = 0;
    int lineWidth = 0;
    while (ci < chars.size()) {
      // A line never starts with a space.
      if (chars[ci].cp == ' ' && line.empty()) {
        ++ci;
        continue;
      }
      const int indent = firstLine ? indentPx : 0;
      const int avail = usableWidth - indent - 16;
      size_t wordEnd = ci;
      int wordWidth = 0;
      while (wordEnd < chars.size() && chars[wordEnd].cp != ' ') {
        wordWidth += chars[wordEnd].width;
        ++wordEnd;
      }
      const int sepWidth = line.empty() ? 0 : spaceWidth;
      if (!line.empty() && lineWidth + sepWidth + wordWidth > avail) {
        // Prefer a language hyphenation point over moving the whole word to
        // the next line.  Hyphenator::breakOffsets is only enabled by the
        // reader's Hyphenation setting; CJK words yield no Latin breaks.
        const size_t wordByteStart = chars[ci].off;
        const std::string word =
            para.substr(wordByteStart, chars[wordEnd - 1].off + chars[wordEnd - 1].len - wordByteStart);
        size_t breakIndex = wordEnd;
        int breakPrefixWidth = 0;
        bool breakNeedsHyphen = false;
        for (const auto& info : Hyphenator::breakOffsets(word, false)) {
          if (info.byteOffset == 0 || info.byteOffset >= word.size()) continue;
          size_t k = ci;
          int prefixWidth = 0;
          while (k < wordEnd && chars[k].off - wordByteStart < info.byteOffset) {
            prefixWidth += chars[k].width;
            ++k;
          }
          if (k <= ci || k >= wordEnd || chars[k].off - wordByteStart != info.byteOffset) continue;
          const int extra = info.requiresInsertedHyphen ? hyphenWidth : 0;
          if (lineWidth + sepWidth + prefixWidth + extra > avail) break;
          breakIndex = k;
          breakPrefixWidth = prefixWidth;
          breakNeedsHyphen = info.requiresInsertedHyphen;
        }
        if (breakIndex < wordEnd) {
          if (sepWidth > 0) {
            line.push_back(' ');
            lineWidth += sepWidth;
          }
          line.append(para, chars[ci].off, chars[breakIndex].off - chars[ci].off);
          if (breakNeedsHyphen) {
            line.push_back('-');
            lineWidth += hyphenWidth;
          } else {
            lineWidth += breakPrefixWidth;
          }
          flush(false);
          lineWidth = 0;
          ci = breakIndex;
          continue;  // re-evaluate the word suffix on the new line
        }
        flush(false);
        lineWidth = 0;
        continue;  // re-evaluate this word on the new line
      }
      if (sepWidth > 0) {
        line.push_back(' ');
        lineWidth += sepWidth;
      }
      if (wordWidth <= avail) {
        for (size_t k = ci; k < wordEnd; ++k) line.append(para, chars[k].off, chars[k].len);
        lineWidth += wordWidth;
        ci = wordEnd;
      } else {
        // A single word longer than the line: break it by characters.
        for (; ci < wordEnd; ++ci) {
          if (!line.empty() && lineWidth + chars[ci].width > avail) {
            flush(false);
            lineWidth = 0;
          }
          line.append(para, chars[ci].off, chars[ci].len);
          lineWidth += chars[ci].width;
        }
      }
      if (ci < chars.size() && chars[ci].cp == ' ') ++ci;
    }
    flush(true);
  }

  size_t chars = 0;
  for (const auto& line : reflowLines_) chars += line.size();
  if (chars < 200) {
    LOG_ERR("PDF", "reflow page %d rejected: only %u bytes of text", currentPage + 1,
            static_cast<unsigned>(chars));
    resetReflow();
    return;
  }

  // Paginate by real line heights (line spacing plus paragraph gaps) instead
  // of a fixed lines-per-screen count, so extra paragraph spacing matches the
  // EPUB reader.
  int y = 0;
  int start = 0;
  for (int i = 0; i < static_cast<int>(reflowLines_.size()); ++i) {
    const int need = lineHeight + (i < static_cast<int>(reflowLineExtra_.size()) ? reflowLineExtra_[i] : 0);
    if (i > start && y + need > pageHeight) {
      reflowScreenStarts_.push_back(start);
      start = i;
      y = 0;
    }
    y += need;
  }
  if (start < static_cast<int>(reflowLines_.size())) reflowScreenStarts_.push_back(start);
  reflowScreens_ = std::max(1, static_cast<int>(reflowScreenStarts_.size()));
  reflowMode_ = true;
  if (landAtEnd) reflowScreen_ = reflowScreens_ - 1;
  LOG_ERR("PDF", "reflow page %d: %u lines, %d screen(s)", currentPage + 1,
          static_cast<unsigned>(reflowLines_.size()), reflowScreens_);
}

void PdfReaderActivity::ensureSelectionBase() {
  if (selectionBase_ != nullptr) return;
  const uint32_t bytes = renderer.getBufferSize();
  selectionBase_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (selectionBase_ != nullptr) selectionBaseBytes_ = bytes;
}

void PdfReaderActivity::captureSelectionBase() {
  ensureSelectionBase();
  if (selectionBase_ == nullptr || renderer.getFrameBuffer() == nullptr) {
    selectionBaseValid_ = false;
    return;
  }
  std::memcpy(selectionBase_, renderer.getFrameBuffer(), selectionBaseBytes_);
  selectionBasePage_ = currentPage;
  selectionBaseScreen_ = reflowScreen_;
  selectionBaseValid_ = true;
}

void PdfReaderActivity::invalidateSelectionBase() {
  selectionBaseValid_ = false;
  selectionBasePage_ = -1;
  selectionBaseScreen_ = -1;
  selectionFramePainted_ = false;
}

bool PdfReaderActivity::renderReflow(const int usableWidth, const int pageTop, const int marginLeft,
                                     const bool showStatus) {
  if (!reflowMode_ || reflowLines_.empty()) return false;
  sdFontSystem.ensureLoaded(renderer);
  const int fontId = reflowFontId();
  const int lineFont = reflowLineFont_.empty() ? fontId : reflowLineFont_.front();
  const int lineHeight = std::max(1, renderer.getLineHeight(lineFont, SETTINGS.getReaderLineCompression()));
  ensureSelectionBase();
  const bool overlayWanted = selectionActive_ || selectionFramePainted_;
  if (selectionBase_ != nullptr && selectionBaseValid_ && selectionBasePage_ == currentPage &&
      selectionBaseScreen_ == reflowScreen_ && overlayWanted && !toolbarOpen) {
    std::memcpy(renderer.getFrameBuffer(), selectionBase_, selectionBaseBytes_);
    drawReflowSelection(lineFont, lineHeight);
    selectionFramePainted_ = selectionActive_ || selectionMenuActive_;
    renderer.displayBuffer(reflowScreen_ == 0 ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    return true;
  }
  renderer.clearScreen();
  const int first = reflowScreen_ < static_cast<int>(reflowScreenStarts_.size()) ? reflowScreenStarts_[reflowScreen_] : 0;
  const int last = reflowScreen_ + 1 < static_cast<int>(reflowScreenStarts_.size())
                       ? reflowScreenStarts_[reflowScreen_ + 1]
                       : static_cast<int>(reflowLines_.size());
  int y = pageTop;
  // reflowWords_ is rebuilt for exactly this screen and is the single source of
  // truth: the text is drawn from it, and selection/handles/lookup read the
  // same rectangles. That keeps "what you select" identical to "what is on the
  // screen", including on justified lines where words are spread.
  reflowWords_.clear();
  for (int i = first; i < last && i < static_cast<int>(reflowLines_.size()); ++i) {
    int hlStart = -1;
    int hlEnd = -1;
    const bool underlined = lineHighlightSpan(i, hlStart, hlEnd);
    const std::string& text = reflowLines_[static_cast<size_t>(i)];
    if (!text.empty()) {
      const int indent = i < static_cast<int>(reflowLineIndent_.size()) ? reflowLineIndent_[i] : 0;
      const int drawFont = i < static_cast<int>(reflowLineFont_.size()) ? reflowLineFont_[i] : lineFont;
      const int natural = renderer.getTextWidth(drawFont, text.c_str());
      int baseX = marginLeft + indent;
      if (SETTINGS.paragraphAlignment == CrossPointSettings::CENTER_ALIGN ||
          SETTINGS.paragraphAlignment == CrossPointSettings::RIGHT_ALIGN) {
        baseX = SETTINGS.paragraphAlignment == CrossPointSettings::CENTER_ALIGN
                    ? marginLeft + (usableWidth - indent - natural) / 2
                    : marginLeft + usableWidth - natural;
        baseX = std::max(marginLeft, baseX);
      }
      const bool paraEnd = i < static_cast<int>(reflowLineParaEnd_.size()) && reflowLineParaEnd_[i] != 0;

      // Segment into selectable atoms: a CJK codepoint is its own word (the
      // granularity the dictionary expects), a run of ASCII is one word. Spaces
      // are gaps between atoms, never words.
      struct Atom {
        uint32_t start;
        uint32_t end;
        int advance;
        bool gapBefore;
      };
      std::vector<Atom> atoms;
      size_t at = 0;
      bool pendingGap = false;
      while (at < text.size()) {
        if (text[at] == ' ' || text[at] == '\t') {
          pendingGap = true;
          ++at;
          continue;
        }
        uint32_t cp = 0;
        const size_t n = utf8Next(text, at, cp);
        Atom a;
        a.start = static_cast<uint32_t>(at);
        a.gapBefore = pendingGap;
        pendingGap = false;
        if (cp >= 0x80) {
          a.end = static_cast<uint32_t>(at + n);
        } else {
          size_t j = at + n;
          while (j < text.size() && text[j] != ' ' && text[j] != '\t' &&
                 static_cast<unsigned char>(text[j]) < 0x80) {
            ++j;
          }
          a.end = static_cast<uint32_t>(j);
        }
        a.advance = std::max(1, renderer.getTextWidth(drawFont, text.substr(at, a.end - at).c_str()));
        atoms.push_back(a);
        at = a.end;
      }

      // Justify distributes slack across the gaps; CJK lines have no gaps so they
      // stay put, and a line with only one gap is left alone (huge gaps read as
      // a typesetting error).
      int gapCount = 0;
      for (const auto& a : atoms) {
        if (a.gapBefore) ++gapCount;
      }
      int gapExtra = 0;
      if (SETTINGS.paragraphAlignment == CrossPointSettings::JUSTIFIED && !paraEnd && gapCount > 1) {
        const int extra = (usableWidth - indent - natural) / gapCount;
        const int maxGapExtra = std::max(1, renderer.getSpaceWidth(drawFont) * 3);
        if (extra > 0 && extra <= maxGapExtra) gapExtra = extra;
      }

      int x = baseX;
      uint16_t wordIndex = 0;
      for (const auto& a : atoms) {
        if (a.gapBefore) x += renderer.getSpaceWidth(drawFont) + gapExtra;
        renderer.drawText(drawFont, x, y, text.substr(a.start, a.end - a.start).c_str(), true);
        ReflowWord w;
        w.x = static_cast<int16_t>(x);
        w.y = static_cast<int16_t>(y);
        w.w = static_cast<int16_t>(a.advance);
        w.h = static_cast<int16_t>(lineHeight);
        w.line = static_cast<uint16_t>(i);
        w.word = wordIndex++;
        w.start = a.start;
        w.end = a.end;
        w.gapBefore = a.gapBefore;
        reflowWords_.push_back(w);
        x += a.advance;
      }

      if (underlined) {
        const int lineBytes = static_cast<int>(text.size());
        const int start = hlStart >= 0 ? std::clamp(hlStart, 0, lineBytes) : 0;
        const int end = hlEnd >= 0 ? std::clamp(hlEnd, start, lineBytes) : lineBytes;
        const std::string prefix = text.substr(0, static_cast<size_t>(start));
        const std::string marked = text.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
        const int sx = baseX + renderer.getTextWidth(drawFont, prefix.c_str());
        const int width = std::max(1, renderer.getTextWidth(drawFont, marked.c_str()));
        renderer.fillRect(sx, y + lineHeight - 3, std::min(width, usableWidth), 2, true);
      }
    }
    y += lineHeight + (i < static_cast<int>(reflowLineExtra_.size()) ? reflowLineExtra_[i] : 0);
  }

  if (showStatus) drawStatusBar();
  if (noteForPage(currentPage)) {
    renderer.fillRect(renderer.getScreenWidth() - 14, pageTop + 6, 8, 8, true);
  }
  if (selectionActive_ || selectionMenuActive_) {
    selectionFramePainted_ = true;
  } else {
    captureSelectionBase();
    selectionFramePainted_ = false;
  }
  drawReflowSelection(lineFont, lineHeight);
  renderer.displayBuffer(reflowScreen_ == 0 ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  return true;
}

void PdfReaderActivity::drawReflowSelection(const int lineFont, const int lineHeight) {
  // Selection highlight: inverse video over the covered words, exactly like the
  // EPUB reader (paint the block, redraw the glyphs in white).
  selection_geom::Ends ends;
  // `selectionRange_` is {0,0..0,0} when nothing is selected, and covers() is
  // inclusive -- drawing that range inverted the first word of every page and
  // laid two dead handles beside it.  Only an active selection owns pixels.
  if (selectionActive_) {
    int prevLine = -1;
    int prevEnd = 0;
    for (const auto& w : reflowWords_) {
      if (!selection_geom::covers(selectionRange_, w.line, w.word)) continue;
    if (!ends.found) {
      ends.found = true;
      ends.firstX = w.x;
      ends.firstY = w.y;
      ends.firstW = w.w;
      ends.firstH = w.h;
    }
    ends.lastX = w.x;
    ends.lastY = w.y;
    ends.lastW = w.w;
    ends.lastH = w.h;
    // Fill the inter-word space too: inverted glyph boxes alone left white gaps
    // between every word, so a multi-word selection looked like dashes.
    if (prevLine == static_cast<int>(w.line) && w.x > prevEnd) {
      renderer.fillRect(prevEnd, w.y, w.x - prevEnd, w.h, true);
    }
    renderer.fillRect(w.x, w.y, w.w, w.h, true);
    const std::string& text = reflowLines_[static_cast<size_t>(w.line)];
    const int drawFont = w.line < static_cast<int>(reflowLineFont_.size()) ? reflowLineFont_[w.line] : lineFont;
    renderer.drawText(drawFont, w.x, w.y, text.substr(w.start, w.end - w.start).c_str(), false);
    prevLine = w.line;
    prevEnd = w.x + w.w;
    }
  }
  if (ends.found) {
    selectionFirstTopY_ = static_cast<int16_t>(ends.firstY);
    selectionLastBottomY_ = static_cast<int16_t>(ends.lastY + ends.lastH);
    selectionFirstX_ = static_cast<int16_t>(ends.firstX);
    selectionLastX_ = static_cast<int16_t>(ends.lastX);
    selectionLastW_ = static_cast<int16_t>(ends.lastW);
    // Two endpoint handles -- the only way to change the range. Geometry is
    // shared with the EPUB reader.
    const selection_geom::Handles handles = selection_geom::handleGeometry(
        ends, SETTINGS.verticalText != 0, renderer.getScreenWidth(), lineHeight);
    for (int i = 0; i < 2; ++i) {
      renderer.fillRect(handles.x[i], handles.y[i], handles.w[i], handles.h[i], false);
      renderer.drawRect(handles.x[i], handles.y[i], handles.w[i], handles.h[i], 2, true);
      selectionHandleX_[i] = handles.x[i];
      selectionHandleY_[i] = handles.y[i];
      selectionHandleW_[i] = handles.w[i];
      selectionHandleH_[i] = handles.h[i];
    }
    if (selectionActive_ && !selectionHandlesLogged_) {
      selectionHandlesLogged_ = true;
      LOG_INF("PDF", "sel handles: %d,%d %dx%d and %d,%d %dx%d", handles.x[0], handles.y[0], handles.w[0],
              handles.h[0], handles.x[1], handles.y[1], handles.w[1], handles.h[1]);
    }
  } else {
    for (int i = 0; i < 2; ++i) selectionHandleW_[i] = 0;
  }

  // The action bar is drawn after the whole page, exactly like the EPUB
  // reader's drawSelectionMenu(), so later body lines never paint over it.
  if (selectionMenuActive_ && ends.found) {
    const int barFont = selectionRange_.startLine < static_cast<int>(reflowLineFont_.size())
                            ? reflowLineFont_[selectionRange_.startLine]
                            : lineFont;
    const bool editing = selectionEditingIndex_ >= 0;
    const int chipCount = editing ? 2 : 3;
    const char* labels[3] = {editing ? tr(STR_DELETE) : tr(STR_LOOKUP),
                             editing ? tr(STR_NOTE) : tr(STR_HIGHLIGHT), tr(STR_NOTE)};
    selection_bar::draw(renderer, barFont, labels, chipCount, std::min(selectionMenuChoice_, chipCount - 1),
                        SETTINGS.verticalText != 0, renderer.getScreenWidth(), renderer.getScreenHeight(),
                        selectionFirstTopY_, selectionLastBottomY_, selectionFirstX_, selectionLastX_,
                        selectionLastW_, selectionBars_);
  }
  if (!toast_.text().empty()) toast_.paint(renderer, SMALL_FONT_ID, 24);
}

void PdfReaderActivity::paintRenderFailed() {
  status_card::paintMessage(renderer, failEscalated_ ? tr(STR_PDF_UNREADABLE) : tr(STR_PDF_RENDER_FAILED),
                            nullptr);
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  // Geometry (and the shrink-on-narrow-screen rule) is host-tested in
  // test/pdf_fail_card; this function only draws what it decided.
  const pdf_fail_card::Spec spec{screenW, screenH, 220, 64, 24, screenH / 2 + 56};
  const pdf_fail_card::Layout pills = pdf_fail_card::layout(spec);
  const int textH = renderer.getLineHeight(UI_10_FONT_ID);
  const auto pill = [&](const pdf_fail_card::Rect r, const char* label, const bool primary) {
    renderer.fillRoundedRect(r.x, r.y, r.w, r.h, r.h / 2, primary ? Color::Black : Color::White);
    if (!primary) renderer.drawRoundedRect(r.x, r.y, r.w, r.h, 2, r.h / 2, true);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, label);
    renderer.drawText(UI_10_FONT_ID, r.x + (r.w - textW) / 2, r.y + (r.h - textH) / 2, label, !primary);
  };
  pill(pills.retry, tr(STR_RETRY), true);
  pill(pills.skip, failEscalated_ ? tr(STR_BACK) : tr(STR_NEXT_PAGE), false);
  failRetryX_ = pills.retry.x; failRetryY_ = pills.retry.y; failRetryW_ = pills.retry.w;
  failRetryH_ = pills.retry.h;
  failSkipX_ = pills.skip.x; failSkipY_ = pills.skip.y; failSkipW_ = pills.skip.w;
  failSkipH_ = pills.skip.h;
  LOG_INF("PDF", "render-failure card: retry at %d,%d %dx%d skip at %d,%d %dx%d", pills.retry.x,
          pills.retry.y, pills.retry.w, pills.retry.h, pills.skip.x, pills.skip.y, pills.skip.w,
          pills.skip.h);
}

void PdfReaderActivity::renderPage() {
  if (renderFailed_) {
    // Set by the failure branch below, by the bench, or by a caller that gave up
    // on a page: the card is the page until the user acts.  It must be PUSHED
    // here -- painting alone left the panel on the previous page, so the bench
    // card (and any repaint after it) looked like nothing had happened.
    paintRenderFailed();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }
  struct PresentedGuard {
    bool& flag;
    ~PresentedGuard() { flag = true; }
  } presented{firstPagePresented_};
  const auto sb = SETTINGS.statusBarSpec();
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const bool showStatus = statusBarHeight > 0 && sb.xtcMode != CrossPointSettings::XTC_STATUS_BAR_HIDE;
  const bool statusTop = showStatus && sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_TOP;
  const bool statusBottom = showStatus && sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_BOTTOM;

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int screenMargin = SETTINGS.screenMargin;
  marginTop += screenMargin;
  marginRight += screenMargin;
  marginBottom += screenMargin;
  marginLeft += screenMargin;
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int usableWidth = std::max(1, screenWidth - marginLeft - marginRight);
  const int pageTop = marginTop + (statusTop ? statusBarHeight + 4 : 0);
  const int pageBottom = screenHeight - marginBottom - (statusBottom ? statusBarHeight + 4 : 0);
  const int pageHeight = std::max(1, pageBottom - pageTop);
  // Remembered for the prefetch pass, which needs the same box but runs after
  // this frame is on screen.
  lastUsableWidth_ = usableWidth;
  lastPageHeight_ = pageHeight;

  // Text pages are reflowed with the reader font; image pages keep the bitmap
  // path below.  The layout is rebuilt whenever the PDF page changes, and that
  // rebuild blocks the render task before anything else in this frame -- a slow
  // one is exactly the "the turn did nothing" report.  The wait card is shown
  // only when the PREVIOUS build was slow: putting it on every turn would add a
  // panel push (and a visible flash) to the common sub-second case.
  if (debugReflowEnabled && reflowPage_ != currentPage) {
    const bool slowExpected = lastReflowMs_ > 800;
    if (slowExpected) {
      char label[24];
      std::snprintf(label, sizeof(label), "%d / %d", currentPage + 1, pdf->pageCount());
      status_card::showWait(renderer, tr(STR_LOADING), label, nullptr, -1, false, millis());
    }
    const uint32_t reflowStart = millis();
    buildReflow(usableWidth, pageHeight);
    lastReflowMs_ = millis() - reflowStart;
    LOG_ERR("PDF", "reflow build %ums (page %d%s)", static_cast<unsigned>(lastReflowMs_), currentPage + 1,
            slowExpected ? ", card was shown" : "");
  }
  if (reflowMode_ && renderReflow(usableWidth, pageTop, marginLeft, showStatus)) {
    failedStreak_ = pdf_fail_streak::afterPage(failedStreak_, true);
    return;
  }

  const std::string bmpPath = pageCachePath(currentPage);
  // Opening the page on purpose clears its failure marker AND the blank
  // placeholder the prefetch pass wrote beside it.  Removing only the marker
  // left the blank BMP in place, `Storage.exists(bmpPath)` then said "cached",
  // and the page the user asked for was rendered as a silent white sheet with
  // no log line anywhere -- the worst of both outcomes.
  if (Storage.exists((bmpPath + ".failed").c_str())) {
    Storage.remove((bmpPath + ".failed").c_str());
    Storage.remove(bmpPath.c_str());
    LOG_INF("PDF", "page %d failed during prefetch; retrying with the full budget", currentPage + 1);
  }
  // A serial page test must always re-render: the cache holds placeholders from
  // earlier failed runs, and reading one would look like a successful render.
  const bool debugRerender = debugInitialPage >= 0;
  if (debugRerender || !Storage.exists(bmpPath.c_str())) {
    LOG_ERR("PDF", "renderPage: page=%d cache=%s max=%dx%d", currentPage + 1, bmpPath.c_str(), usableWidth,
            pageHeight);
    // MuPDF needs the PSRAM the SD family holds; reflow reloads it for text.
    sdFontSystem.releaseSdFamilies(renderer);
    if (auto* fontCache = renderer.getFontCacheManager()) fontCache->releaseSdFontCaches();
    const bool hasText = pdf->pageCount() <= 32 && pdfPageHasText(*pdf, currentPage);
    const bool cover = pdf->pageCount() <= 32 && !hasText;
    // A page can take seconds (whole-page rasterisation of a heavy PDF).  Put
    // the theme's own wait card on screen first -- duck wave, page number -- so
    // a turn never looks like a dead device, and let the wait rule redraw it
    // with elapsed seconds when the render runs long enough to need saying.
    char pageLabel[24];
    std::snprintf(pageLabel, sizeof(pageLabel), "%d / %d", currentPage + 1, pdf->pageCount());
    status_card::showWait(renderer, tr(STR_LOADING), pageLabel, nullptr, -1, false, millis());
    struct TickCtx {
      GfxRenderer* r;
      const char* label;
      char elapsed[16];
    } tickCtx{&renderer, pageLabel, {0}};
    gPdfWaitTickCtx = &tickCtx;
    gPdfWaitTick = [](void* raw, const uint32_t ms) {
      auto* ctx = static_cast<TickCtx*>(raw);
      std::snprintf(ctx->elapsed, sizeof(ctx->elapsed), "%u s", static_cast<unsigned>(ms / 1000));
      status_card::showWait(*ctx->r, tr(STR_LOADING), ctx->label, ctx->elapsed, -1, false, millis());
    };
    // One call: the fallback ladder inside renderPageToBmp already resets the
    // context and trims between attempts.
    const bool rendered = renderPdfPage(*pdf, currentPage, bmpPath, usableWidth, pageHeight, cover);
    gPdfWaitTick = nullptr;
    gPdfWaitTickCtx = nullptr;
    if (!rendered) {
      // The bitmap path is out of memory.  If this page has extractable text,
      // show it as reflowed text (figures may be missing) instead of an error.
      buildReflow(usableWidth, pageHeight, /*allowMixed=*/true);
      if (reflowMode_ && renderReflow(usableWidth, pageTop, marginLeft, showStatus)) {
        LOG_ERR("PDF", "page %d fell back to reflow after a failed bitmap render", currentPage + 1);
        return;
      }
      LOG_ERR("PDF", "Failed to render page %d/%d (free=%u largest=%u)", currentPage + 1, pdf->pageCount(),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
      // The page is genuinely unrenderable in this memory shape.  Say so on the
      // theme's message card and make the two ways out explicit: retry (the
      // memory may have changed) or skip (the rest of the book is readable).
      // "Turn the page to retry" alone left Back as the only discoverable key.
      failedStreak_ = pdf_fail_streak::afterPage(failedStreak_, false);
      failEscalated_ = pdf_fail_streak::escalate(failedStreak_);
      renderFailed_ = true;
      LOG_ERR("PDF", "page %d failed (%d in a row%s)", currentPage + 1, failedStreak_,
              failEscalated_ ? ", this file cannot be shown here" : "");
      paintRenderFailed();
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return;
    }
    LOG_ERR("PDF", "renderPage ok page=%d psram free=%u largest=%u", currentPage + 1,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  }
  rememberCachedPage(currentPage);

  HalFile file;
  if (!Storage.openFileForRead("PDF", bmpPath, file)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, tr(STR_INVALID_IMAGE_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  const int x = marginLeft + (usableWidth - bitmap.getWidth()) / 2;
  // Centre the fitted page vertically: a top-aligned page leaves all the slack
  // as a white band at the bottom (measured 97 px against 36 px on top).
  const int pageY = pageTop + std::max(0, (pageHeight - bitmap.getHeight()) / 2);
  if (!grayLogged) {
    grayLogged = true;
    LOG_ERR("PDF", "display: grayscale levels=%u bitmap=%dx%d", renderer.getGrayscaleLevels(),
            bitmap.getWidth(), bitmap.getHeight());
  }
  // The page is about to be presented: this render succeeded, whatever happened
  // to earlier pages.
  failedStreak_ = pdf_fail_streak::afterPage(failedStreak_, true);
  bool shown = false;
  // The toolbar overlay is 1-bit FreeInkUI chrome.  Rendering the page through
  // the native 16-level path and then pushing the whole 1-bit framebuffer would
  // replace that gray page with its thresholded proxy, so while the toolbar is
  // open use the 1-bit page too; closing it restores the 16-level path.
  if (!toolbarOpen && renderer.beginGrayscale16()) {
    if (renderer.drawBitmapGrayscale16(bitmap, x, pageY, usableWidth, pageHeight)) {
      if (showStatus) drawStatusBar();
      shown = renderer.commitGrayscale16();
    }
    if (!shown) renderer.cancelGrayscale16();
  }

  if (!shown) {
    // 1-bit fallback: Bitmap dithering turns the 8-bit page into the panel's
    // 2-bit/BW pipeline when the grayscale buffer is unavailable.
    renderer.clearScreen();
    if (!renderer.drawBitmap(bitmap, x, pageY, usableWidth, pageHeight, 0, 0)) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return;
    }
    if (showStatus) drawStatusBar();
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }
  }

  // Small PDFs: warm the next page while the current one is already on screen.
  // This turns the next page turn into a cache hit; large PDFs skip it because
  // a second page image would re-fragment the PSRAM the renderer needs.
  if (pdf && !fullCacheComplete_ && pdf->pageCount() <= 32 && currentPage + 1 < pdf->pageCount()) {
    const std::string nextPath = pageCachePath(currentPage + 1);
    if (!Storage.exists(nextPath.c_str())) {
      const bool nextHasText = pdf->pageCount() <= 32 && pdfPageHasText(*pdf, currentPage + 1);
      const bool nextCover = pdf->pageCount() <= 32 && !nextHasText;
      renderPdfPage(*pdf, currentPage + 1, nextPath, usableWidth, pageHeight, nextCover);
    }
  }
}

bool PdfReaderActivity::pageTurn(const bool isForward) {
  if (!pdf) return false;
  // A page change means the user moved on from a failed page, whichever path
  // asked for it (the bench turn goes straight to pageTurn).  It must happen
  // only when the page change DID happen: clearing it first turned "skip" on
  // the last page into a silent retry of the page that had just failed -- the
  // exact loop this card exists to stop.
  const auto leftCard = [&] {
    if (renderFailed_) {
      renderFailed_ = false;
      LOG_INF("PDF", "leaving the failure card for page change");
    }
  };
  READING_STATS.noteActivity();
  // Within a reflowed page, a turn moves one screen before it moves the PDF page.
  if (reflowMode_) {
    if (isForward && reflowScreen_ + 1 < reflowScreens_) {
      ++reflowScreen_;
      leftCard();
      return true;
    }
    if (!isForward && reflowScreen_ > 0) {
      --reflowScreen_;
      leftCard();
      return true;
    }
    // Crossing onto the end-of-book notice keeps the layout: the notice shares
    // this activity, and coming back from it must be instant and land on the
    // very screen the reader left. Any other page change rebuilds it.
    if (!(isForward && currentPage + 1 >= pdf->pageCount())) resetReflow();
  }
  const int pageCount = pdf->pageCount();
  if (isForward) {
    if (currentPage < pageCount) {
      ++currentPage;
      leftCard();
      return true;
    }
  } else if (currentPage > 0) {
    --currentPage;
    leftCard();
    return true;
  }
  return false;
}

bool PdfReaderActivity::skipPages(const int amount) {
  if (!pdf || amount == 0) return false;
  READING_STATS.noteActivity();
  const int next = std::clamp(currentPage + amount, 0, pdf->pageCount());
  if (next == currentPage) return false;
  // Same rule as pageTurn(): moving off a failed page leaves the failure card,
  // and this path (jump by N) does not go through pageTurn().
  if (renderFailed_) {
    renderFailed_ = false;
    LOG_INF("PDF", "leaving the failure card for a page jump");
  }
  resetReflow();
  currentPage = next;
  return true;
}

bool PdfReaderActivity::isAtEndOfBook() const { return pdf && currentPage >= pdf->pageCount(); }

void PdfReaderActivity::onReturnFromEndOfBook() {
  if (!pdf || pdf->pageCount() == 0) {
    currentPage = 0;
    return;
  }
  currentPage = pdf->pageCount() - 1;
  // The natural route (a turn off the final screen) kept the layout, so the
  // reader lands on exactly the screen it left. When the state was dropped
  // (a 10-page skip onto the notice), rebuild and still land at the end.
  if (!reflowMode_ || reflowPage_ != currentPage) {
    reflowEndScreenPending_ = reflowBookEnabled_;
  }
}

void PdfReaderActivity::saveProgress() const {
  if (!pdf || cachePath_.empty()) return;
  const int total = pdf->pageCount();
  const uint8_t percent =
      total > 0 ? static_cast<uint8_t>(std::min(100, (currentPage + 1) * 100 / total)) : 0;
  READING_STATS.updateProgress(percent, total > 0 && currentPage + 1 >= total, "", percent);

  const uint8_t data[4] = {static_cast<uint8_t>(currentPage & 0xFF),
                           static_cast<uint8_t>((currentPage >> 8) & 0xFF),
                           static_cast<uint8_t>((currentPage >> 16) & 0xFF),
                           static_cast<uint8_t>((currentPage >> 24) & 0xFF)};
  if (!ProgressFile::writeAtomic(cachePath_, data, sizeof(data))) {
    LOG_ERR("PDF", "Failed to save progress: page %d", currentPage);
  }
}

void PdfReaderActivity::loadProgress() {
  if (!pdf || cachePath_.empty()) return;
  HalFile file;
  if (!Storage.openFileForRead("PDF", cachePath_ + "/progress.bin", file)) return;
  uint8_t data[4];
  if (file.read(data, sizeof(data)) != 4) return;
  currentPage = static_cast<int>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
  if (pdf->pageCount() > 0 && currentPage >= pdf->pageCount()) currentPage = pdf->pageCount() - 1;
  if (currentPage < 0) currentPage = 0;
}

ScreenshotInfo PdfReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Pdf;
  if (pdf) {
    const std::string title = getBookTitle();
    snprintf(info.title, sizeof(info.title), "%s", title.c_str());
    const int pageCount = pdf->pageCount();
    info.totalPages = pageCount;
    const int page = pageCount > 0 ? std::clamp(currentPage, 0, pageCount - 1) : 0;
    info.progressPercent = pageCount > 0 ? (page + 1) * 100 / pageCount : 0;
    info.currentPage = page + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}

#endif
