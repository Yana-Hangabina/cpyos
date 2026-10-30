#include "InxRecentActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "util/TimeUtils.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/icons/listIcons48.h"
#include "fontIds.h"
#include "util/BookCoverLoader.h"
#include "util/ReadingStatsAnalytics.h"
#ifdef BOARD_HAS_PSRAM
#include "activities/apps/jjwxc/JjwxcActivity.h"
#include "activities/apps/jjwxc/JjwxcStore.h"
#include "activities/apps/jjwxc/JjwxcUpdates.h"
#endif

namespace {
// Draws a fui Mask1 icon (0 bit = ink) at an arbitrary size. The older
// drawIcon() rotates its input; these 48px Lucide assets are already upright
// and are used unscaled-by-rotation throughout the settings UI.
void drawMaskIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const int size) {
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

const char* sourceLabelForPath(const std::string& path) {  if (path.rfind("/JJWXC/", 0) == 0) return I18N.get(StrId::STR_SOURCE_JJWXC);
  if (path.rfind("/WeRead/", 0) == 0 || path.rfind("/WEREAD/", 0) == 0) return I18N.get(StrId::STR_SOURCE_WEREAD);
  return I18N.get(StrId::STR_SOURCE_LOCAL);
}

std::string relativeDayLabel(const uint32_t timestamp) {
  if (timestamp == 0) return {};
  const time_t now = time(nullptr);
  if (now <= 1700000000 || timestamp <= 1700000000 || timestamp > static_cast<uint32_t>(now)) return {};
  const uint32_t days = (static_cast<uint32_t>(now) - timestamp) / 86400U;
  if (days == 0) return tr(STR_DASH_TODAY);
  if (days == 1) return tr(STR_YESTERDAY);
  char buf[32];
  std::snprintf(buf, sizeof(buf), I18N.get(StrId::STR_DAYS_AGO_FMT), static_cast<unsigned>(days));
  return buf;
}

// refreshed UI progress: a thin sine baseline, a solid 10 px band over the read
// portion, and a short marker at the boundary. Drawing it per pixel keeps the
// wave crisp on the 1-bit panel; the line is only ~600 px wide.
void drawWaveProgress(const GfxRenderer& renderer, const Rect rect, const int percent) {
  constexpr float kTwoPi = 6.2831853f;
  const int mid = rect.y + rect.height / 2;
  const int amplitude = std::max(2, rect.height / 8);
  const int clamped = std::clamp(percent, 0, 100);
  const int fillWidth = rect.width * clamped / 100;
  const auto waveY = [&](const int x) {
    return mid + static_cast<int>(std::lround(static_cast<float>(amplitude) *
                                             std::sin(static_cast<float>(x) * kTwoPi / 36.0f)));
  };
  for (int x = 0; x + 1 < rect.width; ++x) {
    const int y0 = waveY(x);
    const int y1 = waveY(x + 1);
    if (x < fillWidth) {
      const int top = std::min(y0, y1) - 4;
      renderer.fillRect(rect.x + x, top, 1, 9, true);
    } else {
      renderer.drawLine(rect.x + x, y0, rect.x + x + 1, y1, true);
    }
  }
  if (fillWidth > 0) {
    const int markerX = rect.x + std::min(fillWidth, rect.width - 2);
    renderer.fillRect(markerX, mid - amplitude - 4, 2, amplitude * 2 + 8, true);
  }
}

}  // namespace

void InxRecentActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  // Key navigation from the tab strip lands on the first/last dashboard slot,
  // not on a book index the dashboard does not use.
  selected = MainTabs::contentEdgeIndex(edge, kDashSlots);
}

Rect InxRecentActivity::contentRect() const {
  Rect content = pageContentRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topInset = mainTabsAtBottom() && !hasMainTabStatusBar() ? metrics.batteryBarHeight : 0;
  content.y += topInset + metrics.verticalSpacing;
  content.height -= topInset + metrics.verticalSpacing * 2;
  if (!mainTabsAtBottom()) {
    content.height -= std::max(0, InxRecentGeometry::footerReservedHeight - metrics.buttonHintsHeight);
  }
  return content;
}

const ReadingBookStats* InxRecentActivity::statsAt(const int index) const {
  return index >= 0 && index < static_cast<int>(bookStats.size()) ? bookStats[index] : nullptr;
}

void InxRecentActivity::onEnter() {
  Activity::onEnter();
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  books = &RECENT_BOOKS.getBooks();
  bookStats.fill(nullptr);
  for (size_t i = 0; i < std::min(books->size(), bookStats.size()); ++i) {
    const RecentBook& book = (*books)[i];
    bookStats[i] = READING_STATS.findMatchingBookForPath(book.path, book.title, book.author);
  }
  selected = 0;
  chapterInfo_.fill({});
  for (size_t i = 0; i < std::min<size_t>(books->size(), chapterInfo_.size()); ++i) {
    chapterInfo_[i] = readChapterInfo((*books)[i]);
  }
  thumbnailHeight = 0;
  coverLoadingSinceMs_ = 0;
  targetCoverStates.fill(CoverCacheState::Unchecked);
  fallbackCoverStates.fill(CoverCacheState::Unchecked);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  clearCoverCaches();
#endif
  requestUpdate();
}

