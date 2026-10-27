#if defined(CROSSPOINT_MUPDF)

#include "PdfReaderActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <PinyinDb.h>

#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "CrossPointSettings.h"
#include "Epub/FirstLineIndent.h"
#include "Epub/hyphenation/HyphenationStore.h"
#include "EpubReaderMenuActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "UiFontRefresh.h"
#include "ExternalFontPackStorage.h"
#include "ReadingStatsStore.h"
#include "ReaderActivity.h"
#include "activities/ActivityResult.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr uint32_t kCacheMagic = 0x50444643u;  // "PDFC"
constexpr uint16_t kCacheVersion = 1;

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
  PdfDocument* doc = nullptr;
  int page = 0;
  std::string path;
  int width = 0;
  int height = 0;
  bool cover = false;
  volatile bool done = false;
  volatile bool ok = false;
};

TaskHandle_t gPdfWorkerTask = nullptr;
SemaphoreHandle_t gPdfWorkerStart = nullptr;
SemaphoreHandle_t gPdfWorkerDone = nullptr;
PdfRenderJob* gPdfWorkerJob = nullptr;

void pdfRenderWorker(void*) {
  for (;;) {
    if (!gPdfWorkerStart || xSemaphoreTake(gPdfWorkerStart, portMAX_DELAY) != pdTRUE) continue;
    PdfRenderJob* job = gPdfWorkerJob;
    if (!job) continue;
    job->ok = job->doc->renderPageToBmp(job->page, job->path, job->width, job->height, job->cover);
    job->done = true;
    xSemaphoreGive(gPdfWorkerDone);
  }
}

void stopPdfWorker();