void InxRecentActivity::onExit() {
  books = nullptr;
  bookStats.fill(nullptr);
  targetCoverStates.fill(CoverCacheState::Unchecked);
  fallbackCoverStates.fill(CoverCacheState::Unchecked);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  clearCoverCaches();
#endif
  thumbnailHeight = 0;
  Activity::onExit();
}

InxRecentActivity::CachedChapterInfo InxRecentActivity::readChapterInfo(const RecentBook& book) const {
  CachedChapterInfo info;
#ifdef BOARD_HAS_PSRAM
  char novelId[32] = {};
  if (!jjwxcUpdates::novelIdFromPath(book.path.c_str(), novelId, sizeof(novelId))) return info;
  JjwxcStore::TocSummary summary;
  uint32_t highest = 0;
  if (!JjwxcStore::readTocSummary(novelId, summary, highest)) return info;
  info.cached = summary.readable;
  info.total = summary.count;
#endif
  return info;
}

void InxRecentActivity::openSelected() {
  if (!books || books->empty()) return;
  // The continue card is books[0]; the recent row owns books[1..3].
  onSelectBook((*books)[0].path);
}

void InxRecentActivity::setThumbnailHeight(const int displayHeight) {
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  const int height = std::max(0, displayHeight);
#else
  const int height = InxCoverGeometry::thumbnailHeightForCropFill(displayHeight);
#endif
  if (thumbnailHeight == height) return;
  thumbnailHeight = height;
  targetCoverStates.fill(CoverCacheState::Unchecked);
  fallbackCoverStates.fill(CoverCacheState::Unchecked);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  clearCoverCaches();
#endif
}

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
void InxRecentActivity::clearCoverCaches() {
  std::for_each(targetCoverCaches.begin(), targetCoverCaches.end(), [](auto& cache) { cache = {}; });
  std::for_each(fallbackCoverCaches.begin(), fallbackCoverCaches.end(), [](auto& cache) { cache = {}; });
  cachedCoverBytes = 0;
}

InxRecentActivity::CoverCacheLoadResult InxRecentActivity::tryLoadCoverCache(HalFile& file, CoverRamCache& cache) {
  if (cache.attempted) return CoverCacheLoadResult::Stream;
  cache.attempted = true;

  const size_t fileSize = file.fileSize();
  if (fileSize == 0 || fileSize > MAX_CACHED_COVER_FILE_BYTES || cachedCoverBytes > MAX_COVER_CACHE_BYTES ||
      fileSize > MAX_COVER_CACHE_BYTES - cachedCoverBytes) {
    return CoverCacheLoadResult::Stream;
  }
  if (!memory::psramHasHeadroom(fileSize, fileSize, COVER_CACHE_MAX_ALLOC_RESERVE)) {
    return CoverCacheLoadResult::Stream;
  }

  // A cover is up to 64 KB, so it cannot live on the task stack. Allocate once
  // on first use and retain it for this Activity.
  auto bytes = memory::makePsramByteBufferNoThrow(fileSize);
  if (!bytes) return CoverCacheLoadResult::Stream;
  if (!file.seek(0) || file.read(bytes.get(), fileSize) != static_cast<int>(fileSize)) {
    LOG_ERR("INX", "Short read loading cover into PSRAM (%u bytes)", static_cast<unsigned>(fileSize));
    file.seek(0);
    return CoverCacheLoadResult::Stream;
  }

  Bitmap bitmap(bytes.get(), fileSize);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    return CoverCacheLoadResult::Invalid;
  }

  cache.bytes = std::move(bytes);
  cache.size = fileSize;
  cachedCoverBytes += fileSize;
  return CoverCacheLoadResult::Loaded;
}
#endif

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
bool InxRecentActivity::tryDrawBookCover(const std::string& path, const Rect& bounds, CoverCacheState& state,
                                         CoverRamCache& cache) {
#else
bool InxRecentActivity::tryDrawBookCover(const std::string& path, const Rect& bounds, CoverCacheState& state) {
#endif
  switch (state) {
    case CoverCacheState::Unchecked:
      if (!Storage.exists(path.c_str())) {
        state = CoverCacheState::Missing;
        return false;
      }
      state = CoverCacheState::Ready;
      break;
    case CoverCacheState::Ready:
      break;
    case CoverCacheState::Missing:
    case CoverCacheState::Unavailable:
      return false;
  }

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  const auto drawCached = [this, &bounds, &state, &cache] {
    Bitmap bitmap(cache.bytes.get(), cache.size);
    if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
      cachedCoverBytes -= cache.size;
      cache = {};
      state = CoverCacheState::Missing;
      return false;
    }
    return renderer.drawBitmapCropToFill(bitmap, bounds.x, bounds.y, bounds.width, bounds.height);
  };

  if (cache.bytes) return drawCached();
#endif

  HalFile file;
  if (!Storage.openFileForRead("INX", path, file)) {
    state = CoverCacheState::Missing;
    return false;
  }

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  switch (tryLoadCoverCache(file, cache)) {
    case CoverCacheLoadResult::Loaded:
      return drawCached();
    case CoverCacheLoadResult::Invalid:
      state = CoverCacheState::Missing;
      return false;
    case CoverCacheLoadResult::Stream:
      break;
  }
#endif

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    state = CoverCacheState::Missing;
    return false;
  }
  return renderer.drawBitmapCropToFill(bitmap, bounds.x, bounds.y, bounds.width, bounds.height);
}

bool InxRecentActivity::drawBookCover(const int bookIndex, const Rect& bounds) {
  renderer.fillRect(bounds.x, bounds.y, bounds.width, bounds.height, false);

  if (books && bookIndex >= 0 && bookIndex < static_cast<int>(books->size()) &&
      bookIndex < static_cast<int>(RecentBooksStore::MAX_RECENT_BOOKS) && thumbnailHeight > 0) {
    const RecentBook& book = (*books)[bookIndex];
    if (!book.coverBmpPath.empty()) {
      std::string path = UITheme::getCoverThumbPath(book.coverBmpPath, thumbnailHeight);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
      if (tryDrawBookCover(path, bounds, targetCoverStates[bookIndex], targetCoverCaches[bookIndex])) return true;
#else
      if (tryDrawBookCover(path, bounds, targetCoverStates[bookIndex])) return true;
#endif
      // Frozen value: the INX layout's cover thumbnail height (InxMetrics used
      // to provide it; the INX layouts are retired and this constant keeps the
      // remaining fallback path behaving identically until they are cut too).
      constexpr int kInxHomeCoverHeight = 226;
      if (book.coverBmpPath.find("[HEIGHT]") != std::string::npos && thumbnailHeight != kInxHomeCoverHeight) {
        path = UITheme::getCoverThumbPath(book.coverBmpPath, kInxHomeCoverHeight);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
        if (tryDrawBookCover(path, bounds, fallbackCoverStates[bookIndex], fallbackCoverCaches[bookIndex])) return true;
#else
        if (tryDrawBookCover(path, bounds, fallbackCoverStates[bookIndex])) return true;
#endif
      }
    }
  }

  // CPY: a quiet framed placeholder with a book glyph, not the retired INX
  // slab (a black block that read as a broken thumbnail). The title already
  // sits beside/below the cover, so repeating it here only duplicated text.
  if (UITheme::getInstance().usesCpyLayout()) {
    renderer.drawRoundedRect(bounds.x, bounds.y, bounds.width, bounds.height, 1, 12, true);
    constexpr int iconSize = 36;
    drawMaskIcon(renderer, icon_book_48, bounds.x + (bounds.width - iconSize) / 2,
                 bounds.y + (bounds.height - iconSize) / 2, iconSize);
    return false;
  }

  const auto size = InxCoverGeometry::fit(bounds.width, bounds.height);
  const int x = bounds.x + (bounds.width - size.width) / 2;
  const int y = bounds.y + (bounds.height - size.height) / 2;
  renderer.drawRect(x, y, size.width, size.height, 2, true);
  renderer.fillRect(x, y + size.height / 3, size.width, size.height * 2 / 3);
  constexpr int iconSize = 32;
  renderer.drawIcon(CoverIcon, x + (size.width - iconSize) / 2, y + size.height / 6 - iconSize / 2, iconSize);
  return false;
}

bool InxRecentActivity::prepareNextMissingCover() {
  if (!books || thumbnailHeight <= 0) return false;
  // Home only shows the continue book and three recent covers; generating
  // thumbnails for the other seven recents would delay the first paint.
  const size_t count = std::min<size_t>(books->size(), 4);
  for (size_t index = 0; index < count; ++index) {
    CoverCacheState& state = targetCoverStates[index];
    switch (state) {
      case CoverCacheState::Unchecked: {
        const std::string& templatePath = (*books)[index].coverBmpPath;
        if (templatePath.empty()) {
          state = CoverCacheState::Unavailable;
          continue;
        }
        state = Storage.exists(UITheme::getCoverThumbPath(templatePath, thumbnailHeight).c_str())
                    ? CoverCacheState::Ready
                    : CoverCacheState::Missing;
        if (state == CoverCacheState::Ready) continue;
        break;
      }
      case CoverCacheState::Ready:
      case CoverCacheState::Unavailable:
        continue;
      case CoverCacheState::Missing:
        break;
    }

    // Do not push a progress frame on the pass that follows onEnter(): that pass
    // draws the page with placeholder covers, and render() pushes the real page once
    // every cover has settled — two panel refreshes for one screen, which is the
    // double flash. Staying silent costs nothing (the page is already in the
    // framebuffer); the deadline below only bounds how long a slow generator may
    // leave the previous screen up.
    if (coverLoadingSinceMs_ == 0) coverLoadingSinceMs_ = millis();
    if (!coverProgressPushed_ && millis() - coverLoadingSinceMs_ > 700) {
      renderer.displayBuffer();
      coverProgressPushed_ = true;
    }
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      state = BookCoverLoader::ensureThumbnail((*books)[index].path, thumbnailHeight).empty()
                  ? CoverCacheState::Unavailable
                  : CoverCacheState::Ready;
    }
    requestUpdate();
    return true;
  }
  return false;
}