bool startPdfWorker() {
  if (gPdfWorkerTask) return true;
  gPdfWorkerStart = xSemaphoreCreateBinary();
  gPdfWorkerDone = xSemaphoreCreateBinary();
  if (!gPdfWorkerStart || !gPdfWorkerDone) {
    stopPdfWorker();
    return false;
  }
  const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      pdfRenderWorker, "PdfRender", 65536 / sizeof(StackType_t), nullptr, 1, &gPdfWorkerTask, 0,
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

void stopPdfWorker() {
  if (gPdfWorkerTask) {
    vTaskDeleteWithCaps(gPdfWorkerTask);
    gPdfWorkerTask = nullptr;
  }
  if (gPdfWorkerStart) {
    vSemaphoreDelete(gPdfWorkerStart);
    gPdfWorkerStart = nullptr;
  }
  if (gPdfWorkerDone) {
    vSemaphoreDelete(gPdfWorkerDone);
    gPdfWorkerDone = nullptr;
  }
  gPdfWorkerJob = nullptr;
}

bool renderPdfPage(PdfDocument& doc, int page, const std::string& path, int width, int height, bool cover) {
  if (!gPdfWorkerTask) return doc.renderPageToBmp(page, path, width, height, cover);
  PdfRenderJob job{&doc, page, path, width, height, cover, false, false};
  gPdfWorkerJob = &job;
  xSemaphoreGive(gPdfWorkerStart);
  xSemaphoreTake(gPdfWorkerDone, portMAX_DELAY);
  gPdfWorkerJob = nullptr;
  doc.setUseDisplayList(false);
  return job.ok;
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
}  // namespace

int PdfReaderActivity::debugInitialPage = -1;
bool PdfReaderActivity::debugReflowEnabled = true;
int PdfReaderActivity::debugSeedNotePage = -1;
int PdfReaderActivity::debugSeedHighlightPage = -1;
bool PdfReaderActivity::debugSelfTest = false;

bool PdfReaderActivity::loadBook() {
  LOG_ERR("PDF", "loadBook enter: %s", bookPath.c_str());
  auto loaded = makeUniqueNoThrow<PdfDocument>(bookPath);
  if (!loaded) {
    LOG_ERR("PDF", "OOM: PdfDocument");
    return false;
  }
  LOG_ERR("PDF", "loadBook: PdfDocument created");
  if (!loaded->load()) {
    LOG_ERR("PDF", "failed to load %s", bookPath.c_str());
    return false;
  }
  LOG_ERR("PDF", "loadBook: loaded pages=%d", loaded->pageCount());
  pdf = std::move(loaded);

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
  // A PDF page can need ~3.5 MB of MuPDF page resources.  The external CJK pack
  // is UI-only; hand its 1.3 MB back to the allocator for the whole PDF session
  // and use the built-in CJK face for status/title text.  onExit() reloads it.
  probeReflowSupport();
  releaseExternalFontPack();
  useBuiltinFontFallback();
  // Pinyin is a keyboard/library feature; a PDF page needs the contiguous PSRAM
  // more than the 349 KB table.  It reloads lazily when a text keyboard opens.
  pinyin::PinyinDb::shared().release();
  hyphenation::HyphenationStore::shared().releaseAll();
  LOG_ERR("PDF", "pinyin/hyphenation caches released for PDF render");
  startPdfWorker();
  return true;
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

  // The PDF changed (or this is the first open): drop old page BMPs and
  // progress together rather than mixing pages from two files.
  Storage.removeDir(cachePath_.c_str());
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
    const bool hasText = pdf->pageCount() <= 32 ? pdf->pageHasText(i) : false;
    const bool cover = pdf->pageCount() <= 32 && !hasText;
    if (!renderPdfPage(*pdf, i, path, usableWidth, pageHeight, cover)) {
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
  HalFile file;
  if (!Storage.openFileForWrite("PDF", cachePath_ + "/annotations.bin", file)) return false;
  file.write(bytes.data(), bytes.size());
  file.close();
  return true;
}

const annotations::Annotation* PdfReaderActivity::noteForPage(const int page) const {
  for (const auto& note : pageNotes_) {
    if (note.spineIndex == static_cast<uint16_t>(page)) return &note;
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
        pageNotes_.erase(std::remove_if(pageNotes_.begin(), pageNotes_.end(),
                                        [this](const annotations::Annotation& a) {
                                          return a.spineIndex == static_cast<uint16_t>(currentPage);
                                        }),
                         pageNotes_.end());
        if (!text.empty()) {
          annotations::Annotation note{};
          note.spineIndex = static_cast<uint16_t>(currentPage);
          note.startOffset = 0;
          note.endOffset = 1;  // whole page: the marker covers this page
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
  const uint32_t start = static_cast<uint32_t>(reflowScreen_ * reflowLinesPerScreen_);
  const uint32_t end = static_cast<uint32_t>(std::min<int>(
      static_cast<int>(reflowLines_.size()), (reflowScreen_ + 1) * reflowLinesPerScreen_));
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
  HalFile out;
  if (!Storage.openFileForWrite("PDF", cachePath_ + "/bookmarks.bin", out)) return false;
  bool ok = out.write(&header, sizeof(header)) == static_cast<int>(sizeof(header));
  for (size_t i = 0; ok && i < header.count; ++i) {
    const uint32_t page = static_cast<uint32_t>(bookmarks_[i]);
    ok = out.write(&page, sizeof(page)) == static_cast<int>(sizeof(page));
  }
  out.close();
  return ok;
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
  for (const auto& note : pageNotes_) rows.push_back({static_cast<int>(note.spineIndex), true});
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
  stopPdfWorker();
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
  ReaderActivity::onExit();
}

int PdfReaderActivity::progressPercent() const {
  if (!pdf || pdf->pageCount() <= 0) return 0;
  return std::clamp((currentPage + 1) * 100 / pdf->pageCount(), 0, 100);
}

bool PdfReaderActivity::usesToolbarMenu() const {
  return SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

void PdfReaderActivity::openToolbar() {
  if (!pdf) return;
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
  requestUpdate();
}

void PdfReaderActivity::closeToolbar() {
  toolbarOpen = false;
  toolbarPanel = false;
  requestUpdate();
}

int PdfReaderActivity::toolbarItemCount() const {
  if (!pdf) return 0;
  switch (focusedTool) {
    case 0: return pdf->outline().empty() ? pdf->pageCount() : static_cast<int>(pdf->outline().size());
    case 1: return 1;
    case 2: return bookmarks_.empty() ? 1 : static_cast<int>(bookmarks_.size());
    case 3: return 6;
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
    closeToolbar();
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
    default:
      break;
  }
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
              pageNotes_.erase(std::remove_if(pageNotes_.begin(), pageNotes_.end(),
                                              [&row](const annotations::Annotation& a) {
                                                return a.spineIndex == static_cast<uint16_t>(row.page);
                                              }),
                               pageNotes_.end());
              savePageNotes();
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
      model.fullRowSelection = true;
    } else if (focusedTool == 1) {
      model.panelTitle = tr(STR_TOOL_TYPESETTING);
      model.itemCount = 1;
      model.rowText = [this](int) {
        char buffer[96];
        snprintf(buffer, sizeof(buffer), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, pdf->pageCount(),
                 static_cast<double>(progressPercent()));
        return std::string(buffer);
      };
      model.rowValue = [](int) { return std::string(); };
    } else if (focusedTool == 2) {
      model.panelTitle = tr(STR_BOOKMARKS);
      model.bookmarkAction = true;
      model.bookmarkActionRemove = isPageBookmarked(currentPage);
      const auto rows = bookmarkRows();
      model.itemCount = rows.empty() ? 1 : static_cast<int>(rows.size());
      model.rowText = [this, rows](int i) {
        if (rows.empty()) return std::string(tr(STR_NO_BOOKMARKS));
        return std::to_string(rows[static_cast<size_t>(i)].page + 1);
      };
      model.rowValue = [this, rows](int i) {
        if (rows.empty()) return std::string();
        if (rows[static_cast<size_t>(i)].note) return std::string(tr(STR_PDF_PAGE_NOTE));
        char percent[8];
        snprintf(percent, sizeof(percent), "%d%%",
                 (rows[static_cast<size_t>(i)].page + 1) * 100 / std::max(1, pdf->pageCount()));
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
      model.fullRowSelection = true;
    } else {
      model.panelTitle = tr(STR_TOOL_MORE);
      model.itemCount = 6;
      model.rowText = [](int i) -> std::string {
        switch (i) {
          case 0: return std::string(tr(STR_ORIENTATION));
          case 1: return std::string(tr(STR_GO_TO_PERCENT));
          case 2: return std::string(tr(STR_GO_TO_PAGE));
          case 3: return std::string(tr(STR_SCREENSHOT_BUTTON));
          case 4: return std::string(tr(STR_GO_HOME_BUTTON));
          default: return std::string(tr(STR_DELETE_CACHE));
        }
      };
      model.rowValue = [](int i) -> std::string {
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
      false, true);
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
  if (!pdf || currentPage < 0 || currentPage >= pdf->pageCount()) return;
  renderPage();
  saveProgress();
  if (toolbarOpen) {
    renderToolbarOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
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
  const bool hasText = pdf->extractText(page, paras, images) && !images;
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

void PdfReaderActivity::loop() {
  ReaderActivity::loop();
  if (debugSelfTest) {
    runSelfTestStep();
    return;
  }
  // Serial-driven page tests ask for one page; the background full cache would
  // otherwise render all 112 and drown the diagnostic log.
  if (debugInitialPage >= 0) return;
  // A reflowed text page needs no page bitmap, so do not spend 13 s rendering
  // one in the background while the reader is on it.
  if (reflowMode_ || reflowBookEnabled_) return;
  if (fullCacheComplete_ || !pdf) return;
  const auto sb = SETTINGS.statusBarSpec();
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const bool showStatus = statusBarHeight > 0 && sb.xtcMode != CrossPointSettings::XTC_STATUS_BAR_HIDE;
  const bool statusTop = showStatus && sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_TOP;
  const bool statusBottom = showStatus && sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_BOTTOM;
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int usableWidth = std::max(1, screenWidth - marginLeft - marginRight);
  const int pageTop = marginTop + (statusTop ? statusBarHeight + 4 : 0);
  const int pageBottom = screenHeight - marginBottom - (statusBottom ? statusBarHeight + 4 : 0);
  const int pageHeight = std::max(1, pageBottom - pageTop);
  ensureFullCache(usableWidth, pageHeight);
}

void PdfReaderActivity::probeReflowSupport() {
  reflowBookEnabled_ = false;
  if (!pdf || pdf->pageCount() <= 0) return;

  // Sample three pages spread over the document.  A page counts as usable when
  // it yields a body's worth of text and draws no figure/table: reflowing a
  // mixed page would silently drop the figure, which the reader must never do.
  const int total = pdf->pageCount();
  const int samples[3] = {std::min(1, total - 1), total / 2, std::max(0, total - 2)};
  int usable = 0;
  int counted = 0;
  for (const int page : samples) {
    std::vector<std::string> paras;
    bool images = false;
    if (!pdf->extractText(page, paras, images)) continue;
    ++counted;
    size_t chars = 0;
    for (const auto& p : paras) chars += p.size();
    if (!images && chars >= 400) ++usable;
  }
  reflowBookEnabled_ = counted > 0 && usable * 2 >= counted;
  LOG_ERR("PDF", "reflow support: %d/%d sampled pages usable -> %s", usable, counted,
          reflowBookEnabled_ ? "on" : "off");
}

bool PdfReaderActivity::pageHasLineHighlight(const int line) const {
  for (const auto& a : pageNotes_) {
    if (a.spineIndex != static_cast<uint16_t>(currentPage)) continue;
    if (a.kind != static_cast<uint8_t>(annotations::Kind::Highlight)) continue;
    if (line >= static_cast<int>(a.startOffset) && line < static_cast<int>(a.endOffset)) return true;
  }
  return false;
}

void PdfReaderActivity::resetReflow() {
  reflowMode_ = false;
  reflowPage_ = -1;
  reflowScreen_ = 0;
  reflowScreens_ = 0;
  reflowLinesPerScreen_ = 0;
  reflowLines_.clear();
}

void PdfReaderActivity::buildReflow(const int usableWidth, const int pageHeight, const bool allowMixed) {
  resetReflow();
  reflowPage_ = currentPage;
  if (!pdf || !reflowBookEnabled_ || usableWidth <= 0 || pageHeight <= 0) return;

  std::vector<std::string> paragraphs;
  bool hasImages = false;
  if (!pdf->extractText(currentPage, paragraphs, hasImages)) return;
  // A page with a figure or table keeps the bitmap path: reflowing would drop
  // the figure, and the user's rule is "text pages reflow, everything else
  // stays a picture".
  // Mixed text+figure pages keep the bitmap because reflow would drop the
  // figure; `allowMixed` is the last-resort path used only after the bitmap
  // render itself failed, where text without the figure beats an error page.
  if (hasImages && !allowMixed) return;

  const int fontId = SETTINGS.getReaderFontId();
  // Match the EPUB reader's typography: the same line compression setting, and
  // a first-line indent when the reader is configured to indent paragraphs.
  const bool indent = SETTINGS.firstLineIndent == FirstLineIndent::Indent;
  for (const auto& para : paragraphs) {
    if (para.empty()) continue;
    // U+3000 twice: the same two-character indent the EPUB reader applies.
    std::string line = indent ? std::string("\xE3\x80\x80\xE3\x80\x80") : std::string();
    size_t i = 0;
    while (i < para.size()) {
      uint32_t cp = 0;
      const size_t n = utf8Next(para, i, cp);
      std::string candidate = line;
      candidate.append(para, i, n);
      // 16 px of slack: getTextWidth ignores the last glyph's right bearing, and
      // the rotated draw clips at the physical panel edge (measured: long lines
      // lost their final character at exactly usableWidth).
      if (!line.empty() && renderer.getTextWidth(fontId, candidate.c_str()) > usableWidth - 16) {
        reflowLines_.push_back(line);
        line.clear();
        if (cp == ' ') {  // swallow the space that caused the break
          i += n;
          continue;
        }
      }
      line.append(para, i, n);
      i += n;
    }
    if (!line.empty()) reflowLines_.push_back(std::move(line));
    reflowLines_.push_back(std::string());  // paragraph gap
  }
  if (reflowLines_.empty()) return;
  // Extraction can succeed with only a running header/footer when the body uses
  // a font without Unicode mappings (page 40 of the test book: 21 lines, body
  // missing).  Never replace a complete bitmap page with a nearly empty reflow.
  size_t chars = 0;
  for (const auto& line : reflowLines_) chars += line.size();
  if (chars < 200) {
    LOG_ERR("PDF", "reflow page %d rejected: only %u bytes of text", currentPage + 1,
            static_cast<unsigned>(chars));
    resetReflow();
    return;
  }

  const int lineHeight = std::max(1, renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression()) + 2);
  reflowLinesPerScreen_ = std::max(1, pageHeight / lineHeight);
  reflowScreens_ =
      std::max(1, static_cast<int>((reflowLines_.size() + reflowLinesPerScreen_ - 1) / reflowLinesPerScreen_));
  reflowMode_ = true;
  LOG_ERR("PDF", "reflow page %d: %u lines, %d screen(s)", currentPage + 1,
          static_cast<unsigned>(reflowLines_.size()), reflowScreens_);
}

bool PdfReaderActivity::renderReflow(const int usableWidth, const int pageTop, const int marginLeft,
                                     const bool showStatus) {
  if (!reflowMode_ || reflowLines_.empty()) return false;
  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = std::max(1, renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression()) + 2);
  renderer.clearScreen();
  int y = pageTop;
  const int first = reflowScreen_ * reflowLinesPerScreen_;
  for (int i = first; i < first + reflowLinesPerScreen_ && i < static_cast<int>(reflowLines_.size()); ++i) {
    const bool underlined = pageHasLineHighlight(i);
    if (!reflowLines_[i].empty()) {
      renderer.drawText(fontId, marginLeft, y, reflowLines_[i].c_str(), true);
      if (underlined) {
        const int width = renderer.getTextWidth(fontId, reflowLines_[i].c_str());
        renderer.fillRect(marginLeft, y + lineHeight - 3, std::min(width, usableWidth), 2, true);
      }
    }
    y += lineHeight;
  }
  if (showStatus) drawStatusBar();
  // A page carrying a note gets a small marker so it can be found again.
  if (noteForPage(currentPage)) {
    renderer.fillRect(renderer.getScreenWidth() - 14, pageTop + 6, 8, 8, true);
  }
  renderer.displayBuffer(reflowScreen_ == 0 ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  (void)usableWidth;
  return true;
}

void PdfReaderActivity::renderPage() {
  const auto sb = SETTINGS.statusBarSpec();
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const bool showStatus = statusBarHeight > 0 && sb.xtcMode != CrossPointSettings::XTC_STATUS_BAR_HIDE;
  const bool statusTop = showStatus && sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_TOP;
  const bool statusBottom = showStatus && sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_BOTTOM;

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int usableWidth = std::max(1, screenWidth - marginLeft - marginRight);
  const int pageTop = marginTop + (statusTop ? statusBarHeight + 4 : 0);
  const int pageBottom = screenHeight - marginBottom - (statusBottom ? statusBarHeight + 4 : 0);
  const int pageHeight = std::max(1, pageBottom - pageTop);

  // Text pages are reflowed with the reader font; image pages keep the bitmap
  // path below.  The layout is rebuilt whenever the PDF page changes.
  if (debugReflowEnabled && reflowPage_ != currentPage) buildReflow(usableWidth, pageHeight);
  if (reflowMode_ && renderReflow(usableWidth, pageTop, marginLeft, showStatus)) return;

  const std::string bmpPath = pageCachePath(currentPage);
  // Opening the page on purpose clears its failure marker so the full ladder
  // runs again (e.g. after the user freed memory by closing other books).
  if (Storage.exists((bmpPath + ".failed").c_str())) Storage.remove((bmpPath + ".failed").c_str());
  // A serial page test must always re-render: the cache holds placeholders from
  // earlier failed runs, and reading one would look like a successful render.
  const bool debugRerender = debugInitialPage >= 0;
  if (debugRerender || !Storage.exists(bmpPath.c_str())) {
    LOG_ERR("PDF", "renderPage: page=%d cache=%s max=%dx%d", currentPage + 1, bmpPath.c_str(), usableWidth,
            pageHeight);
    const bool hasText = pdf->pageCount() <= 32 && pdf->pageHasText(currentPage);
    const bool cover = pdf->pageCount() <= 32 && !hasText;
    // A page can take seconds (whole-page rasterisation of a heavy PDF).  Put
    // something on screen first so a turn never looks like a dead device.
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, tr(STR_LOADING), true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    // One call: the fallback ladder inside renderPageToBmp already resets the
    // context and trims between attempts.
    const bool rendered = renderPdfPage(*pdf, currentPage, bmpPath, usableWidth, pageHeight, cover);
    if (!rendered) {
      // The bitmap path is out of memory.  If this page has extractable text,
      // show it as reflowed text (figures may be missing) instead of an error.
      buildReflow(usableWidth, pageHeight, /*allowMixed=*/true);
      if (reflowMode_ && renderReflow(usableWidth, pageTop, marginLeft, showStatus)) {
        LOG_ERR("PDF", "page %d fell back to reflow after a failed bitmap render", currentPage + 1);
        return;
      }
      LOG_ERR("PDF", "Failed to render page %d/%d", currentPage + 1, pdf->pageCount());
      // WhiteOS-style wording: say the page failed and that another turn
      // retries it, instead of a generic "page load error".  A turn removes the
      // failure marker, so the retry is real.
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2 - 12, tr(STR_PDF_RENDER_FAILED), true,
                                EpdFontFamily::BOLD);
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
      const bool nextHasText = pdf->pageCount() <= 32 && pdf->pageHasText(currentPage + 1);
      const bool nextCover = pdf->pageCount() <= 32 && !nextHasText;
      renderPdfPage(*pdf, currentPage + 1, nextPath, usableWidth, pageHeight, nextCover);
    }
  }
}

bool PdfReaderActivity::pageTurn(const bool isForward) {
  if (!pdf) return false;
  READING_STATS.noteActivity();
  // Within a reflowed page, a turn moves one screen before it moves the PDF page.
  if (reflowMode_) {
    if (isForward && reflowScreen_ + 1 < reflowScreens_) {
      ++reflowScreen_;
      return true;
    }
    if (!isForward && reflowScreen_ > 0) {
      --reflowScreen_;
      return true;
    }
    resetReflow();
  }
  const int pageCount = pdf->pageCount();
  if (isForward) {
    if (currentPage < pageCount) {
      ++currentPage;
      return true;
    }
  } else if (currentPage > 0) {
    --currentPage;
    return true;
  }
  return false;
}

bool PdfReaderActivity::skipPages(const int amount) {
  if (!pdf || amount == 0) return false;
  READING_STATS.noteActivity();
  const int next = std::clamp(currentPage + amount, 0, pdf->pageCount());
  if (next == currentPage) return false;
  resetReflow();
  currentPage = next;
  return true;
}

bool PdfReaderActivity::isAtEndOfBook() const { return pdf && currentPage >= pdf->pageCount(); }

void PdfReaderActivity::onReturnFromEndOfBook() {
  currentPage = pdf && pdf->pageCount() > 0 ? pdf->pageCount() - 1 : 0;
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