void InxRecentActivity::loop() { loopDashboard(); }

InxRecentActivity::DashLayout InxRecentActivity::dashboardLayout(const Rect& content) const {
  const int pad = UITheme::getInstance().getMetrics().contentSidePadding;
  const int x = content.x + pad;
  const int w = std::max(1, content.width - pad * 2);
  DashLayout out;
  int y = content.y + 4;
  // Continue card is sized by its content: cover / right column end at +270,
  // the wave takes the last 38 px. The freed height goes to the recent block
  // (larger covers + author lines), so the page still closes above the dock.
  out.firstCard = Rect{x, y, w, 318};
  y += out.firstCard.height + 12;
  out.todayCard = Rect{x, y, w, 140};
  y += out.todayCard.height + 12;
  out.recentCard = Rect{x, y, w, 370};
  out.recentAll = Rect{x + w - 156, y, 132, 44};
  constexpr int kColW = 168;
  constexpr int kColGap = 26;
  for (int i = 0; i < 3; ++i) {
    out.recentBooks[i] = Rect{x + 22 + i * (kColW + kColGap), y + 50, kColW, 300};
  }
  y += out.recentCard.height + 12;
  out.syncRow = Rect{x, y, w, 96};
  return out;
}

bool InxRecentActivity::syncRowVisible() const {
#ifdef BOARD_HAS_PSRAM
  return jjwxcUpdates::sessionPresent();
#else
  return false;
#endif
}

int InxRecentActivity::dashboardIndexFromPoint(const int x, const int y) const {
  const DashLayout rects = dashboardLayout(contentRect());
  const auto hit = [x, y](const Rect& r) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
  };
  if (hit(rects.firstCard)) return 0;
  if (hit(rects.todayCard)) return 1;
  for (int i = 0; i < 3; ++i) {
    if (hit(rects.recentBooks[i])) return 2 + i;
  }
  if (hit(rects.recentAll)) return 5;
  if (syncRowVisible() && hit(rects.syncRow)) return 6;
  return -1;
}

void InxRecentActivity::loopDashboard() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    dashboardActivate(selected);
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTouchDown(x, y)) {
    const int touched = dashboardIndexFromPoint(x, y);
    if (touched >= 0 && touched != selected) {
      selected = touched;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(x, y)) {
    const int touched = dashboardIndexFromPoint(x, y);
    if (touched >= 0) {
      selected = touched;
      dashboardActivate(touched);
    }
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  // Left/right no longer cycle a duplicate carousel; the recent row below is
  // the one place to pick another book. Up/down keeps moving dashboard slots.
  const int slotCount = syncRowVisible() ? kDashSlots : kDashSlots - 1;
  if (selected >= slotCount) selected = slotCount - 1;
  if (mappedInput.wasReleased(MappedInputManager::Button::NavNext) || swipe == MappedInputManager::SwipeDir::Up) {
    selected = (selected + 1) % slotCount;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
      swipe == MappedInputManager::SwipeDir::Down) {
    selected = (selected + slotCount - 1) % slotCount;
    requestUpdate();
    return;
  }
}

void InxRecentActivity::dashboardActivate(const int index) {
  switch (index) {
    case 0:
      openSelected();  // opens the selected recent book; no-op when the list is empty
      return;
    case 1:
      activityManager.goToReadingStats();
      return;
    case 2:
    case 3:
    case 4: {
      // Recent covers: slot 2..4 map to books 1..3. The continue card owns
      // books[0]; slot 2's "全部 N 本" header is handled in the touch path so
      // the whole card header opens the full recent list.
      const int bookIndex = index - 1;
      if (books != nullptr && bookIndex >= 0 && bookIndex < static_cast<int>(books->size())) {
        onSelectBook((*books)[bookIndex].path);
      }
      return;
    }
    case 5:
      activityManager.goToRecentBooks();
      return;
    case 6:
      checkJjwxcUpdates();
      return;
    default:
      return;
  }
}

void InxRecentActivity::checkJjwxcUpdates() {
#ifdef BOARD_HAS_PSRAM
  if (syncInFlight_ || !jjwxcUpdates::sessionPresent()) return;
  // Same constraint as the shelf's sync button: the online shelf walk may only
  // run in the app's PSRAM-stacked worker (a failed handshake on this task can
  // freeze it for ~60 s), so the app is started as a child and returns here.
  syncInFlight_ = true;
  JjwxcActivity::requestDeepLink("", JjwxcActivity::DeepLinkAction::RefreshUpdates);
  const bool started = startActivityForResultWith<JjwxcActivity>([this](const ActivityResult& result) {
    syncInFlight_ = false;
    jjwxcUpdates::invalidate();
    const auto* jjwxcResult = std::get_if<JjwxcReaderResult>(&result.data);
    const bool completed = jjwxcResult != nullptr && jjwxcResult->completed;
    LOG_INF("HOME", "JJWXC update check %s", completed ? "ok" : "failed");
    requestUpdate(true);
  });
  if (!started) {
    syncInFlight_ = false;
    JjwxcActivity::requestDeepLink("", JjwxcActivity::DeepLinkAction::None);
    LOG_ERR("HOME", "OOM: JJWXC update check activity");
  }
#endif
}

Rect InxRecentActivity::dashboardCoverRect(const Rect& content) const {
  const DashLayout rects = dashboardLayout(content);
  // Bottom aligns with the action row (right column ends at +270): the cover
  // and everything on its right share one bottom edge.
  return Rect{rects.firstCard.x + 24, rects.firstCard.y + 58, 140, 212};
}

void InxRecentActivity::drawDashboard(const Rect& content, const bool drawCoverArt) {
  const DashLayout rects = dashboardLayout(content);
  const bool hasBook = books != nullptr && !books->empty();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // ---- 01 continue card (604 x 400) ---------------------------------------
  GUI.drawCardFrame(renderer, rects.firstCard, selected == 0);
  if (!hasBook) {
    GUI.drawEmptyState(renderer,
                       Rect{rects.firstCard.x + 2, rects.firstCard.y + 2, rects.firstCard.width - 4,
                            rects.firstCard.height - 4},
                       tr(STR_NO_RECENT_BOOKS));
  } else {
    // One book only: the carousel duplicated the recent block below, so the
    // continue card always shows books[0] and the recent row owns the others.
    constexpr int kContinueIndex = 0;
    const Rect cover = dashboardCoverRect(content);
    setThumbnailHeight(cover.height);
    if (drawCoverArt) {
      drawBookCover(kContinueIndex, cover);
    } else {
      renderer.fillRect(cover.x, cover.y, cover.width, cover.height, false);
    }

    const int cardRight = rects.firstCard.x + rects.firstCard.width - 24;
    const int textX = cover.x + cover.width + 24;
    const int textW = std::max(1, cardRight - textX);
    const RecentBook& book = (*books)[kContinueIndex];
    const ReadingBookStats* stats = statsAt(kContinueIndex);
    const CachedChapterInfo info = chapterInfo_[kContinueIndex];
    const int pct = stats != nullptr ? static_cast<int>(stats->lastProgressPercent) : 0;

    // Card header: last-read position left, cached/progress right. The bottom
    // pill is the only "continue reading" affordance (the old text link
    // duplicated this label and its chevron did not sit on the text baseline).
    renderer.drawText(SMALL_FONT_ID, rects.firstCard.x + 24, rects.firstCard.y + 18,
                      tr(STR_LAST_READ_AT), true, EpdFontFamily::BOLD);
    char head[72];
    if (info.total > 0) {
      // Progress lives once, on the action bar; the header keeps the cache fact.
      std::snprintf(head, sizeof(head), I18N.get(StrId::STR_CACHED_CHAPTERS_FMT),
                    static_cast<unsigned>(info.cached), static_cast<unsigned>(info.total));
    } else {
      head[0] = '\0';
    }
    const int headW = renderer.getTextWidth(SMALL_FONT_ID, head);
    renderer.drawText(SMALL_FONT_ID, cardRight - headW, rects.firstCard.y + 18, head);

    // One type scale across the card: title 24 bold, author 18 regular, chapter
    // 20 bold. The serif title previously fell back to a smaller CJK face for
    // titles with glyphs outside its subset, which could make it smaller than
    // the author line.
    const std::string title = renderer.truncatedText(
        UI_12_FONT_ID, book.title.empty() ? book.path.c_str() : book.title.c_str(), textW,
        EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, textX, rects.firstCard.y + 58, title.c_str(), true,
                      EpdFontFamily::BOLD);

    std::string authorText = book.author;
    const char* source = sourceLabelForPath(book.path);
    if (!authorText.empty()) authorText += " · ";
    authorText += source;
    const std::string author = renderer.truncatedText(SMALL_FONT_ID, authorText.c_str(), textW);
    renderer.drawText(SMALL_FONT_ID, textX, rects.firstCard.y + 100, author.c_str());

    if (stats != nullptr && !stats->chapterTitle.empty()) {
      const std::string chapter = renderer.truncatedText(UI_10_FONT_ID, stats->chapterTitle.c_str(), textW);
      renderer.drawText(UI_10_FONT_ID, textX, rects.firstCard.y + 132, chapter.c_str(), true,
                        EpdFontFamily::BOLD);
    }

    // State pills: source (always real), finished state (derived from progress),
    // and the snapshot's new-chapter count when present.
    int pillX = textX;
    const int pillY = rects.firstCard.y + 170;
    const auto drawPill = [&](const char* label) {
      const int textWpx = renderer.getTextWidth(SMALL_FONT_ID, label);
      const int ph = 34;
      const int pw = textWpx + 22;
      renderer.fillRoundedRect(pillX, pillY, pw, ph, ph / 2, Color::White);
      renderer.drawRoundedRect(pillX, pillY, pw, ph, 2, ph / 2, true);
      renderer.drawText(SMALL_FONT_ID, pillX + 11, pillY + (ph - renderer.getLineHeight(SMALL_FONT_ID)) / 2, label);
      pillX += pw + 8;
    };
    drawPill(source);
    if (pct >= 100) {
      drawPill(tr(STR_BOOK_FINISHED));
    } else {
      drawPill(tr(STR_NOT_FINISHED));
    }
#ifdef BOARD_HAS_PSRAM
    const auto updateGap = jjwxcUpdates::gapForPath(book.path.c_str());
    if (updateGap.known && updateGap.newChapters > 0) {
      char tail[40];
      std::snprintf(tail, sizeof(tail), I18N.get(StrId::STR_JJWXC_HAS_NEW_FMT),
                    static_cast<unsigned>(std::min<uint32_t>(updateGap.newChapters, 9999)));
      drawPill(tail);
    }
#endif
    (void)metrics;

    // Primary action: the row directly under the 晋江/未读完 pills, in the text
    // column. The cover now ends on this row's bottom edge (+270), so there is
    // no blank square under the cover and no separate button-only row.
    constexpr int kActionH = 56;
    const int barX = textX;
    const int barW = std::max(1, cardRight - textX);
    const int actionY = rects.firstCard.y + 214;
    renderer.fillRoundedRect(barX, actionY, barW, kActionH, 12, Color::Black);
    const int resumeY = actionY + (kActionH - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    const char* resumeLabel = tr(STR_CONTINUE_READING);
    renderer.drawText(UI_10_FONT_ID, barX + 24, resumeY, resumeLabel, false, EpdFontFamily::BOLD);
    const int arrowX = barX + 24 + renderer.getTextWidth(UI_10_FONT_ID, resumeLabel, EpdFontFamily::BOLD) + 12;
    const int arrowY = resumeY + renderer.getLineHeight(UI_10_FONT_ID) / 2;
    renderer.drawLine(arrowX, arrowY - 6, arrowX + 7, arrowY, false);
    renderer.drawLine(arrowX + 7, arrowY, arrowX, arrowY + 6, false);
    char pctText[8];
    std::snprintf(pctText, sizeof(pctText), "%d%%", pct);
    const int pctW = renderer.getTextWidth(UI_10_FONT_ID, pctText, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, barX + barW - 24 - pctW, resumeY, pctText, false, EpdFontFamily::BOLD);

    // Full-width wave progress at the card's foot (design frame 01).
    drawWaveProgress(renderer,
                     Rect{rects.firstCard.x + 2, rects.firstCard.y + rects.firstCard.height - 38,
                          rects.firstCard.width - 4, 34},
                     pct);
  }

  // ---- 02 readings strip: four metrics + seven bars ------------------------
  GUI.drawCardFrame(renderer, rects.todayCard, selected == 1);
  {
    const uint64_t todayMs = READING_STATS.getTodayReadingMs();
    const uint64_t weekTotalMs = READING_STATS.getRecentReadingMs(7);
    const uint32_t streak = READING_STATS.getCurrentStreakDays();
    const uint64_t goalMs = getDailyReadingGoalMs();
    char values[4][24];
    std::snprintf(values[0], sizeof(values[0]), "%llum", static_cast<unsigned long long>(todayMs / 60000));
    // Compact form: the strip has four 100 px columns and a full "28h 37m"
    // collides with the next value. The same duration is shown in full on the
    // stats page.
    const uint64_t weekMinutes = weekTotalMs / 60000;
    if (weekMinutes >= 600) {
      std::snprintf(values[1], sizeof(values[1]), "%lluh", static_cast<unsigned long long>(weekMinutes / 60));
    } else if (weekMinutes >= 60) {
      std::snprintf(values[1], sizeof(values[1]), "%lluh%02llu",
                    static_cast<unsigned long long>(weekMinutes / 60),
                    static_cast<unsigned long long>(weekMinutes % 60));
    } else {
      std::snprintf(values[1], sizeof(values[1]), "%llum", static_cast<unsigned long long>(weekMinutes));
    }
    std::snprintf(values[2], sizeof(values[2]), "%ud", static_cast<unsigned>(streak));
    std::snprintf(values[3], sizeof(values[3]), "%llum", static_cast<unsigned long long>(goalMs / 60000));
    const freeink::Icon* icons[4] = {&icon_clock_48, &icon_chart_line_48, &icon_zap_48, &icon_target_48};
    const StrId labels[4] = {StrId::STR_DASH_TODAY, StrId::STR_DASH_WEEK, StrId::STR_STREAK_SHORT,
                             StrId::STR_GOAL_SHORT};
    constexpr int kColW = 100;
    for (int i = 0; i < 4; ++i) {
      const int cx = rects.todayCard.x + 16 + i * kColW;
      drawMaskIcon(renderer, *icons[i], cx, rects.todayCard.y + 16, 24);
      renderer.drawText(UI_12_FONT_ID, cx, rects.todayCard.y + 52, values[i], true, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, cx, rects.todayCard.y + 90, I18N.get(labels[i]));
    }

    // Seven slim columns on the right, today in black.
    const int barAreaX = rects.todayCard.x + rects.todayCard.width - 148;
    const int barAreaW = 128;
    const int baseline = rects.todayCard.y + 88;
    constexpr int kBarW = 12;
    constexpr int kGap = 6;
    std::array<uint64_t, 7> weekMs{};
    const time_t now = time(nullptr);
    const bool weekWindow = now > 1700000000;
    uint32_t todayOrdinal = 0;
    uint64_t maxMs = 1;
    if (weekWindow) {
      todayOrdinal = TimeUtils::getLocalDayOrdinal(static_cast<uint32_t>(now));
      for (const auto& day : READING_STATS.getReadingDays()) {
        if (day.dayOrdinal <= todayOrdinal && todayOrdinal - day.dayOrdinal < weekMs.size()) {
          weekMs[todayOrdinal - day.dayOrdinal] += day.readingMs;
        }
      }
      for (const uint64_t ms : weekMs) maxMs = std::max(maxMs, ms);
    }
    for (int i = 0; i < 7; ++i) {
      const size_t daysAgo = weekWindow ? weekMs.size() - 1 - static_cast<size_t>(i) : 0;
      const uint64_t ms = weekWindow ? weekMs[daysAgo] : 0;
      const int h = ms == 0 ? 4 : 4 + static_cast<int>(ms * 46 / maxMs);
      const int bx = barAreaX + i * (kBarW + kGap);
      if (i == 6) {
        renderer.fillRoundedRect(bx, baseline - h, kBarW, h, 3, Color::Black);
      } else {
        renderer.fillRoundedRect(bx, baseline - h, kBarW, h, 3, Color::DarkGray);
      }
    }
    renderer.drawText(SMALL_FONT_ID, barAreaX, rects.todayCard.y + 90, tr(STR_DASH_LAST7));
  }

  // ---- 03 recent covers (604 x 288) ---------------------------------------
  GUI.drawCardFrame(renderer, rects.recentCard, selected == 2 || selected == 3 || selected == 4);
  {
    renderer.drawText(SMALL_FONT_ID, rects.recentCard.x + 22, rects.recentCard.y + 16,
                      tr(STR_HOME_RECENT_READING), true, EpdFontFamily::BOLD);
    const int total = books != nullptr ? static_cast<int>(books->size()) : 0;
    char all[48];
    std::snprintf(all, sizeof(all), I18N.get(StrId::STR_ALL_BOOKS_FMT), static_cast<unsigned>(total));
    const int allW = renderer.getTextWidth(SMALL_FONT_ID, all);
    const int chevX = rects.recentCard.x + rects.recentCard.width - 20;
    const int allX = chevX - 10 - allW;
    renderer.drawText(SMALL_FONT_ID, allX, rects.recentCard.y + 16, all);
    renderer.drawLine(chevX, rects.recentCard.y + 20, chevX + 6, rects.recentCard.y + 26, true);
    renderer.drawLine(chevX + 6, rects.recentCard.y + 26, chevX, rects.recentCard.y + 32, true);

    bool anyExtra = false;
    for (int i = 0; i < 3; ++i) {
      const int bookIndex = i + 1;
      if (books == nullptr || bookIndex >= static_cast<int>(books->size())) continue;
      anyExtra = true;
      const Rect item = rects.recentBooks[i];
      const Rect cover{item.x, item.y, 132, 184};
      if (drawCoverArt) {
        drawBookCover(bookIndex, cover);
      } else {
        renderer.fillRect(cover.x, cover.y, cover.width, cover.height, false);
      }
      const RecentBook& recent = (*books)[bookIndex];
      const ReadingBookStats* stats = statsAt(bookIndex);
      const std::string title = renderer.truncatedText(
          SMALL_FONT_ID, recent.title.empty() ? recent.path.c_str() : recent.title.c_str(), item.width,
          EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, item.x, item.y + 192, title.c_str(), true, EpdFontFamily::BOLD);
      if (!recent.author.empty()) {
        const std::string author =
            renderer.truncatedText(SMALL_FONT_ID, recent.author.c_str(), item.width);
        renderer.drawText(SMALL_FONT_ID, item.x, item.y + 220, author.c_str());
      } else {
        // Keep the three-line rhythm when a local book has no author: the
        // source is the one true byline we have for it.
        const std::string source =
            renderer.truncatedText(SMALL_FONT_ID, sourceLabelForPath(recent.path), item.width);
        renderer.drawText(SMALL_FONT_ID, item.x, item.y + 220, source.c_str());
      }
      char meta[64] = {};
      const int pct = stats != nullptr ? static_cast<int>(stats->lastProgressPercent) : 0;
      const std::string when = stats != nullptr ? relativeDayLabel(stats->lastReadAt) : std::string{};
      if (!when.empty()) {
        std::snprintf(meta, sizeof(meta), "%d%% · %s", pct, when.c_str());
      } else {
        std::snprintf(meta, sizeof(meta), "%d%%", pct);
      }
      renderer.drawText(SMALL_FONT_ID, item.x, item.y + 248, meta);
    }
    if (!anyExtra) {
      UITheme::drawCenteredText(renderer, Rect{rects.recentCard.x, rects.recentCard.y + 110,
                                               rects.recentCard.width, 40},
                                SMALL_FONT_ID, rects.recentCard.y + 118, tr(STR_NO_RECENT_BOOKS));
    }
  }

  // ---- 04 update row (604 x 96) -------------------------------------------
  if (syncRowVisible()) {
    const Rect& row = rects.syncRow;
    GUI.drawCardFrame(renderer, row, selected == 6);
    drawMaskIcon(renderer, icon_refresh_cw_48, row.x + 24, row.y + (row.height - 28) / 2, 28);
    const int textX = row.x + 24 + 28 + 18;
    // Secondary action: 28 px bold sits below the 32 px book title in the
    // scale, instead of competing with it.
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, textX, row.y + (row.height - lineH) / 2,
                      I18N.get(StrId::STR_JJWXC_CHECK_UPDATES_ACTION), true, EpdFontFamily::BOLD);
    const std::string action = tr(STR_CHECK_SHORT);
    const int actionW = renderer.getTextWidth(SMALL_FONT_ID, action.c_str(), EpdFontFamily::BOLD);
    constexpr int kPillH = 48;
    const int pillW = actionW + 48;
    const int pillX = row.x + row.width - 24 - pillW;
    const int pillY = row.y + (row.height - kPillH) / 2;
    renderer.fillRoundedRect(pillX, pillY, pillW, kPillH, kPillH / 2, Color::White);
    renderer.drawRoundedRect(pillX, pillY, pillW, kPillH, 2, kPillH / 2, true);
    renderer.drawText(SMALL_FONT_ID, pillX + (pillW - actionW) / 2,
                      pillY + (kPillH - renderer.getLineHeight(SMALL_FONT_ID)) / 2, action.c_str(), true,
                      EpdFontFamily::BOLD);
  }
}

void InxRecentActivity::render(RenderLock&&) {
  // One full-page 16-level pass. The cover keeps its Gray2 tones and the tile
  // icons get their four levels in the same canvas. Partial-area scans are
  // deliberately not used: on this raw LCD panel a partial scan can latch the
  // source/gate driver state and produce a horizontal smear that only a reboot
  // clears.
  const bool gray16 = renderer.beginGrayscale16();
  if (!gray16) renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  // The band title is the page name, not the underlying activity's old name:
  // CPY's home is the dashboard (frame 02 titles it 首页, matching the tab).
  drawPageHeader(Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_HOME_SHORTCUT));
  const Rect content = contentRect();

  if (content.width > 0 && content.height > 0) {
    const GfxRenderer::ClipScope clip(renderer, content.x, content.y, content.width, content.height);
    drawDashboard(content, /*drawCoverArt=*/true);
  }

  const auto labels = mainTabButtonLabels(SETTINGS.standbyShortcutEnabled ? tr(STR_STANDBY_TITLE) : "", tr(STR_OPEN),
                                          books && books->size() > 1, false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (usesMainTabBar() && mappedInput.hasTouch() && !mainTabsAtBottom()) {
    int top, right, bottom, left;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    const int footerTop = renderer.getScreenHeight() - InxRecentGeometry::footerReservedHeight;
    GUI.drawMainTabStatusBar(renderer, Rect{left, footerTop, std::max(0, width - left - right),
                                            std::max(0, renderer.getScreenHeight() - bottom - footerTop)});
  } else if (!hasMainTabStatusBar()) {
    Rect battery = InxRecentGeometry::batteryRect(safeArea);
    if (mainTabsAtBottom()) battery.y = safeArea.y + metrics.topPadding + 5;
    GUI.drawBatteryRight(renderer, battery,
                         SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);
  }
  if (prepareNextMissingCover()) {
    // Do not push a half-built cover page; the next pass re-begins the canvas.
    if (gray16) renderer.cancelGrayscale16();
    return;
  }
  // A tap that already queued another screen makes this frame dead on arrival:
  // paying its ~0.4 s panel scan would be pure latency on the way to the one the
  // user asked for (rapid Home <-> Library switching).
  if (activityManager.isSwitchPending()) {
    if (gray16) renderer.cancelGrayscale16();
    return;
  }
  if (gray16) {
    renderer.commitGrayscale16(false);
  } else {
    renderer.displayBuffer();
  }
  coverProgressPushed_ = false;  // next pass gets its own single progress push
  coverLoadingSinceMs_ = 0;      // and its own deadline
}
