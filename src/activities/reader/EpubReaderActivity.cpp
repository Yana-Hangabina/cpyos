#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/FontManifest.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/css/CssParser.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Print.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFontCache.h>
#include <SdCardFontSystem.h>
#include <TtfEpdFont.h>
#include <TrustedTime.h>
#include <Txt.h>
#include <WiFi.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <iterator>
#include <limits>

#include "AchievementsStore.h"
#include "BleInput.h"
#include <ChangedRegion.h>
#include <Fb2.h>
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnoteSelectActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderFontSizes.h"
#include "ReaderToolbarUi.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/FontPreloadView.h"
#include "util/HtmlToPlainText.h"
#include "util/BookCacheUtils.h"
#include "util/BookmarkFile.h"
#include "util/ReadingBackground.h"
#include "util/ReadingGuideLine.h"
#ifdef ENABLE_CHINESE_VERSION
#include <WeReadStore.h>

#include "activities/apps/weread/WeReadProgressSyncActivity.h"
#include "activities/settings/FontDownloadActivity.h"
#endif
#include "components/UITheme.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"
#include "util/AchievementPopupUtils.h"
#include "util/BookmarkUtil.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"
#include "util/TimeUtils.h"

#ifdef BOARD_HAS_PSRAM
#include "activities/apps/jjwxc/JjwxcActivity.h"
#include "activities/apps/jjwxc/JjwxcUpdates.h"
#include "activities/apps/jjwxc/JjwxcCommentActivity.h"
#include "activities/apps/jjwxc/JjwxcStore.h"
#endif

namespace {
#ifdef BOARD_HAS_PSRAM
// Defined next to the JJWXC comment code lower in this file; onExit() runs
// before it and needs the same /JJWXC/<title>-<id>.epub test.
bool parseJjwxcNovelIdFromPath(const std::string& path, char* out, size_t cap);
#endif
constexpr uint8_t MAX_PAGE_TURN_RATE = 30;
// The X4 Pro and X4 Classic carry the X4's panel but sit outside isXteinkDevice()
// (that helper also gates power management). Overlay refresh choices are per-panel:
// this family runs the grayscale anti-aliasing pass, so chrome painted over a
// fresh page needs the HALF ghost-cleanup and closing re-renders the page.
// EEGO A4 runs the same grayscale AA pass on its UC8279C 4-gray panel, so its
// overlay paints need the identical full-waveform treatment: a FAST differential
// after the AA waveform leaves the covered page ghosting through the chrome.
bool xteinkClassPanel() {
  return gpio.isXteinkDevice() || BoardConfig::isX4Pro() || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_EEGO_A4;
}

constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

// refreshed UI reader chrome (frame 04). The header uses the reader's status face
// (a vector face on Read Pico, the small UI face elsewhere); the numeric face
// is the same tabular one the old status bar used for clock and page counts.
#if FREEINK_DEVICE_READPICO
constexpr int kChromeTextFont = READER_STATUS_FONT_ID;
constexpr int kChromeNumericFont = BaseTheme::STATUS_NUMERIC_FONT_ID;
#else
constexpr int kChromeTextFont = SMALL_FONT_ID;
constexpr int kChromeNumericFont = UI_10_FONT_ID;
#endif
// The 28x72 bookmark ribbon hangs 16 px from the right edge; the header's
// right cluster stops clear of it instead of running underneath.
constexpr int kChromeRibbonReserve = 16 + 28 + 12;
constexpr int kChromeWaveHeight = 18;
constexpr int kChromeHeaderGap = 8;   // air between the header rule and the body
constexpr int kChromeFooterGap = 8;   // air between the body and the wave
constexpr int kChromeWaveToPages = 10;  // air between the wave and its page numbers

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Protected-book sidecars travel with the book; a protected book separated
  // from its key no longer opens, so a failed move rolls everything back.
  static constexpr const char* SIDECARS[] = {".key", ".rights"};
  for (size_t i = 0; i < std::size(SIDECARS); i++) {
    const std::string from = srcPath + SIDECARS[i];
    if (!Storage.exists(from.c_str())) continue;
    const std::string to = dstPath + SIDECARS[i];
    if (Storage.rename(from.c_str(), to.c_str())) continue;
    LOG_ERR("ERS", "Failed to move sidecar %s -> %s", from.c_str(), to.c_str());
    for (size_t j = 0; j < i; j++) {
      Storage.rename((dstPath + SIDECARS[j]).c_str(), (srcPath + SIDECARS[j]).c_str());
    }
    if (!Storage.rename(dstPath.c_str(), srcPath.c_str())) {
      LOG_ERR("ERS", "Failed to restore epub after sidecar move failure: %s -> %s", dstPath.c_str(), srcPath.c_str());
    }
    return;
  }

  const bool artifactsOk = relocateBookArtifacts(srcPath, dstPath);
  const bool referencesOk = relocateBookReferences(srcPath, dstPath);
  if (!artifactsOk || !referencesOk) {
    LOG_ERR("ERS", "Finished-book data migration incomplete: %s -> %s", srcPath.c_str(), dstPath.c_str());
  }
}

int getChapterTocIndex(const Epub& epub, const int spineIndex, std::optional<uint32_t> visibleOffset,
                       uint8_t* chapterProgress = nullptr) {
  if (Txt::isTxtOrMd(epub.getSourcePath()) && visibleOffset) {
    const int index = Txt::tocIndexForPosition(epub.getSourcePath(), epub.getCachePath(), *visibleOffset, chapterProgress);
    if (index >= 0 && index < epub.getTocItemsCount()) return index;
  }
  return epub.getTocIndexForSpineIndex(spineIndex);
}

std::string getStatsChapterTitle(const Epub& epub, const int spineIndex, std::optional<uint32_t> visibleOffset,
                                 uint8_t* chapterProgress = nullptr) {
  int tocIndex = getChapterTocIndex(epub, spineIndex, visibleOffset, chapterProgress);
  if (tocIndex < 0) {
    int nearestTocIndex = -1;
    int nearestSpineIndex = -1;
    for (int index = 0; index < epub.getTocItemsCount(); ++index) {
      const int tocSpineIndex = epub.getSpineIndexForTocIndex(index);
      if (tocSpineIndex <= spineIndex && tocSpineIndex >= nearestSpineIndex) {
        nearestSpineIndex = tocSpineIndex;
        nearestTocIndex = index;
      }
    }
    tocIndex = nearestTocIndex;
  }
  return tocIndex < 0 ? "" : epub.getTocItem(tocIndex).title;
}

uint8_t getStatsChapterProgressPercent(const int currentPage, const int pageCount) {
  if (pageCount <= 0) return 0;
  return static_cast<uint8_t>(clampPercent(
      static_cast<int>((static_cast<float>(currentPage + 1) / static_cast<float>(pageCount)) * 100.0f + 0.5f)));
}

}  // namespace

bool EpubReaderActivity::needsPartialRebuild() const {
  return section && !section->isBuilding() && section->isPartial() && failedBuildSpine_ != currentSpineIndex &&
         section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount);
}

bool EpubReaderActivity::deferBluetoothStart() const {
  // Preparation blocks a new host, not an existing reader/menu connection.
  return !section || section->isBuilding() || section->pageCount == 0 || needsPartialRebuild();
}

void EpubReaderActivity::prepareChapterBuild() {
  // C3 and PSRAM hosts retain existing links; parser memory guards still apply.
#if FREEINK_CAP_BLE_HID_HOST && !CROSSPOINT_BLE_HOST_PSRAM && !CONFIG_IDF_TARGET_ESP32C3
  bleinput::stop();
#endif
#if CONFIG_IDF_TARGET_ESP32C3 && FREEINK_CAP_BLE_HID_HOST
  // Reclaim arenas before parser allocations interleave with them; retain the
  // selected font and coverage index for both layout and subsequent reading.
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
#endif
}

bool EpubReaderActivity::updateChapterBuild() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return true;
  if (!section) return true;

  if (needsPartialRebuild() && buildViewportWidth > 0) {
    const ReaderRenderSpec buildSpec = effectiveRenderSpec(buildViewportWidth, buildViewportHeight);
    prepareChapterBuild();
    if (!section->startBuild(buildSpec)) {
      if (!handleBuildFailure("deferred partial start", section->buildError())) requestUpdate();
      return false;
    }
    LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
            section->pageCount);
  }
  if (!section->isBuilding()) return true;

  bool suspendForBluetooth = false;
#if CONFIG_IDF_TARGET_ESP32C3 && FREEINK_CAP_BLE_HID_HOST
  suspendForBluetooth = SETTINGS.bluetoothEnabled;
#endif
  // A paused parser still owns its heap. Build beyond the restart margin, then
  // persist a partial cache and release the parser to bound its lifetime.
  const int buildWindow = BUILD_WINDOW_AHEAD + (suspendForBluetooth ? PARTIAL_REBUILD_START_MARGIN : 0);
  const bool pendingReposition = cachedVisibleTextOffset.has_value() || cachedChapterTotalPageCount != 0;
  if (((!suspendForBluetooth && section->isPartial()) || (suspendForBluetooth && pendingReposition) ||
       static_cast<int>(section->pageCount) < section->currentPage + buildWindow) &&
      buildTickHeapGate()) {
    if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
      if (!handleBuildFailure("background build", section->buildError())) requestUpdate();
      return false;
    }
    if (section->isBuildComplete() && applyDeferredReposition()) requestUpdate();
  }

  if (suspendForBluetooth && section->isBuilding() && !pendingReposition &&
      static_cast<int>(section->pageCount) >= section->currentPage + buildWindow) {
    section->suspendBuild();
    if (section->buildError() != Section::BuildError::None) {
      if (!handleBuildFailure("partial suspend", section->buildError())) requestUpdate();
      return false;
    }
  }
  return true;
}

EpubReaderActivity::~EpubReaderActivity() {
  // Idempotent backstop: onExit() normally already released the embedded font.
  releaseEmbeddedFont();
  ImageBlock::setExtractor(nullptr, nullptr);
  settleOverlayRefresh();
  discardOverlayPage();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getSourcePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::onExit() {
  waterTurnDirection = 0;  // 未消费的动画请求不跨 Activity 存活 / an unconsumed request never outlives the reader
  // A5: drop the book's embedded font (if any) before anything else is torn down,
  // so the renderer never keeps a pointer to a freed face.
  releaseEmbeddedFont();
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  READING_STATS.endSession();
  ACHIEVEMENTS.recordSessionEnded(READING_STATS.getLastSessionSnapshot());
#if FREEINK_DEVICE_READPICO
  // Four frames of PSRAM; the reader is the only owner and gives them back here.
  freePageCache();
#endif
#ifdef BOARD_HAS_PSRAM
  {
    // The reader decompresses spine items and lays out pages into plaintext
    // files under /.crosspoint/epub_*/. Those are a copy of the book that bypasses
    // any protection on the EPUB itself, so they must not outlive the session.
    // Rebuilt on the next open; the cost is one re-index, the alternative is a
    // plaintext chapter cache on the card.
    char novelId[16];
    if (epub && parseJjwxcNovelIdFromPath(epub->getSourcePath(), novelId, sizeof(novelId))) {
      section.reset();
      const std::string cache = epub->getCachePath();
      Storage.removeDir((cache + "/sections").c_str());
      Storage.removeDir((cache + "/html").c_str());
      LOG_DBG("ERS", "Removed derived plaintext caches for JJWXC book %s", novelId);
    }
  }
#endif
  showPendingAchievementPopups(renderer);
  ReaderActivity::onExit();
#if FREEINK_DEVICE_EEGO_A4
  // EEGO uses a single-pass grayscale page; force a clean first frame after exit.
  renderer.requestNextFullRefresh();
#endif
}

#if CROSSPOINT_VECTOR_FONTS
namespace {
// Reads an uncompressed zip member into a PSRAM buffer that grows by doubling. A
// body font is megabytes, so this must never touch internal RAM; doubling keeps
// the total copy at O(n), and the cap stops a corrupt or hostile entry from
// exhausting PSRAM.
class PsramGrowingSink final : public Print {
 public:
  static constexpr uint32_t kMaxBytes = 6u * 1024u * 1024u;

  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (size == 0) return 0;
    if (size_ + size > kMaxBytes) {
      overflowed_ = true;
      return 0;
    }
    if (!ensure(static_cast<uint32_t>(size_ + size))) return 0;
    memcpy(buf_.get() + size_, data, size);
    size_ += size;
    return size;
  }

  bool overflowed() const { return overflowed_; }
  uint32_t size() const { return size_; }
  memory::ByteBuffer take() {
    size_ = cap_ = 0;
    return std::move(buf_);
  }

 private:
  bool ensure(uint32_t need) {
    if (buf_ && cap_ >= need) return true;
    uint32_t next = cap_ ? cap_ : 64u * 1024u;
    while (next < need) next *= 2;
    if (next > kMaxBytes) next = kMaxBytes;
    auto grown = memory::makePsramByteBufferUninitializedNoThrow(next);
    if (!grown) return false;
    if (buf_) memcpy(grown.get(), buf_.get(), size_);
    buf_ = std::move(grown);
    cap_ = next;
    return true;
  }
  memory::ByteBuffer buf_;
  uint32_t size_ = 0;
  uint32_t cap_ = 0;
  bool overflowed_ = false;
};

}  // namespace
#endif

void EpubReaderActivity::loadEmbeddedFontIfPresent() {
#if CROSSPOINT_VECTOR_FONTS
  if (!epub) return;
  // The OPF-derived list is only built while the metadata cache is generated;
  // Epub::load() short-circuits from that cache on later opens, so fall back to
  // the package's own file list. This is what made the font silently disappear
  // after the first open on the device.
  epub->discoverEmbeddedFontsFromZip();
  if (epub->getEmbeddedFonts().empty()) return;

  const auto pointSize = SETTINGS.fontPointSize;
  const bool havePsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
  const size_t cacheBytes = havePsram ? 1024 * 1024 : 32 * 1024;
  const uint16_t maxGlyphs = havePsram ? 4096 : 768;
  const bool fourBit = renderer.getGrayscaleLevels() == 16;

  for (const auto& href : epub->getEmbeddedFonts()) {
    PsramGrowingSink sink;
    if (!epub->readItemContentsToStream(href, sink, 4096, /*allowEarlyStop=*/false, /*psramChunkSize=*/64 * 1024)) {
      LOG_DBG("ERS", "Embedded font read failed: %s", href.c_str());
      continue;
    }
    const uint32_t fontBytes = sink.size();
    if (sink.overflowed() || fontBytes < 512) {
      LOG_DBG("ERS", "Embedded font rejected (size %u): %s", static_cast<unsigned>(fontBytes), href.c_str());
      continue;
    }
    auto face = makeUniqueNoThrow<TtfEpdFont>();
    if (!face) return;
    auto bytes = sink.take();
    face->addResidentSource(TtfEpdFont::Regular, bytes.get(), fontBytes);
    if (!face->load(pointSize, /*twoBit=*/true, cacheBytes, maxGlyphs, fourBit)) {
      LOG_DBG("ERS", "Embedded font parse failed: %s", href.c_str());
      continue;
    }
    face->build(" ");
    const std::string family = epubfont::familyFromHref(href);
    const int id = readerVectorFontId((family + "\x01epub").c_str(), static_cast<uint8_t>(pointSize));
    renderer.insertFont(id, face->family());
    renderer.registerTtfFont(id, face.get());
    embeddedFontBytes_ = std::move(bytes);
    embeddedFont_ = face.release();
    embeddedFontId_ = id;
    SETTINGS.readerFontOverrideId = id;
    LOG_DBG("ERS", "Embedded EPUB font: %s @%upt id=%d (%u bytes)", family.c_str(), static_cast<unsigned>(pointSize), id,
            static_cast<unsigned>(fontBytes));
    return;
  }
#endif
}

void EpubReaderActivity::releaseEmbeddedFont() {
#if CROSSPOINT_VECTOR_FONTS
  if (embeddedFontId_ != 0) {
    // Order mirrors SdCardFontSystem: drop the renderer's references first, then
    // free the face (its FreeType faces read the resident bytes), then the bytes.
    renderer.unregisterTtfFont(embeddedFontId_);
    renderer.removeFont(embeddedFontId_);
    embeddedFontId_ = 0;
  }
  SETTINGS.readerFontOverrideId = 0;
  delete embeddedFont_;
  embeddedFont_ = nullptr;
  embeddedFontBytes_.reset();
#endif
}

bool EpubReaderActivity::loadBook() {
  std::string contentPath = bookPath;
  const bool isFb2 = FsHelpers::hasFb2Extension(bookPath);
  if (isFb2) {
    // FB2 is converted once into a store-only EPUB under /.crosspoint; the
    // reader then uses the normal EPUB path, while progress/bookmarks/recents
    // stay keyed to the .fb2 the user sees on the card.
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    Fb2 converter(bookPath, "/.crosspoint");
    if (!converter.ensureEpub()) {
      LOG_ERR("ERS", "Failed to convert FB2: %s", bookPath.c_str());
      loadProtectionError = "fb2 conversion failed";
      return false;
    }
    contentPath = converter.getEpubPath();
  }
  auto loadedEpub = makeUniqueNoThrow<Epub>(contentPath, bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
#if FREEINK_CAP_BLE_HID_HOST && !CROSSPOINT_BLE_HOST_PSRAM && !CONFIG_IDF_TARGET_ESP32C3
    bleinput::stop();
#endif
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (!loaded) {
    // Surfaced by handleLoadFailure() as a dialog; loadedEpub dies with this
    // scope, so carry the reason out in a member.
    loadProtectionError = loadedEpub->getProtectionError();
    LOG_ERR("ERS", "Failed to load EPUB%s%s", loadProtectionError.empty() ? "" : ": ", loadProtectionError.c_str());
    return false;
  }
  epub = std::move(loadedEpub);

  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest, CancelCheck cancellation) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest, cancellation);
  });

  epub->setupCacheDir();

#ifdef ENABLE_CHINESE_VERSION
  wereadBookId_[0] = '\0';
  if (WeReadStore::findBookIdForPath(epub->getSourcePath(), wereadBookId_, sizeof(wereadBookId_)) &&
      strncmp(wereadBookId_, "MP_WXS_", 7) == 0) {
    wereadBookId_[0] = '\0';
  }
  if (wereadBookId_[0]) {
    const uint32_t timestamp = TimeUtils::getCurrentValidTimestamp();
    if (timestamp != 0) {
      switch (WeReadStore::promoteShelfBook(wereadBookId_, timestamp)) {
        case WeReadStore::ShelfSortResult::Ok:
          break;
        case WeReadStore::ShelfSortResult::Degraded:
          LOG_DBG("WR", "Large shelf: recent book promotion deferred until sync");
          break;
        case WeReadStore::ShelfSortResult::StorageError:
          LOG_ERR("WR", "Failed to promote recently opened shelf book");
          break;
      }
    }
  }
#endif
  bool hasSavedProgress = false;

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      hasSavedProgress = true;
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }

  if (!hasSavedProgress && Txt::isTxtOrMd(bookPath)) {
    uint32_t visibleOffset = 0;
    switch (Txt::restoreLegacyProgress(bookPath, epub->getCachePath(), visibleOffset)) {
      case txt_progress::LegacyResult::Absent:
        break;
      case txt_progress::LegacyResult::Restored:
        currentSpineIndex = cachedSpineIndex = 0;
        nextPageNumber = 0;
        cachedVisibleTextOffset = pendingOffsetJump = visibleOffset;
        legacyProgressPending = true;
        break;
      case txt_progress::LegacyResult::Failed:
        legacyProgressPending = true;
        loadProtectionError = "txt progress migration failed";
        return false;
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

#ifdef ENABLE_CHINESE_VERSION
  if (wereadBookId_[0]) {
    float initialProgress = 0.0f;
    const bool loaded = WeReadStore::loadInitialProgress(wereadBookId_, initialProgress);
    if (hasSavedProgress || !loaded || initialProgress <= 0.0f) {
      WeReadStore::clearInitialProgress(wereadBookId_);
    } else if (jumpToFraction(initialProgress)) {
      clearInitialProgressAfterSave_ = true;
    } else {
      WeReadStore::clearInitialProgress(wereadBookId_);
    }
  }
#endif

  READING_STATS.beginSession(
      epub->getSourcePath(), epub->getTitle(), epub->getAuthor(), epub->getCoverBmpPath(),
      clampPercent(static_cast<int>(epub->calculateProgress(currentSpineIndex, 0.0f) * 100.0f + 0.5f)),
      getStatsChapterTitle(*epub, currentSpineIndex, cachedVisibleTextOffset), 0);

  loadLinkStack();
  loadCachedBookmarks();
  // A5: honour a font carried inside the package, if any (and if the user has not
  // chosen an SD font). Done last so the override applies to the first render.
  loadEmbeddedFontIfPresent();
  return true;
}

ChapterPosition EpubReaderActivity::chapterPosition() const {
  if (section) return {section->currentPage, section->estimatedTotalPages()};
  return {nextPageNumber, cachedChapterTotalPageCount};
}

int EpubReaderActivity::bookPercentFor(const ChapterPosition& position) const {
  if (!epub || epub->getBookSize() == 0 || !position.hasTotal()) return 0;
  // The page index can run past the chapter's estimated total while it is still
  // building, so the fraction is clamped before the cast.
  const float fraction = epub->calculateProgress(currentSpineIndex, position.chapterFraction());
  return static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 100.0f + 0.5f);
}

void EpubReaderActivity::openReaderMenu() {
  clearPendingTurns();
  if (usesToolbarMenu()) {
    // Reached from a child activity's result handler (footnotes, bookmarks,
    // go-to-percent... cancelled back to the menu), so the framebuffer holds
    // that screen, not the page: re-render the page and let renderBook() put
    // the toolbar on top. The in-reader fast path is openOverlay().
    overlay = Overlay::Toolbar;
    focusedTool = 0;
    panelHoldJumped = false;
    panelCursorShown = !mappedInput.hasTouch();
    if (!toolbarUi) toolbarUi = makeUniqueNoThrow<ReaderToolbarUi>(renderer);
    if (!toolbarUi) {
      LOG_ERR("ERS", "OOM allocating reader toolbar");
      overlay = Overlay::None;
      return;
    }
    toolbarUi->begin();
    discardOverlayPage();
    requestUpdate();
    return;
  }
  const ChapterPosition position = chapterPosition();
  const int currentPage = position.displayPage();
  const int totalPages = position.totalPages;
  const int bookProgressPercent = bookPercentFor(position);
#ifdef BOARD_HAS_PSRAM
  resolveJjwxcBook();
  const bool isJjwxc = jjwxcBook_;
#else
  const bool isJjwxc = false;
#endif
  startActivityForResultWith<EpubReaderMenuActivity>(
      [this](const ActivityResult& result) {
        READING_STATS.resumeSession();
        const auto& menu = std::get<MenuResult>(result.data);
        if (SETTINGS.orientation != menu.orientation) {
          applyOrientation(menu.orientation);
        }
        toggleAutoPageTurn(menu.pageTurnRate);
#if FREEINK_DEVICE_EEGO_A4
        // EEGO's single-pass grayscale page must clear the menu first.
        pagesUntilFullRefresh = 1;
        forcedRefreshPending = true;
#endif
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      },
      epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
      !currentPageFootnotes.empty(), !cachedBookmarks.empty(), isJjwxc);
}

ReaderRenderSpec EpubReaderActivity::effectiveRenderSpec(const uint16_t width, const uint16_t height) const {
  auto spec = SETTINGS.readerRenderSpec(width, height);
  spec.collectTouchLinks = mappedInput.hasTouch();
  if (stylesDisabledForSession_) spec.embeddedStyle = false;
  return spec;
}

bool EpubReaderActivity::handleBuildFailure(const char* stage, const Section::BuildError error) {
  if (failedBuildSpine_ == currentSpineIndex) return false;
  bool retry = false;
#ifndef BOARD_HAS_PSRAM
  retry = error == Section::BuildError::OutOfMemory && !stylesDisabledForSession_ && SETTINGS.embeddedStyle != 0;
#endif
  if (retry) {
    // A page number is not a content position after CSS changes. Explicit
    // anchor/percentage/offset requests retain precedence over the displayed page.
    if (pendingPageJump == std::numeric_limits<uint16_t>::max()) {
      pendingPercentJump = true;
      pendingSpineProgress = 1.0f;
    }
    if (pendingAnchor.empty() && !pendingPercentJump && !pendingOffsetJump) {
      if (renderedSpineIndex_ == currentSpineIndex && currentPageVisibleOffset) {
        pendingOffsetJump = currentPageVisibleOffset;
      } else if (cachedSpineIndex == currentSpineIndex && cachedVisibleTextOffset) {
        pendingOffsetJump = cachedVisibleTextOffset;
      }
    }
    pendingPageJump.reset();
    clearDeferredReposition();
    nextPageNumber = 0;
    stylesDisabledForSession_ = true;
  }
  if (section) section->abandonBuild();
  section.reset();
  if (auto* css = epub->getCssParser()) css->clear();
  discardOverlayPage();
  currentPageLinks.clear();
  currentPageFootnotes.resize(0);
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  buildHeapPaused = false;
  buildPopupPending = false;
  failedBuildSpine_ = retry ? -1 : currentSpineIndex;
  LOG_ERR("ERS", "Build failed: spine=%d stage=%s error=%u retryWithoutCss=%d (free=%u, min=%u, maxAlloc=%u)",
          currentSpineIndex, stage, static_cast<unsigned>(error), retry, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (retry) {
    requestUpdate();
  } else {
    automaticPageTurnActive = false;
  }
  return retry;
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
#ifndef BOARD_HAS_PSRAM
  if (buildHeapPaused && !stylesDisabledForSession_ && SETTINGS.embeddedStyle != 0) return true;
#endif
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  startActivityForResultWith<DictionaryWordSelectActivity>(
      [this](const ActivityResult&) {
        READING_STATS.resumeSession();
        requestUpdate();
      },
      std::move(page), orientedMarginLeft, orientedMarginTop);
}

#ifdef ENABLE_CHINESE_VERSION
bool EpubReaderActivity::maybeOfferCompleteChineseFont() {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    pendingMissingChineseCodepoint_.store(0, std::memory_order_relaxed);
    return false;
  }

  const uint32_t codepoint = pendingMissingChineseCodepoint_.exchange(0, std::memory_order_relaxed);
  if (codepoint == 0 || FontDownloadActivity::wasChineseFontPromptShownThisBoot()) return false;

  LOG_INF("FONT", "Missing built-in Chinese glyph U+%04X; offering automatic NotoSansSC install",
          static_cast<unsigned>(codepoint));
  auto downloader =
      makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInput, FontDownloadActivity::Purpose::ReaderAutoInstall);
  if (!downloader) {
    LOG_ERR("FONT", "OOM allocating FontDownloadActivity (%zu bytes)", sizeof(FontDownloadActivity));
    return false;
  }
  startActivityForResult(std::move(downloader), [this](const ActivityResult&) {
    READING_STATS.resumeSession();
    requestUpdate();
  });
  return true;
}
#endif
namespace {
// Length of the UTF-8 sequence at `p` (1..4). Invalid lead bytes count as one so
// a damaged footnote body cannot stall the card layout.
size_t footnoteUtf8Length(const char* p) {
  const auto c = static_cast<unsigned char>(*p);
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// Collects at most `limit` bytes while the footnote item streams by, so the card
// never materializes a whole chapter in RAM.
class BoundedCollector final : public Print {
 public:
  explicit BoundedCollector(const size_t limit) : limit_(limit) {}
  size_t write(uint8_t c) override {
    if (data_.size() >= limit_) return 0;
    data_.push_back(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    const size_t room = limit_ > data_.size() ? limit_ - data_.size() : 0;
    const size_t take = std::min(size, room);
    data_.append(reinterpret_cast<const char*>(buffer), take);
    return take;
  }
  const std::string& data() const { return data_; }

 private:
  std::string data_;
  size_t limit_;
};
}  // namespace

bool EpubReaderActivity::handlePinchGesture() {
  // Two-finger pinch adjusts the font size by one step (spread = larger). The
  // SDK reports the gesture once, on release, so a gesture costs one reflow
  // instead of a frame-by-frame relayout (which e-ink cannot sustain).
  //
  // Must run from the reading surface, not from handleOverlayInput(): that one
  // only executes while a sheet is open, so a pinch in the page never reached it.
  float pinchScale = 0.0f;
  if (!mappedInput.wasPinch(pinchScale)) return false;
  // 1.08 / 0.92: a real two-finger spread rarely reaches 1.15 on this panel, and
  // the tighter thresholds made enlarging feel dead while shrinking worked.
  if (pinchScale < 1.08f && pinchScale > 0.92f) return false;
  const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
  if (sizes.size() < 2) return false;
  const uint8_t current = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
  size_t index = 0;
  while (index < sizes.size() && sizes[index] != current) ++index;
  if (index >= sizes.size()) return false;
  int next = static_cast<int>(index);
  next += pinchScale >= 1.15f ? 1 : -1;
  next = std::clamp(next, 0, static_cast<int>(sizes.size()) - 1);
  LOG_DBG("ERS", "pinch %.2f -> %u pt", pinchScale, static_cast<unsigned>(sizes[static_cast<size_t>(next)]));
  if (sizes[static_cast<size_t>(next)] == SETTINGS.fontPointSize) return true;  // at the end of the range
  SETTINGS.fontPointSize = sizes[static_cast<size_t>(next)];
  // A pinch that repaginates with no visible acknowledgement reads as "nothing
  // happened" — show the resulting size for a moment.
  fontStepHintPointSize = SETTINGS.fontPointSize;
  fontStepHintUntilMs = millis() + 4000;
  applyTextSettingLive();
  return true;
}

namespace {
size_t countUtf8Codepoints(const char* text) {
  size_t count = 0;
  for (const char* p = text; p && *p; ++p) {
    if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) ++count;
  }
  return count;
}

// Long press that opens a selection, and the finger travel that turns it into a
// drag instead. 650 ms, not the SDK's 500 ms classifier: on a reading page a
// normal "read the next line" tap frequently sits in the 500-600 ms band, and
// the SDK trigger is stationary-only, so it opened selection on presses the
// reader never meant as a hold. The SDK event is deliberately NOT consumed now;
// this timer owns the gesture and suppresses the lift only after it fires.
constexpr uint32_t kSelectionPressMs = 650;
// Sampling drops frames: a press may only end after a gap a real lift produces,
// otherwise the timer reset every few frames and the press could never complete.
constexpr uint32_t kSelectionHoldGapMs = 150;
// Travel beyond this is a swipe, not a press (about 5.5 mm on this panel; the
// old 18 px was thinner than normal finger tremor, 80 px was loose enough that
// a slow page-turn drag could still qualify as a hold).
constexpr int kSelectionSlopPx = 64;
// E-ink cannot follow a finger, so a drag repaints at most ~3x/second.
constexpr uint32_t kSelectionRedrawMs = 320;
constexpr int kAnnotationCardButtonH = 40;
// Paragraph-comment bubbles are ~20-30 px wide/high; a finger target needs more.
// Grow mostly away from the text: the bubble sits 3 px after the last glyph, so
// a large left pad would steal taps on the final character. The right side is
// the page margin, and the vertical pad uses the line's leading.
constexpr int kCommentBubbleHitPadLeft = 2;
constexpr int kCommentBubbleHitPadRight = 8;
constexpr int kCommentBubbleHitPadY = 8;
}  // namespace

void EpubReaderActivity::loadAnnotations() {
  annotations_.clear();
  annotationBubbles_.clear();
  annotationsLoaded = true;
  if (!epub) return;
  const std::string path = epub->getCachePath() + "/annotations.bin";
  HalFile file;
  if (!Storage.openFileForRead("ERS", path, file)) return;  // none yet is normal
  const size_t size = file.size();
  if (size == 0 || size > 64 * 1024) {
    file.close();
    LOG_ERR("ERS", "annotations: implausible size %u", static_cast<unsigned>(size));
    return;
  }
  std::vector<uint8_t> bytes(size);
  const int got = file.read(bytes.data(), size);
  file.close();
  if (got != static_cast<int>(size)) {
    LOG_ERR("ERS", "annotations: short read %d/%u", got, static_cast<unsigned>(size));
    return;
  }
  if (!annotations::deserialize(bytes.data(), bytes.size(), annotations_)) {
    // Rejected whole: a half-trusted range would underline the wrong sentence.
    annotations_.clear();
    LOG_ERR("ERS", "annotations: rejected %u bytes", static_cast<unsigned>(size));
    return;
  }
  LOG_DBG("ERS", "annotations: %u entries", static_cast<unsigned>(annotations_.size()));
}

void EpubReaderActivity::saveAnnotations() {
  if (!epub) return;
  annotations::normalize(annotations_);
  const auto bytes = annotations::serialize(annotations_);
  const std::string path = epub->getCachePath() + "/annotations.bin";
  const std::string tmp = path + ".tmp";
  {
    HalFile file;
    if (!Storage.openFileForWrite("ERS", tmp, file)) {
      LOG_ERR("ERS", "annotations: cannot open %s for write", tmp.c_str());
      return;
    }
    const size_t written = file.write(bytes.data(), bytes.size());
    file.close();
    if (written != bytes.size()) {
      LOG_ERR("ERS", "annotations: short write %u/%u", static_cast<unsigned>(written),
              static_cast<unsigned>(bytes.size()));
      Storage.remove(tmp.c_str());
      return;
    }
  }
  // FAT rename does not replace an existing file. Using rename() here made every
  // save fail once annotations.bin existed, so a delete/rename wrote a .tmp,
  // failed to install it, and the next load restored the old entry.
  if (!Storage.replaceFile(tmp.c_str(), path.c_str())) {
    LOG_ERR("ERS", "annotations: replace failed");
    Storage.remove(tmp.c_str());
    return;
  }
  LOG_DBG("ERS", "annotations: saved %u entries", static_cast<unsigned>(annotations_.size()));
}

void EpubReaderActivity::drawAnnotations(const Page& page, const int fontId, const int marginLeft,
                                         const int marginTop) {
  annotationBubbles_.clear();
  if (annotations_.empty()) return;
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int lineHeight = renderer.getLineHeight(fontId);
  const int screenW = renderer.getScreenWidth();
  constexpr int kBubbleW = 18;
  constexpr int kBubbleH = 16;
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto* block = line.getBlock().get();
    if (!block || block->wordCount() == 0) continue;
    uint32_t offset = block->getVisibleOffset();
    const int lineX = marginLeft + line.xPos;
    const int lineY = marginTop + line.yPos;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      const uint32_t wordStart = offset;
      const uint32_t wordEnd = wordStart + static_cast<uint32_t>(countUtf8Codepoints(text));
      offset = wordEnd;
      const int advance = renderer.getTextAdvanceX(fontId, text, block->wordStyle(i));
      for (size_t a = 0; a < annotations_.size(); ++a) {
        const auto& entry = annotations_[a];
        if (entry.spineIndex != static_cast<uint16_t>(currentSpineIndex)) continue;
        if (!annotations::overlaps(entry, wordStart, wordEnd)) continue;
        const int x = lineX + block->wordXpos(i);
        if (entry.isNote()) {
          // Notes carry a solid rule: the bubble already marks them.
          renderer.fillRect(x, lineY + ascender + 2, advance, 2, true);
        } else {
          // Highlights get a 25% dither wash so they read as "marked", clearly
          // different from the inverted glyphs of an active selection.
          for (int row = 0; row < lineHeight; row += 4) renderer.fillRect(x, lineY + row, advance, 1, true);
        }
        if (!entry.isNote() || entry.endOffset > wordEnd) continue;
        // Small bubble tucked into the line gap at the end of the range, so it
        // never covers the next word.
        int bx = x + advance - kBubbleW;
        if (bx < 2) bx = 2;
        const int by = lineY + lineHeight - 2;
        renderer.fillRect(bx, by, kBubbleW, kBubbleH, false);
        renderer.drawRect(bx, by, kBubbleW, kBubbleH, 1, true);
        for (int d = 0; d < 3; ++d) renderer.fillRect(bx + 3 + d * 4, by + kBubbleH / 2 - 1, 2, 2, true);
        annotationBubbles_.push_back({static_cast<int16_t>(bx), static_cast<int16_t>(by),
                                      static_cast<int16_t>(kBubbleW), static_cast<int16_t>(kBubbleH), a});
      }
    }
  }
}

int EpubReaderActivity::hitTestAnnotationBubble(const int x, const int y) const {
  for (size_t i = 0; i < annotationBubbles_.size(); ++i) {
    const auto& hit = annotationBubbles_[i];
    // A slightly larger target than the drawn bubble: 18x16 is small for a finger.
    if (x >= hit.x - 4 && x < hit.x + hit.w + 4 && y >= hit.y - 4 && y < hit.y + hit.h + 4) {
      return static_cast<int>(hit.index);
    }
  }
  return -1;
}

void EpubReaderActivity::showAnnotationCard(const size_t index) {
  if (index >= annotations_.size()) return;
  const auto& entry = annotations_[index];
  annotationCardIndex = index;
  annotationCardTitle = entry.chapterTitle;
  annotationCardBody = entry.note.empty() ? entry.quote : entry.note;
  annotationCardActive = true;
  LOG_DBG("ERS", "annotation card: %u bytes", static_cast<unsigned>(annotationCardBody.size()));
  requestUpdate();
}

void EpubReaderActivity::closeAnnotationCard() {
  if (!annotationCardActive) return;
  annotationCardActive = false;
  annotationCardBody.clear();
  annotationCardTitle.clear();
  requestUpdate();
}

void EpubReaderActivity::drawAnnotationCard() {
  if (!annotationCardActive) return;
  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId);
  const int padding = 12;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int cardW = std::min(screenW - 32, 460);
  const int textW = cardW - padding * 2;
  const int maxLines =
      std::max(1, (screenH * 7 / 10 - padding * 3 - kAnnotationCardButtonH) / std::max(1, lineHeight));

  // The quoted sentence first (context), then the note body.
  const std::string body = annotationCardTitle.empty() ? annotationCardBody
                                                       : annotationCardTitle + "\n" + annotationCardBody;
  std::vector<std::string> lines;
  lines.reserve(static_cast<size_t>(maxLines));
  size_t pos = 0;
  while (pos < body.size() && static_cast<int>(lines.size()) < maxLines) {
    if (body[pos] == '\n') {
      lines.emplace_back();
      ++pos;
      continue;
    }
    const size_t lineStart = pos;
    int width = 0;
    size_t lastFit = pos;
    while (pos < body.size() && body[pos] != '\n') {
      const size_t len = footnoteUtf8Length(body.c_str() + pos);
      char glyph[8] = {0};
      std::memcpy(glyph, body.c_str() + pos, std::min<size_t>(len, sizeof(glyph) - 1));
      const int advance = renderer.getTextAdvanceX(fontId, glyph, EpdFontFamily::REGULAR);
      if (width + advance > textW && pos > lineStart) break;
      width += advance;
      pos += len;
      lastFit = pos;
    }
    if (lastFit == lineStart) break;
    lines.emplace_back(body, lineStart, lastFit - lineStart);
  }

  int cardH = padding * 2 + static_cast<int>(lines.size()) * lineHeight + kAnnotationCardButtonH + padding;
  cardH = std::min(cardH, screenH * 7 / 10);
  const int cardX = (screenW - cardW) / 2;
  const int cardY = (screenH - cardH) / 2;
  annotationCardX = static_cast<int16_t>(cardX);
  annotationCardY = static_cast<int16_t>(cardY);
  annotationCardW = static_cast<int16_t>(cardW);
  annotationCardH = static_cast<int16_t>(cardH);

  // Same rounded card language as the rest of the UI (radius 14, 2 px ring).
  renderer.fillRoundedRect(cardX, cardY, cardW, cardH, 14, Color::White);
  renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 2, 14, true);
  int y = cardY + padding;
  for (const auto& line : lines) {
    renderer.drawText(fontId, cardX + padding, y, line.c_str(), true);
    y += lineHeight;
  }

  // Two actions side by side: edit and delete. Filled plate, same capsule
  // language as the reader's other primary buttons.
  const int gap = 12;
  const int btnW = (cardW - padding * 2 - gap) / 2;
  const int btnY = cardY + cardH - kAnnotationCardButtonH - padding;
  const int editX = cardX + padding;
  const int delX = editX + btnW + gap;
  annotationCardButtonX = static_cast<int16_t>(editX);
  annotationCardButtonW = static_cast<int16_t>(btnW);
  annotationCardButtonY = static_cast<int16_t>(btnY);
  annotationCardDeleteX = static_cast<int16_t>(delX);
  annotationCardDeleteW = static_cast<int16_t>(btnW);
  const char* editLabel = tr(STR_EDIT);
  const char* delLabel = tr(STR_DELETE);
  const int labelY = btnY + (kAnnotationCardButtonH - lineHeight) / 2;
  const uint8_t btnRadius = static_cast<uint8_t>(kAnnotationCardButtonH / 2);
  renderer.fillRoundedRect(editX, btnY, btnW, kAnnotationCardButtonH, btnRadius, Color::Black);
  renderer.fillRoundedRect(delX, btnY, btnW, kAnnotationCardButtonH, btnRadius, Color::Black);
  renderer.drawText(fontId, editX + (btnW - renderer.getTextAdvanceX(fontId, editLabel, EpdFontFamily::BOLD)) / 2,
                    labelY, editLabel, false, EpdFontFamily::BOLD);
  renderer.drawText(fontId, delX + (btnW - renderer.getTextAdvanceX(fontId, delLabel, EpdFontFamily::BOLD)) / 2,
                    labelY, delLabel, false, EpdFontFamily::BOLD);
}

bool EpubReaderActivity::handleAnnotationCardInput() {
  if (!annotationCardActive) return false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeAnnotationCard();
    return true;
  }
  int tx = 0;
  int ty = 0;
  if (!mappedInput.wasScreenTapped(tx, ty)) return true;  // modal
  const bool onEdit = tx >= annotationCardButtonX && tx < annotationCardButtonX + annotationCardButtonW &&
                      ty >= annotationCardButtonY && ty < annotationCardButtonY + kAnnotationCardButtonH;
  if (onEdit) {
    promptEditAnnotation(annotationCardIndex);
    return true;
  }
  const bool onDelete = tx >= annotationCardDeleteX && tx < annotationCardDeleteX + annotationCardDeleteW &&
                        ty >= annotationCardButtonY && ty < annotationCardButtonY + kAnnotationCardButtonH;
  if (onDelete) {
    if (annotationCardIndex < annotations_.size()) {
      annotations_.erase(annotations_.begin() + annotationCardIndex);
      saveAnnotations();
    }
    closeAnnotationCard();
    return true;
  }
  const bool inside = tx >= annotationCardX && tx < annotationCardX + annotationCardW && ty >= annotationCardY &&
                      ty < annotationCardY + annotationCardH;
  if (!inside) closeAnnotationCard();
  return true;
}

bool EpubReaderActivity::handleAnnotationBubbleTap() {
  if (annotationCardActive || annotationBubbles_.empty() || overlay != Overlay::None) return false;
  int tx = 0;
  int ty = 0;
  if (!mappedInput.wasScreenTapped(tx, ty)) return false;
  const int hit = hitTestAnnotationBubble(tx, ty);
  if (hit < 0) return false;
  showAnnotationCard(static_cast<size_t>(hit));
  return true;
}

std::vector<EpubReaderActivity::AnnoWordRect> EpubReaderActivity::layoutPageWords(const Page& page, const int fontId,
                                                                                 const int marginLeft,
                                                                                 const int marginTop) {
  std::vector<AnnoWordRect> words;
  const int lineHeight = renderer.getLineHeight(fontId);
  const bool vertical = SETTINGS.verticalText != 0;
  const int colW = vertical ? renderer.getLineHeight(fontId, renderedPageKey_.spec.lineCompression) : lineHeight;
  const int usableW = renderer.getScreenWidth() - marginLeft * 2;
  uint16_t lineIndex = 0;
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto* block = line.getBlock().get();
    if (!block) continue;
    uint32_t offset = block->getVisibleOffset();
    // Vertical columns start each line at its own depth; a word's depth grows by
    // its *character count*, not by its horizontal pixel advance (that mismatch is
    // what shifted Latin words out of place).
    int depth = line.xPos;
    const int col = colW > 0 ? line.yPos / colW : 0;
    const int columnX = marginLeft + usableW - (col + 1) * colW;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      const uint32_t start = offset;
      const uint32_t chars = static_cast<uint32_t>(countUtf8Codepoints(text));
      offset += chars;
      const int advance = renderer.getTextAdvanceX(fontId, text, block->wordStyle(i));
      if (vertical) {
        const int wordH = static_cast<int>(chars) * colW;
        words.push_back({static_cast<int16_t>(columnX), static_cast<int16_t>(marginTop + depth),
                         static_cast<int16_t>(colW), static_cast<int16_t>(wordH), lineIndex, i, start, offset, text});
        depth += wordH;
      } else {
        words.push_back({static_cast<int16_t>(marginLeft + line.xPos + block->wordXpos(i)),
                         static_cast<int16_t>(marginTop + line.yPos), static_cast<int16_t>(advance),
                         static_cast<int16_t>(lineHeight), lineIndex, i, start, offset, text});
      }
    }
    ++lineIndex;
  }
  return words;
}

int EpubReaderActivity::wordIndexAt(const std::vector<AnnoWordRect>& words, const int x, const int y) const {
  int best = -1;
  int bestDist = 0x7FFFFFFF;
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& w = words[i];
    if (x >= w.x && x < w.x + w.w && y >= w.y && y < w.y + w.h) return static_cast<int>(i);
    const int dx = x < w.x ? w.x - x : (x > w.x + w.w ? x - (w.x + w.w) : 0);
    const int dy = y < w.y ? w.y - y : (y > w.y + w.h ? y - (w.y + w.h) : 0);
    if (dx + dy < bestDist) {
      bestDist = dx + dy;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void EpubReaderActivity::updateSelectionDragTo(const int x, const int y) {
  if (!selectionActive || selectionDragHandle == 0) return;
  auto page = loadPageForSelection();
  if (!page) return;
  int mt = 0;
  int mr = 0;
  int mb = 0;
  int ml = 0;
  renderer.getOrientedViewableTRBL(&mt, &mr, &mb, &ml);
  mt += SETTINGS.screenMargin;
  ml += SETTINGS.screenMargin;
  const int fontId = SETTINGS.getReaderFontId();
  const auto words = layoutPageWords(*page, fontId, ml, mt);
  const int hit = wordIndexAt(words, x, y);
  if (hit < 0) return;
  const auto& w = words[static_cast<size_t>(hit)];
  const auto before = [](const uint16_t l1, const uint16_t w1, const uint16_t l2, const uint16_t w2) {
    return l1 < l2 || (l1 == l2 && w1 < w2);
  };
  if (selectionDragHandle == 1) {
    // Never cross the other end: clamp instead of swapping mid-drag, which would
    // make the range flip under the finger.
    if (before(selectionEndLine, selectionEndWord, w.line, w.word)) return;
    if (w.line == selectionStartLine && w.word == selectionStartWord) return;
    selectionStartLine = w.line;
    selectionStartWord = w.word;
  } else {
    if (before(w.line, w.word, selectionStartLine, selectionStartWord)) return;
    if (w.line == selectionEndLine && w.word == selectionEndWord) return;
    selectionEndLine = w.line;
    selectionEndWord = w.word;
  }
  selectionAnchorX = w.x + w.w / 2;
  // E-ink cannot follow a finger; the cap keeps a drag from queuing a repaint per
  // pixel while still showing the range grow.
  const uint32_t now = millis();
  if (now - selectionLastRedrawMs < kSelectionRedrawMs) return;
  selectionLastRedrawMs = now;
  requestUpdate();
}

bool EpubReaderActivity::handleSelectionTouch() {
  if (overlayPopup.isActive()) return false;
  // A selection that just closed must not start a new one under the same finger:
  // that is what made "delete highlight" appear to select a neighbouring word.
  if (millis() - selectionClosedMs < 350) {
    pressTracking = false;
    return true;
  }
  const uint32_t now = millis();
  int x = 0;
  int y = 0;
  const bool held = mappedInput.isScreenTouchHeld(x, y);

  if (!selectionActive) {
    // The SDK's 500 ms classifier is intentionally not consumed: its threshold
    // is short enough to catch ordinary reading taps, and consuming it would
    // also suppress the contact before our longer timer could decide. Let the
    // release become a normal tap if the finger lifts before kSelectionPressMs.
    if (held) {
      if (!pressTracking) {
        pressTracking = true;
        pressStartMs = now;
        pressStartX = pressLastX = static_cast<int16_t>(x);
        pressStartY = pressLastY = static_cast<int16_t>(y);
        LOG_DBG("ERS", "selection: press start %d,%d", x, y);
      }
      pressLastHeldMs = now;
      pressLastX = static_cast<int16_t>(x);
      pressLastY = static_cast<int16_t>(y);
      const int dx = x - pressStartX;
      const int dy = y - pressStartY;
      if (dx * dx + dy * dy > kSelectionSlopPx * kSelectionSlopPx) {
        LOG_DBG("ERS", "selection: press cancelled (travel)");
        pressTracking = false;  // genuine swipe
        return false;
      }
      if (now - pressStartMs >= kSelectionPressMs) {
        LOG_DBG("ERS", "selection: hold fired %d,%d", x, y);
        pressTracking = false;
        beginSelection(x, y);
        // Consume the contact once the hold timer fired, even when no word was
        // found: otherwise the still-down finger restarts the timer and the
        // "no word" path repeats every 650 ms. A deliberate hold on blank space
        // doing nothing is safer than a surprise page turn on the lift.
        mappedInput.suppressScreenTouch();
        selectionDragging = selectionActive;
        selectionLastRedrawMs = now;
        return selectionActive;
      }
      return false;
    }
    // No contact this frame. A real lift is followed by many empty frames, while
    // a dropped sample is followed by contact again in a few ms — only the former
    // may end the press, or the 520 ms timer restarts forever.
    if (pressTracking && now - pressLastHeldMs < kSelectionHoldGapMs) return false;
    // The panel can stop reporting a stationary finger entirely. If the press has
    // already lasted long enough at its last known point, open the selection from
    // there instead of dropping the gesture.
    if (pressTracking && now - pressStartMs >= kSelectionPressMs) {
      const int px = pressLastX;
      const int py = pressLastY;
      pressTracking = false;
      LOG_DBG("ERS", "selection: hold fired from last point %d,%d", px, py);
      beginSelection(px, py);
      mappedInput.suppressScreenTouch();
      selectionDragging = selectionActive;
      selectionLastRedrawMs = now;
      return selectionActive;
    }
    pressTracking = false;
    return false;
  }

  // A selection is open: handleSelectionInput() owns the touch (handles + keys).
  // This path is unreachable while selecting, but keep it explicit instead of
  // duplicating the drag logic here — that duplication is what made the handles
  // look "drag-less": the drag code sat behind an event that was always consumed
  // first.
  return true;
}

std::unique_ptr<Page> EpubReaderActivity::loadPageForSelection() {
  if (!section) return nullptr;
  auto page = section->loadPage(section->currentPage);
  if (!page && nextPageNumber != section->currentPage) page = section->loadPage(nextPageNumber);
  if (!page) LOG_DBG("ERS", "selection: page %u unavailable", static_cast<unsigned>(section->currentPage));
  return page;
}

void EpubReaderActivity::beginSelection(const int x, const int y) {
  // Deliberately NOT gated on section->isBuilding(). On this board a chapter
  // build routinely stalls (the CSS parse wants ~64 KB while the reader has
  // ~48 KB), after which isBuilding() stays true for the whole session — the old
  // check therefore failed every single press with "section not ready", before
  // anything could be selected. Section::loadPage() is written to serve the pages
  // already laid out while a build is in flight, and the page on screen is always
  // one of those.
  if (!section) {
    LOG_DBG("ERS", "selection: no section");
    return;
  }
  auto page = loadPageForSelection();
  if (!page) {
    GUI.drawPopup(renderer, "selection: no page");
    return;
  }
  int mt = 0;
  int mr = 0;
  int mb = 0;
  int ml = 0;
  renderer.getOrientedViewableTRBL(&mt, &mr, &mb, &ml);
  mt += SETTINGS.screenMargin;
  ml += SETTINGS.screenMargin;
  const int fontId = SETTINGS.getReaderFontId();
  const auto words = layoutPageWords(*page, fontId, ml, mt);
  if (words.empty()) {
    LOG_DBG("ERS", "selection: page has no words");
    GUI.drawPopup(renderer, "selection: no text");
    return;
  }
  // A word under the finger is the natural anchor; a press on the gap between
  // words snaps to the nearest one so the gesture never silently does nothing.
  int best = -1;
  int bestDist = 0x7FFFFFFF;
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& w = words[i];
    if (x >= w.x && x < w.x + w.w && y >= w.y && y < w.y + w.h) {
      best = static_cast<int>(i);
      break;
    }
    const int dx = x < w.x ? w.x - x : (x > w.x + w.w ? x - (w.x + w.w) : 0);
    const int dy = y < w.y ? w.y - y : (y > w.y + w.h ? y - (w.y + w.h) : 0);
    if (dx + dy < bestDist) {
      bestDist = dx + dy;
      best = static_cast<int>(i);
    }
  }
  if (best < 0) {
    LOG_DBG("ERS", "selection: no word near %d,%d", x, y);
    GUI.drawPopup(renderer, "selection: no word");
    return;
  }
  const auto& w = words[static_cast<size_t>(best)];
  selectionEditingIndex = -1;
  // Long-pressing inside an existing annotation re-selects its whole range and
  // turns the first action into Delete, which is the only way back to it once the
  // bubble has scrolled away.
  for (size_t a = 0; a < annotations_.size(); ++a) {
    const auto& entry = annotations_[a];
    if (entry.spineIndex != static_cast<uint16_t>(currentSpineIndex)) continue;
    if (!annotations::overlaps(entry, w.start, w.end)) continue;
    int firstIdx = -1;
    int lastIdx = -1;
    for (size_t i = 0; i < words.size(); ++i) {
      if (!annotations::overlaps(entry, words[i].start, words[i].end)) continue;
      if (firstIdx < 0) firstIdx = static_cast<int>(i);
      lastIdx = static_cast<int>(i);
    }
    if (firstIdx >= 0) {
      selectionStartLine = words[static_cast<size_t>(firstIdx)].line;
      selectionStartWord = words[static_cast<size_t>(firstIdx)].word;
      selectionEndLine = words[static_cast<size_t>(lastIdx)].line;
      selectionEndWord = words[static_cast<size_t>(lastIdx)].word;
      selectionEditingIndex = static_cast<int>(a);
      LOG_DBG("ERS", "selection: editing annotation %u", static_cast<unsigned>(a));
    }
    break;
  }
  if (selectionEditingIndex < 0) {
    selectionStartLine = selectionEndLine = w.line;
    selectionStartWord = selectionEndWord = w.word;
  }
  selectionAnchorX = w.x + w.w / 2;
  selectionActive = true;
  // Show the action bar right away: this device has no Confirm key, so a bar that
  // only opens on Confirm could never be reached at all.
  selectionMenuActive = true;
  selectionMenuChoice = 0;
  selectionMenuOpenedMs = millis();
  selectionMenuDrawLogged = false;
  LOG_DBG("ERS", "selection: line %u word %u (%u words)", w.line, w.word, static_cast<unsigned>(words.size()));
  requestUpdate();
}

void EpubReaderActivity::endSelection() {
  if (!selectionActive && !selectionMenuActive && !selectionDragging) return;
  selectionActive = false;
  selectionMenuActive = false;
  selectionDragging = false;
  selectionDragHandle = 0;
  // Cool-down: the tap that confirmed Delete/Highlight is still settling, and
  // without this the very next frame starts a fresh selection under the finger.
  selectionClosedMs = millis();
  requestUpdate();
}

bool EpubReaderActivity::selectionCovers(const uint16_t line, const uint16_t word) const {
  uint16_t aLine = selectionStartLine;
  uint16_t aWord = selectionStartWord;
  uint16_t bLine = selectionEndLine;
  uint16_t bWord = selectionEndWord;
  if (bLine < aLine || (bLine == aLine && bWord < aWord)) {
    std::swap(aLine, bLine);
    std::swap(aWord, bWord);
  }
  if (line < aLine || line > bLine) return false;
  if (line == aLine && word < aWord) return false;
  if (line == bLine && word > bWord) return false;
  return true;
}

void EpubReaderActivity::drawSelection(const Page& page, const int fontId, const int marginLeft,
                                       const int marginTop) {
  if (!selectionActive) return;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int screenW = renderer.getScreenWidth();
  const bool vertical = SETTINGS.verticalText != 0;
  // First and last highlighted word, for the two drag handles.
  int firstX = -1;
  int firstY = 0;
  int firstW = 0;
  int firstH = 0;
  int lastX = 0;
  int lastY = 0;
  int lastW = 0;
  int lastH = 0;
  // One coordinate source for highlight, handles and hit-testing: the vertical
  // mapping lives in layoutPageWords(), so drawing must use the same rectangles.
  // Keeping a second copy of that math here is what made the selection land in the
  // wrong place in vertical mode.
  const auto words = layoutPageWords(page, fontId, marginLeft, marginTop);
  for (const auto& w : words) {
    if (!selectionCovers(w.line, w.word)) continue;
    if (firstX < 0) {
      firstX = w.x;
      firstY = w.y;
      firstW = w.w;
      firstH = w.h;
    }
    lastX = w.x;
    lastY = w.y;
    lastW = w.w;
    lastH = w.h;
    // Inverse video is the only selection an e-ink panel shows unambiguously in
    // every refresh mode: paint the block, then redraw the glyphs in white.
    renderer.fillRect(w.x, w.y, w.w, w.h, true);
    if (vertical) {
      int gy = w.y;
      for (const char* p = w.text; p && *p;) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 1;
        char glyph[5] = {0, 0, 0, 0, 0};
        for (int k = 0; k < len; ++k) glyph[k] = p[k];
        renderer.drawText(fontId, w.x, gy, glyph, false);
        p += len;
        gy += lineHeight;
      }
    } else {
      renderer.drawText(fontId, w.x, w.y, w.text, false);
    }
  }
  if (firstX < 0) return;
  selectionFirstTopY = static_cast<int16_t>(firstY);
  selectionLastBottomY = static_cast<int16_t>(vertical ? lastY + lastH : lastY + lineHeight);
  selectionFirstX = static_cast<int16_t>(firstX);
  selectionLastX = static_cast<int16_t>(lastX);
  selectionLastW = static_cast<int16_t>(lastW);

  // Two handles, one per end. They are the only way to change the range: tapping
  // inside the range no longer silently shortens it, which was the part that read
  // as arbitrary.
  constexpr int kHandle = 24;
  int hx[2];
  int hy[2];
  if (SETTINGS.verticalText != 0) {
    // Vertical writing: the range runs down a column, so its two ends are above and
    // below the text.
    hx[0] = firstX + (firstW - kHandle) / 2;
    hx[1] = lastX + (lastW - kHandle) / 2;
    hy[0] = firstY - kHandle - 4;
    hy[1] = lastY + lastH + 4;
  } else {
    hx[0] = firstX - kHandle - 4;
    hx[1] = lastX + lastW + 4;
    hy[0] = firstY + (lineHeight - kHandle) / 2;
    hy[1] = lastY + (lineHeight - kHandle) / 2;
  }
  for (int i = 0; i < 2; ++i) {
    int x = hx[i];
    if (x < 2) x = 2;
    if (x + kHandle > screenW - 2) x = screenW - 2 - kHandle;
    renderer.fillRect(x, hy[i], kHandle, kHandle, false);
    renderer.drawRect(x, hy[i], kHandle, kHandle, 2, true);
    selectionHandleX[i] = static_cast<int16_t>(x);
    selectionHandleY[i] = static_cast<int16_t>(hy[i]);
    selectionHandleW[i] = kHandle;
    selectionHandleH[i] = kHandle;
  }
}

bool EpubReaderActivity::handleSelectionHandleTouch() {
  int hx = 0;
  int hy = 0;
  const bool held = mappedInput.isScreenTouchHeld(hx, hy);
  if (!held) {
    selectionDragging = false;
    selectionDragHandle = 0;
    return false;
  }
  if (!selectionDragging) {
    // Handles first: they are the only way to adjust the range.
    for (int i = 0; i < 2; ++i) {
      if (selectionHandleW[i] <= 0) continue;
      if (hx >= selectionHandleX[i] - 12 && hx < selectionHandleX[i] + selectionHandleW[i] + 12 &&
          hy >= selectionHandleY[i] - 12 && hy < selectionHandleY[i] + selectionHandleH[i] + 12) {
        selectionDragging = true;
        selectionDragHandle = i + 1;
        pressLastX = static_cast<int16_t>(hx);
        pressLastY = static_cast<int16_t>(hy);
        LOG_DBG("ERS", "selection: drag handle %d at %d,%d", i + 1, hx, hy);
        return true;
      }
    }
    // Not on a handle: swallow the press. A tap away from the handles ends the
    // selection (see handleSelectionInput); dragging elsewhere does nothing.
    return true;
  }
  if (hx != pressLastX || hy != pressLastY) {
    pressLastX = static_cast<int16_t>(hx);
    pressLastY = static_cast<int16_t>(hy);
    updateSelectionDragTo(hx, hy);
  }
  return true;
}

bool EpubReaderActivity::handleSelectionInput() {
  if (!selectionActive) return false;

  // Handles first: dragging must keep working while the action bar is visible
  // (the bar opens as soon as a selection starts).
  if (handleSelectionHandleTouch()) return true;

  // --- Action bar owns every key while it is up ---
  if (selectionMenuActive) {
    // Back is checked before the anti-double-tap guard: cancelling must work on
    // the very first press, even if it lands within the opening gesture.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      endSelection();
      return true;
    }
    if (millis() - selectionMenuOpenedMs < 250) return true;  // swallow the opening press
    const bool leftKey = mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool rightKey = mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (leftKey || rightKey) {
      selectionMenuChoice = leftKey ? 0 : 1;
      requestUpdate();
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelectionMenuChoice(selectionMenuChoice);
      return true;
    }
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      for (int i = 0; i < 2; ++i) {
        if (tx >= selectionMenuHitX[i] && tx < selectionMenuHitX[i] + selectionMenuHitW[i] &&
            ty >= selectionMenuHitY[i] && ty < selectionMenuHitY[i] + selectionMenuHitH[i]) {
          activateSelectionMenuChoice(i);
          return true;
        }
      }
      selectionMenuActive = false;
      // Tapping away from the chips ends the selection, it does not keep it.
      endSelection();
      return true;
    }
    return true;
  }

  // Bar closed and the user tapped the page: end the selection. Adjusting the
  // range is the handles' job alone now, so a stray tap just cancels.
  {
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasScreenTapped(tapX, tapY)) {
      endSelection();
      return true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showSelectionMenu();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Back cancels the selection outright (no two-stage collapse).
    endSelection();
    return true;
  }

  auto page = loadPageForSelection();
  if (!page) {
    endSelection();
    return true;
  }
  int mt = 0;
  int mr = 0;
  int mb = 0;
  int ml = 0;
  renderer.getOrientedViewableTRBL(&mt, &mr, &mb, &ml);
  mt += SETTINGS.screenMargin;
  ml += SETTINGS.screenMargin;
  const int fontId = SETTINGS.getReaderFontId();
  const auto words = layoutPageWords(*page, fontId, ml, mt);
  if (words.empty()) {
    endSelection();
    return true;
  }

  // Tap: no longer changes the range — see the handle block above.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) return true;

  // Read every key once: wasReleased() is an edge query, so calling it twice in a
  // condition chain is not safe.
  const bool left = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool right = mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool up = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool down = mappedInput.wasReleased(MappedInputManager::Button::Down);
  if (!left && !right && !up && !down) return true;

  size_t focus = 0;
  for (size_t i = 0; i < words.size(); ++i) {
    if (words[i].line == selectionEndLine && words[i].word == selectionEndWord) {
      focus = i;
      break;
    }
  }
  if (left) {
    if (focus == 0) return true;
    --focus;
  } else if (right) {
    if (focus + 1 >= words.size()) return true;
    ++focus;
  } else {
    const int targetLine = static_cast<int>(selectionEndLine) + (up ? -1 : 1);
    if (targetLine < 0) return true;
    // Nearest word on the neighbouring line to the remembered column.
    int best = -1;
    int bestDist = 0x7FFFFFFF;
    for (size_t i = 0; i < words.size(); ++i) {
      if (words[i].line != static_cast<uint16_t>(targetLine)) continue;
      const int centre = words[i].x + words[i].w / 2;
      const int distance = centre > selectionAnchorX ? centre - selectionAnchorX : selectionAnchorX - centre;
      if (distance < bestDist) {
        bestDist = distance;
        best = static_cast<int>(i);
      }
    }
    if (best < 0) return true;
    focus = static_cast<size_t>(best);
  }
  extendSelectionTo(words[focus]);
  return true;
}

void EpubReaderActivity::showSelectionMenu() {
  selectionMenuActive = true;
  selectionMenuChoice = 0;
  selectionMenuDrawLogged = false;
  // The Confirm that opened the bar is still "released" for the next frame or two;
  // without this guard the bar would be dismissed by its own opening press.
  selectionMenuOpenedMs = millis();
  LOG_DBG("ERS", "selection: menu open");
  requestUpdate();
}

void EpubReaderActivity::activateSelectionMenuChoice(const int choice) {
  selectionMenuActive = false;
  if (selectionEditingIndex >= 0) {
    const size_t index = static_cast<size_t>(selectionEditingIndex);
    selectionEditingIndex = -1;
    if (choice == 0) {
      // Tapping an existing annotation makes the first chip Delete.
      if (index < annotations_.size()) {
        annotations_.erase(annotations_.begin() + index);
        saveAnnotations();
      }
      endSelection();
      return;
    }
    endSelection();  // drop the selection before the keyboard takes over
    promptEditAnnotation(index);
    return;
  }
  commitSelection(choice == 1);
}

void EpubReaderActivity::drawSelectionMenu() {
  if (!selectionMenuActive) return;
  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId);
  const int screenW = renderer.getScreenWidth();
  // Compact chips, not full-width blocks: height follows the text, width follows
  // the label. Nothing is filled, so no plane can leave a black slab behind.
  const int chipH = lineHeight + 10;
  const int padX = 26;
  const int gap = 14;
  const char* labels[2] = {selectionEditingIndex >= 0 ? tr(STR_DELETE) : tr(STR_HIGHLIGHT), tr(STR_NOTE)};
  // CJK labels are drawn with a fallback face whose advance is wider than the
  // primary face used to measure here, so leave slack or the text spills out.
  const int w0 = renderer.getTextAdvanceX(fontId, labels[0], EpdFontFamily::BOLD) + padX * 2 + 10;
  const int w1 = renderer.getTextAdvanceX(fontId, labels[1], EpdFontFamily::BOLD) + padX * 2 + 10;
  const int totalW = w0 + gap + w1;
  const uint8_t radius = static_cast<uint8_t>(chipH / 2);
  const auto drawChip = [&](const int x, const int y, const int w, const char* label, const bool active,
                            const int idx) {
    renderer.fillRoundedRect(x, y, w, chipH, radius, Color::White);
    renderer.drawRoundedRect(x, y, w, chipH, active ? 3 : 2, radius, true);
    const int textW = renderer.getTextAdvanceX(fontId, label, EpdFontFamily::BOLD);
    renderer.drawText(fontId, x + (w - textW) / 2, y + (chipH - lineHeight) / 2, label, true, EpdFontFamily::BOLD);
    selectionMenuHitX[idx] = static_cast<int16_t>(x);
    selectionMenuHitY[idx] = static_cast<int16_t>(y);
    selectionMenuHitW[idx] = static_cast<int16_t>(w);
    selectionMenuHitH[idx] = static_cast<int16_t>(chipH);
  };
  if (SETTINGS.verticalText != 0) {
    // Vertical writing. The stack is centred on the selection's vertical middle and
    // hugs the selection's side: to the right of it (chips left-aligned against the
    // column) or, when that would overflow, to its left (chips right-aligned). When
    // neither side has room the chips fall back to a centred horizontal row.
    const int chipW = std::max(w0, w1);
    const int stackH = chipH * 2 + 10;
    const int selCenterY = (selectionFirstTopY + selectionLastBottomY) / 2;
    const int maxStackY = renderer.getScreenHeight() - stackH - 56;  // clear of the status bar
    int bx = selectionLastX + selectionLastW + 10;
    if (bx + chipW > screenW - 6) {
      bx = selectionFirstX - chipW - 10;
      if (bx < 6) {
        // No room on either side: centre the row on the selection's middle.
        const int totalW = w0 + gap + w1;
        int x = std::max<int>(6, (screenW - totalW) / 2);
        int y = selCenterY - chipH / 2;
        if (y > renderer.getScreenHeight() - chipH - 56) y = renderer.getScreenHeight() - chipH - 56;
        if (y < 8) y = 8;
        for (int i = 0; i < 2; ++i) {
          const int w = i == 0 ? w0 : w1;
          drawChip(x, y, w, labels[i], i == selectionMenuChoice, i);
          x += w + gap;
        }
        return;
      }
    }
    int by = selCenterY - stackH / 2;
    if (by > maxStackY) by = maxStackY;
    if (by < 8) by = 8;
    for (int i = 0; i < 2; ++i) {
      drawChip(bx, by + i * (chipH + 10), i == 0 ? w0 : w1, labels[i], i == selectionMenuChoice, i);
    }
  } else {
    int barY = selectionLastBottomY + 10;
    if (barY + chipH > renderer.getScreenHeight() - 8) barY = selectionFirstTopY - chipH - 10;
    if (barY < 8) barY = 8;
    int x = std::max<int>(6, (screenW - totalW) / 2);
    for (int i = 0; i < 2; ++i) {
      const int w = i == 0 ? w0 : w1;
      drawChip(x, barY, w, labels[i], i == selectionMenuChoice, i);
      x += w + gap;
    }
  }
  if (!selectionMenuDrawLogged) {
    selectionMenuDrawLogged = true;
    LOG_DBG("ERS", "selection: menu drawn h=%d (sel %d..%d) vertical=%d", chipH, selectionFirstTopY,
            selectionLastBottomY, SETTINGS.verticalText);
  }
}

void EpubReaderActivity::extendSelectionTo(const AnnoWordRect& word) {
  const auto before = [](const uint16_t l1, const uint16_t w1, const uint16_t l2, const uint16_t w2) {
    return l1 < l2 || (l1 == l2 && w1 < w2);
  };
  // The anchored end never slides: whichever side the target falls on grows. That
  // is what makes tapping left of the anchor extend left instead of translating
  // the whole range.
  if (before(word.line, word.word, selectionStartLine, selectionStartWord)) {
    selectionStartLine = word.line;
    selectionStartWord = word.word;
  } else {
    selectionEndLine = word.line;
    selectionEndWord = word.word;
  }
  selectionAnchorX = word.x + word.w / 2;
  requestUpdate();
}

void EpubReaderActivity::commitSelection(const bool asNote) {
  if (!section) {
    endSelection();
    return;
  }
  auto page = loadPageForSelection();
  if (!page) {
    endSelection();
    return;
  }
  int mt = 0;
  int mr = 0;
  int mb = 0;
  int ml = 0;
  renderer.getOrientedViewableTRBL(&mt, &mr, &mb, &ml);
  mt += SETTINGS.screenMargin;
  ml += SETTINGS.screenMargin;
  const int fontId = SETTINGS.getReaderFontId();
  const auto words = layoutPageWords(*page, fontId, ml, mt);
  bool haveStart = false;
  uint32_t start = 0;
  uint32_t end = 0;
  std::string quote;
  for (const auto& w : words) {
    if (!selectionCovers(w.line, w.word)) continue;
    if (!haveStart) {
      start = w.start;
      haveStart = true;
    }
    end = w.end;
    if (quote.size() + std::strlen(w.text) <= annotations::kMaxQuoteBytes) quote.append(w.text);
  }
  if (!haveStart || end <= start) {
    endSelection();
    return;
  }
  annotations::Annotation entry;
  entry.spineIndex = static_cast<uint16_t>(currentSpineIndex);
  entry.startOffset = start;
  entry.endOffset = end;
  entry.kind = static_cast<uint8_t>(asNote ? annotations::Kind::Note : annotations::Kind::Highlight);
  entry.quote = annotations::clampUtf8(quote, annotations::kMaxQuoteBytes);
  entry.chapterTitle = currentChapterTitle();
  if (!asNote) {
    annotations_.push_back(std::move(entry));
    saveAnnotations();
    endSelection();
    return;
  }
  promptSelectionNote(entry);
}

void EpubReaderActivity::promptSelectionNote(const annotations::Annotation& base) {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NOTE), std::string(),
                                                          annotations::kMaxNoteBytes, InputType::Text);
  if (!keyboard) {
    LOG_ERR("ERS", "OOM: annotation keyboard");
    endSelection();
    return;
  }
  startActivityForResult(std::move(keyboard), [this, base](const ActivityResult& result) {
    if (result.isCancelled) {
      endSelection();
      return;
    }
    annotations::Annotation entry = base;
    entry.note = annotations::clampUtf8(std::get<KeyboardResult>(result.data).text, annotations::kMaxNoteBytes);
    annotations_.push_back(std::move(entry));
    saveAnnotations();
    endSelection();
  });
}

void EpubReaderActivity::promptEditAnnotation(const size_t index) {
  if (index >= annotations_.size()) return;
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_EDIT),
                                                          annotations_[index].note, annotations::kMaxNoteBytes,
                                                          InputType::Text);
  if (!keyboard) {
    LOG_ERR("ERS", "OOM: annotation edit keyboard");
    return;
  }
  annotationCardActive = false;  // the keyboard replaces the card, not sits over it
  startActivityForResult(std::move(keyboard), [this, index](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    if (index >= annotations_.size()) return;
    // Editing an underline-only highlight promotes it to a note: the text the
    // user just typed is the point of the entry from now on.
    annotations_[index].note =
        annotations::clampUtf8(std::get<KeyboardResult>(result.data).text, annotations::kMaxNoteBytes);
    annotations_[index].kind = static_cast<uint8_t>(annotations::Kind::Note);
    saveAnnotations();
    requestUpdate();
  });
}

void EpubReaderActivity::exportAnnotations() {
  if (!epub) return;
  // Human-readable date for the export header; an unset clock simply omits it
  // rather than printing an epoch number.
  char stamp[16] = "";
  const time_t now = time(nullptr);
  if (now > 0) {
    const tm* local = localtime(&now);
    if (local != nullptr) strftime(stamp, sizeof(stamp), "%Y-%m-%d", local);
  }
  const std::string body = annotations::formatExport(epub->getTitle(), stamp, annotations_);
  const std::string path = epub->getCachePath() + "/annotations.txt";
  HalFile file;
  if (!Storage.openFileForWrite("ERS", path, file)) {
    LOG_ERR("ERS", "annotations: cannot open %s for export", path.c_str());
    GUI.drawPopup(renderer, tr(STR_NOT_AVAILABLE));
    return;
  }
  const size_t written = file.write(body.data(), body.size());
  file.close();
  LOG_DBG("ERS", "annotations exported: %u/%u bytes -> %s", static_cast<unsigned>(written),
          static_cast<unsigned>(body.size()), path.c_str());
  GUI.drawPopup(renderer, path.c_str());
}

void EpubReaderActivity::openFootnoteCard(const char* href) {
  if (!epub || !href || href[0] == '\0') return;
  const std::string hrefStr(href);
  // Same-file anchors carry no body of their own; the "note" is text on the
  // page the reader is already looking at, so jumping is still the right move.
  if (hrefStr[0] == '#') {
    navigateToHref(href, true);
    return;
  }
  std::string itemHref = hrefStr;
  const auto hash = itemHref.find('#');
  if (hash != std::string::npos) itemHref.resize(hash);

  BoundedCollector collector(4096);
  if (!epub->readItemContentsToStream(itemHref, collector, 512) || collector.data().empty()) {
    LOG_DBG("ERS", "footnote card: no body for %s", itemHref.c_str());
    navigateToHref(href, true);
    return;
  }

  const std::string plain = htmlToPlainText(collector.data());
  // Collapse HTML whitespace: the card is a glance, not a reproduction.
  std::string compact;
  compact.reserve(plain.size());
  bool pendingSpace = false;
  for (const char c : plain) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      pendingSpace = !compact.empty();
      continue;
    }
    if (pendingSpace) {
      compact.push_back(' ');
      pendingSpace = false;
    }
    compact.push_back(c);
  }
  constexpr size_t kMaxBytes = 900;
  if (compact.size() > kMaxBytes) {
    size_t cut = kMaxBytes;
    while (cut > 0 && (static_cast<unsigned char>(compact[cut]) & 0xC0) == 0x80) --cut;
    compact.resize(cut);
    compact += "…";
  }
  if (compact.empty()) {
    navigateToHref(href, true);
    return;
  }

  footnoteCardText = std::move(compact);
  footnoteCardHref = hrefStr;
  footnoteCardHasPage = epub->resolveHrefToSpineIndex(itemHref) >= 0;
  footnoteCardActive = true;
  LOG_DBG("ERS", "footnote card: %u bytes, own page=%d", static_cast<unsigned>(footnoteCardText.size()),
          footnoteCardHasPage ? 1 : 0);
  requestUpdate();
}

void EpubReaderActivity::closeFootnoteCard() {
  if (!footnoteCardActive) return;
  footnoteCardActive = false;
  footnoteCardHasPage = false;
  footnoteCardText.clear();
  footnoteCardHref.clear();
  requestUpdate();
}

void EpubReaderActivity::drawFootnoteCard() {
  if (!footnoteCardActive) return;
  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId);
  const int padding = 12;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int cardW = std::min(screenW - 32, 460);
  const int textW = cardW - padding * 2;
  const int footerHeight = footnoteCardHasPage ? lineHeight + 8 : 0;
  const int maxLines = std::max(1, (screenH * 6 / 10 - padding * 2 - footerHeight) / std::max(1, lineHeight));

  std::vector<std::string> lines;
  lines.reserve(static_cast<size_t>(maxLines));
  size_t pos = 0;
  while (pos < footnoteCardText.size() && static_cast<int>(lines.size()) < maxLines) {
    const size_t lineStart = pos;
    int width = 0;
    size_t lastFit = pos;
    while (pos < footnoteCardText.size()) {
      const size_t len = footnoteUtf8Length(footnoteCardText.c_str() + pos);
      char glyph[8] = {0};
      std::memcpy(glyph, footnoteCardText.c_str() + pos, std::min<size_t>(len, sizeof(glyph) - 1));
      const int advance = renderer.getTextAdvanceX(fontId, glyph, EpdFontFamily::REGULAR);
      if (width + advance > textW && pos > lineStart) break;
      width += advance;
      pos += len;
      lastFit = pos;
    }
    if (lastFit == lineStart) break;  // A single glyph wider than the card.
    lines.emplace_back(footnoteCardText, lineStart, lastFit - lineStart);
  }

  const int contentHeight = static_cast<int>(lines.size()) * lineHeight;
  int cardH = padding * 2 + contentHeight + footerHeight;
  cardH = std::min(cardH, screenH * 6 / 10);
  const int cardX = (screenW - cardW) / 2;
  const int cardY = (screenH - cardH) / 2;
  footnoteCardX = static_cast<int16_t>(cardX);
  footnoteCardY = static_cast<int16_t>(cardY);
  footnoteCardW = static_cast<int16_t>(cardW);
  footnoteCardH = static_cast<int16_t>(cardH);
  footnoteCardButtonY = static_cast<int16_t>(cardY + cardH - footerHeight);

  renderer.fillRoundedRect(cardX, cardY, cardW, cardH, 14, Color::White);
  renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 2, 14, true);
  int y = cardY + padding;
  for (const auto& line : lines) {
    renderer.drawText(fontId, cardX + padding, y, line.c_str(), true);
    y += lineHeight;
  }
  if (footnoteCardHasPage) {
    // Reuses the existing footnotes label: the footer row opens the full page.
    const char* label = I18N.get(StrId::STR_FOOTNOTES);
    const int labelW = renderer.getTextWidth(fontId, label);
    renderer.drawText(fontId, cardX + cardW - padding - labelW, footnoteCardButtonY + 4, label, true);
  }
}

bool EpubReaderActivity::handleFootnoteCardInput() {
  if (!footnoteCardActive) return false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeFootnoteCard();
    return true;
  }
  int tx = 0;
  int ty = 0;
  if (!mappedInput.wasScreenTapped(tx, ty)) return true;  // modal: swallow the rest
  const bool inside = tx >= footnoteCardX && tx < footnoteCardX + footnoteCardW && ty >= footnoteCardY &&
                      ty < footnoteCardY + footnoteCardH;
  if (!inside) {
    closeFootnoteCard();
    return true;
  }
  if (footnoteCardHasPage && ty >= footnoteCardButtonY) {
    const std::string href = footnoteCardHref;
    closeFootnoteCard();
    navigateToHref(href.c_str(), true);
  }
  return true;
}

void EpubReaderActivity::openFootnoteSelect(const bool reopenMenuOnCancel) {
  if (!section || currentPageFootnotes.empty()) return;
  if (currentPageFootnotes.size() == 1) {
    // Show the note in place; openFootnoteCard falls back to the old jump when
    // the target has no separate body (same-file anchor or unreadable item).
    openFootnoteCard(currentPageFootnotes[0].href);
    return;
  }

  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  auto selector = makeUniqueNoThrow<EpubReaderFootnoteSelectActivity>(renderer, mappedInput, std::move(page),
                                                                      orientedMarginLeft, orientedMarginTop);
  if (!selector) {
    LOG_ERR("ERA", "OOM: EpubReaderFootnoteSelectActivity");
    return;
  }
  startActivityForResult(std::move(selector), [this, reopenMenuOnCancel](const ActivityResult& result) {
    READING_STATS.resumeSession();
    if (result.isCancelled) {
      if (reopenMenuOnCancel) {
        openReaderMenu();
      } else {
        requestUpdate();
      }
      return;
    }
    const auto& footnoteResult = std::get<FootnoteResult>(result.data);
    navigateToHref(footnoteResult.href, true);
    requestUpdate();
  });
}

void EpubReaderActivity::loop() {
  if (loadFailurePopup.isActive()) {
    loadFailurePopup.handleInput(mappedInput, [this] { requestUpdate(); });
    return;
  }
  if (!epub) {
    finish();
    return;
  }

  // The first-run page owns input until dismissed: any tap or Confirm/Back
  // closes it, and that same tap must not also turn the page. The bottom note
  // is the one exception -- it opens Settings -> Controls first and still
  // counts as having read the page.
  if (gestureHintVisible) {
    int hintX = 0;
    int hintY = 0;
    const bool tapped = mappedInput.wasScreenTapped(hintX, hintY);
    const bool settingsTapped =
        tapped && gestureHintSettingsRect_.width > 0 && hintX >= gestureHintSettingsRect_.x &&
        hintX < gestureHintSettingsRect_.x + gestureHintSettingsRect_.width && hintY >= gestureHintSettingsRect_.y &&
        hintY < gestureHintSettingsRect_.y + gestureHintSettingsRect_.height;
    const bool dismiss =
        tapped || mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (dismiss) {
      gestureHintVisible = false;
      SETTINGS.readerGestureHintSeen = 1;
      // A full-screen page came and went; a FAST differential would leave its
      // frame on the glass. Force the clean base on the way back to the text.
      forcedRefreshPending = true;
      // Persist on the UI task: saveToFile() touches NVS, and the render task's
      // stack lives in PSRAM where an internal-flash write would panic.
      if (!SETTINGS.saveToFile()) LOG_ERR("ERS", "gesture hint: could not persist dismissal");
      if (settingsTapped) {
        // Settings -> Controls is category 2 (the paging gestures live there).
        startActivityForResultWith<SettingsActivity>(
            [this](const ActivityResult&) { requestUpdate(); }, 2);
        return;
      }
      requestUpdate();
    }
    return;
  }

  if (fontPromptState != FontPromptState::Idle) {
    lastPageTurnTime = millis();
    handleFontPreloadPrompt();
    return;
  }
  // Child activities suspend this loop; their temporary Overlay::None is not
  // a return to reading. Panel/tool switches retain the same preview session.
  if (overlay == Overlay::None && fontPreview.active()) {
    lastPageTurnTime = millis();
    finishFontPreview();
    return;
  }

  if (handleSelectionInput()) return;
  if (overlay == Overlay::None && handleSelectionTouch()) return;
  if (fontStepHintPointSize != 0 && static_cast<int32_t>(millis() - fontStepHintUntilMs) >= 0) {
    fontStepHintPointSize = 0;
    requestUpdate();
  }
  if (handleAnnotationCardInput()) return;
  if (overlay == Overlay::None && handleAnnotationBubbleTap()) return;
  if (handleFootnoteCardInput()) return;
  if (overlay == Overlay::None && handlePinchGesture()) return;

#ifdef ENABLE_CHINESE_VERSION
  if (maybeOfferCompleteChineseFont()) return;
#endif

  READING_STATS.tickActiveSession();
  rememberBookOnceRendered();

  // Someone else turned the screen while this reader was stacked (the control
  // center's orientation tile). Reflow before the next render, or the page
  // would be drawn with a layout built for the previous frame size.
  if (appliedOrientation != SETTINGS.orientation) {
    applyOrientation(SETTINGS.orientation);
    requestUpdate();
    return;
  }

#if FREEINK_DEVICE_READPICO
  // A2: tick the status-bar clock once a minute while the reader sits idle. It
  // pushes a windowed refresh of just the changed clock pixels, so the page is
  // never re-rendered for a clock update.
  refreshStatusBarClockIfDue();
#endif

#if !FREEINK_DEVICE_READPICO
  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  auto* fcm = renderer.getFontCacheManager();
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock() && section && !section->isBuilding() && renderer.hasFrameBuffer() &&
        lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
        ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC && fcm &&
        fcm->canIdlePrewarm(SETTINGS.getReaderFontId()) &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          const auto t0 = millis();
          auto scope = fcm->createPrewarmScope();
          p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);
          scope.endScanAndPrewarm();
          LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
        }
      }
    }
  }

#endif

  if (!updateChapterBuild()) return;

  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getSourcePath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getSourcePath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getSourcePath());
  } else {
    pendingReadFolderMove = false;
  }

  const auto touch =
      ReaderUtils::detectTouchPageTurn(renderer, mappedInput, ReaderUtils::isRtlBookLanguage(epub->getLanguage()));

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  // The toolbar reader menu owns all input while shown, ahead of the automatic page turn
  // below: the More panel's rate popup switches automatic turning on and leaves the panel
  // open, so the timer must neither flip the page under it nor eat the panel's next
  // Confirm/Back release.
  if (overlay != Overlay::None) {
    if (usesToolbarMenu()) {
      // Hold the interval at zero elapsed so closing the panel starts a fresh one.
      lastPageTurnTime = millis();
      handleOverlayInput();
      return;
    }
    // The style was switched off while an overlay was up (Settings reached via
    // the More panel); fall back to the clean page.
    overlay = Overlay::None;
    discardOverlayPage();
    requestUpdate();
    return;
  }

  switch (mappedInput.homeButtonAction()) {
    case HomeButtonAction::ReaderMenu:
    case HomeButtonAction::Bookmark:
    case HomeButtonAction::Sync:
    case HomeButtonAction::Dictionary:
    case HomeButtonAction::Footnotes:
      automaticPageTurnActive = false;
      break;
    default:
      break;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      const bool succeeded = pageTurn(true);
      notePageTurn(true, succeeded);
      requestUpdate();
      return;
    }
  }

  // While the end-of-book suggestion menu is up it owns Confirm/Back/navigation, so it
  // gets this tick's input first and the long-press shortcuts below stay inert behind it
  // -- a hold there must not drop a bookmark onto the suggestion screen or paint the
  // dictionary word picker over it. Anything the menu does not handle (long-press Back to
  // the file browser, say) still falls through to the regular handlers.
  if (handleEndOfBookMenu()) {
    return;
  }
  const bool endOfBookMenuOpen = endOfBookMenuActive();

  const unsigned long confirmHoldMs = confirmLongPressThreshold();
  // wasLongPressed() suppresses the release that follows it, so leave it unpolled while
  // the end-of-book menu owns Confirm -- otherwise the menu never sees that release.
  const bool confirmLongPressed = !endOfBookMenuOpen && confirmHoldMs != 0 &&
                                  mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, confirmHoldMs);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmLongPressed) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        addBookmark();
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
#ifdef ENABLE_CHINESE_VERSION
        if (wereadBookId_[0] && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS && launchWeReadSync()) return;
#endif
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS && launchKOReaderSync()) return;
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          openDictionaryWordSelect();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_READER_MENU:
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  if (!endOfBookMenuOpen) {
    switch (mappedInput.homeButtonAction()) {
      case HomeButtonAction::Bookmark:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case HomeButtonAction::Sync:
#ifdef ENABLE_CHINESE_VERSION
        if (wereadBookId_[0]) {
          launchWeReadSync();
          return;
        }
#endif
        launchKOReaderSync();
        return;
      case HomeButtonAction::Dictionary:
        if (!showDictionaryMessage) openDictionaryWordSelect();
        return;
      case HomeButtonAction::ReaderMenu:
        if (usesToolbarMenu() && section)
          openOverlay(Overlay::Toolbar);
        else
          openReaderMenu();
        return;
      default:
        break;
    }
  }

  // Comment bubbles and link taps take priority over the reader-menu and
  // page-turn zones. Both read the same one-shot tap; wasScreenTapped() does not
  // consume it, so checking bubbles first cannot starve a link.
  if (!atEndOfBook && SETTINGS.touchReaderControls && mappedInput.hasTouch()) {
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.wasScreenTapped(touchX, touchY)) {
#ifdef BOARD_HAS_PSRAM
      if (jjwxcActionActive_ && touchX >= jjwxcActionRect_.x &&
          touchX < jjwxcActionRect_.x + jjwxcActionRect_.width && touchY >= jjwxcActionRect_.y &&
          touchY < jjwxcActionRect_.y + jjwxcActionRect_.height) {
        openJjwxcChapterAction(jjwxcActionChapterId_, jjwxcActionPurchase_, jjwxcActionOrdinal_);
        return;
      }
      // The end-of-book pill: same one-shot tap, and it is the reason the last
      // page is not a dead end for a serialised book.
      if (jjwxcMoreActive_ && touchX >= jjwxcMoreRect_.x && touchX < jjwxcMoreRect_.x + jjwxcMoreRect_.width &&
          touchY >= jjwxcMoreRect_.y && touchY < jjwxcMoreRect_.y + jjwxcMoreRect_.height) {
        jjwxcMoreActive_ = false;
        openJjwxcUpdate(/*currentChapter=*/false);
        return;
      }
      for (int i = 0; i < commentBubbleCount_; ++i) {
        const auto& bubble = commentBubbles_[i];
        const int hitLeft = bubble.x - kCommentBubbleHitPadLeft;
        const int hitRight = bubble.x + bubble.w + kCommentBubbleHitPadRight;
        const int hitTop = bubble.y - kCommentBubbleHitPadY;
        const int hitBottom = bubble.y + bubble.h + kCommentBubbleHitPadY;
        if (touchX >= hitLeft && touchX < hitRight && touchY >= hitTop && touchY < hitBottom) {
          openCommentBubble(bubble);
          return;
        }
      }
#endif
      if (!currentPageLinks.empty()) {
        const auto* link = EpubReaderUtils::linkAtPoint(currentPageLinks, touchX, touchY, currentPageLinkMarginLeft,
                                                        currentPageLinkMarginTop);
        if (link) {
          navigateToHref(link->href, true);
          return;
        }
      }
      // Top-right corner: add/remove the bookmark for this page. Placed after
      // links so a link that reaches into the margin still wins, and before the
      // page-turn zones so the corner is not a "next page" tap.
      if (bookmarkCornerRect_.width > 0 && touchX >= bookmarkCornerRect_.x &&
          touchX < bookmarkCornerRect_.x + bookmarkCornerRect_.width && touchY >= bookmarkCornerRect_.y &&
          touchY < bookmarkCornerRect_.y + bookmarkCornerRect_.height) {
        addBookmark();
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
        return;
      }
    }
  }

  if (confirmReleased || ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    // Toolbar style: the page is on screen and in the framebuffer, so paint the
    // toolbar over it (one refresh) instead of pushing a full-screen menu.
    if (usesToolbarMenu() && section) {
      clearPendingTurns();
      openOverlay(Overlay::Toolbar);
    } else {
      openReaderMenu();
    }
  }

  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if ((!endOfBookMenuOpen && mappedInput.homeButtonAction() == HomeButtonAction::Footnotes) ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
       mappedInput.wasReleased(MappedInputManager::Button::Power) &&
       !mappedInput.wasReleased(MappedInputManager::Button::Down))) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      openFootnoteSelect(false);
    }
    return;
  }

  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingTurnDelta != 0 && !turnGuardActive) {
    if (!section) {
      clearPendingTurns();
      return;
    }
    // Coalesced drain: apply the whole net delta as state-only pageTurn steps,
    // then render once. A spine crossing resets `section`, so the leftover is
    // requeued and continues after the render task repaginates the new chapter
    // (the turn guard keeps this loop from racing that render).
    const bool forward = pendingTurnDelta > 0;
    const int total = pendingTurnDelta > 0 ? pendingTurnDelta : -pendingTurnDelta;
    pendingTurnDelta = 0;
    int consumed = 0;
    bool succeeded = false;
    while (consumed < total) {
      succeeded = pageTurn(forward);
      notePageTurn(forward, succeeded);
      ++consumed;
      if (!succeeded || !section) break;  // book boundary, or spine crossed
    }
    const int leftover = total - consumed;
    if (total > 1) {
      // A merged jump is not a sequential turn: no water-ripple animation.
      waterTurnDirection = 0;
      if (leftover > 0 && succeeded && !section) {
        pendingTurnDelta = static_cast<int8_t>(forward ? leftover : -leftover);
      }
      LOG_INF("ERS", "coalesced %d queued turns into %d rendered", total, consumed);
    }
    requestUpdate();
    return;
  }

  auto [prevTriggered, nextTriggered, fromMotion] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromMotion && heldMs >= ReaderUtils::SKIP_HOLD_MS;
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    const bool succeeded = skipPages(nextTriggered ? 1 : -1);
    notePageTurn(false, succeeded);
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (!section) {
    requestUpdate();
    return;
  }

  if (turnGuardActive) {
    activityManager.cancelIdleRender();
    queueManualTurn(prevTriggered ? -1 : 1);
    return;
  }

  if (prevTriggered) {
    const bool succeeded = pageTurn(false);
    notePageTurn(false, succeeded);
  } else {
    const bool succeeded = pageTurn(true);
    notePageTurn(true, succeeded);
  }
  requestUpdate();
}

bool EpubReaderActivity::jumpToFraction(float fraction) {
  if (!epub || !std::isfinite(fraction)) return false;
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return false;
  fraction = std::clamp(fraction, 0.0f, 1.0f);

  const size_t targetSize =
      fraction >= 1.0f ? bookSize - 1 : static_cast<size_t>(static_cast<double>(bookSize) * fraction);

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return false;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
  return true;
}

void EpubReaderActivity::jumpToPercent(const int percent) {
  jumpToFraction(static_cast<float>(clampPercent(percent)) / 100.0f);
}

// Shared by the menu-child-activity result handler and the in-sheet
// bookmarks panel: jump the reader to a saved or computed progress point.
void EpubReaderActivity::applyProgressChange(const ProgressChangeResult& sync) {

      if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
        RenderLock lock;
        clearDeferredReposition();
        if (section && currentSpineIndex == sync.spineIndex) {
          const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
          section->currentPage = page.value_or(std::max(0, sync.page));
        } else {
          currentSpineIndex = sync.spineIndex;
          pendingOffsetJump = sync.visibleTextOffset;
          nextPageNumber = std::max(0, sync.page);
          section.reset();
        }
        requestUpdate();
        return;
      }

      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      RenderLock lock;
      clearDeferredReposition();

      if (currentSpineIndex != targetSpineIndex) {
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
      requestUpdate();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    READING_STATS.resumeSession();
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
      return;
    }
    applyProgressChange(std::get<ProgressChangeResult>(result.data));
  };

  switch (action) {
#ifdef BOARD_HAS_PSRAM
    case EpubReaderMenuActivity::MenuAction::JJWXC_UPDATE_CURRENT:
      openJjwxcUpdate(/*currentChapter=*/true);
      return;
    case EpubReaderMenuActivity::MenuAction::JJWXC_UPDATE_LIST:
      openJjwxcUpdate(/*currentChapter=*/false);
      return;
    case EpubReaderMenuActivity::MenuAction::JJWXC_BOOK_DETAIL:
      openJjwxcBookDetail();
      return;
    case EpubReaderMenuActivity::MenuAction::JJWXC_CLOUD_PROGRESS:
      // The app's detail page reads 晋江's cloud position (and writes this
      // device's one back), which is why the row opens that page rather than
      // growing a second network path inside the reader.
      openJjwxcBookDetail();
      return;
#endif
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const int tocIdx = currentTocIndex();
      // Release the section while the chapter list is up (mirrors the
      // TEXT_SETTINGS path): picking a chapter resets it anyway, and its
      // tens-of-KB footprint is the difference between the chapter list
      // holding its CJK glyph arena (RAM-only repaints) and re-reading
      // glyphs from SD on every row step. Cancel restores via the same
      // cached-position rebuild TEXT_SETTINGS uses.
      {
        RenderLock lock;
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
      }
      bool numberedChapters = false;
#ifdef BOARD_HAS_PSRAM
      resolveJjwxcBook();
      numberedChapters = jjwxcBook_;
#endif
      startActivityForResultWith<EpubReaderChapterSelectionActivity>(
          [this](const ActivityResult& result) {
            READING_STATS.resumeSession();
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& chapterResult = std::get<ChapterResult>(result.data);
            RenderLock lock;
            clearDeferredReposition();
            currentSpineIndex = chapterResult.spineIndex;
            pendingAnchor = chapterResult.anchor;
            nextPageNumber = 0;
            section.reset();
            requestUpdate();
          },
          epub, spineIdx, tocIdx, numberedChapters);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      openFootnoteSelect(true);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      startActivityForResultWith<TextSettingsActivity>(
          [this](const ActivityResult&) {
            READING_STATS.resumeSession();
            applyReaderTextSettings();
            openReaderMenu();
          },
          &sdFontSystem.registry(), TextSettingsActivity::Tab::Family);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
      // Both open option popups inside EpubReaderMenuActivity and only reach
      // the result callback after another action closes the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int initialPercent = bookPercentFor(chapterPosition());
      startActivityForResultWith<EpubReaderPercentSelectionActivity>(
          [this](const ActivityResult& result) {
            READING_STATS.resumeSession();
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          },
          initialPercent);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResultWith<QrDisplayActivity>(
              [this](const ActivityResult&) {
                READING_STATS.resumeSession();
                openReaderMenu();
              },
              fullText);
          break;
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock;
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock;
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
#ifdef ENABLE_CHINESE_VERSION
      if (wereadBookId_[0]) {
        launchWeReadSync();
        break;
      }
#endif
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResultWith<EpubReaderBookmarksActivity>(progressChangeResultHandler, epub, epub->getSourcePath());
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      showBookmarkMessage = true;
      bookmarkMessageTime = millis();
      break;
    }
  }
}

unsigned long EpubReaderActivity::confirmLongPressThreshold() const {
  switch (SETTINGS.longPressMenuFunction) {
    case CrossPointSettings::LP_MENU_BOOKMARK:
    case CrossPointSettings::LP_MENU_DICTIONARY:
      return ReaderUtils::BOOKMARK_HOLD_MS;
    case CrossPointSettings::LP_MENU_KOSYNC:
#ifdef ENABLE_CHINESE_VERSION
      if (wereadBookId_[0]) return ReaderUtils::GO_HOME_MS;
#endif
      return KOREADER_STORE.hasCredentials() ? ReaderUtils::GO_HOME_MS : 0;
    case CrossPointSettings::LP_MENU_READER_MENU:
    case CrossPointSettings::LP_MENU_DISABLED:
    default:
      return 0;
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;

  RenderLock renderLock;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos;
  std::string localChapterName = currentChapterTitle();
  const std::string savedEpubPath = epub->getSourcePath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    if (section) {
      nextPageNumber = section->currentPage;
    }
    discardOverlayPage();
    ImageBlock::releaseRenderCache();
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseSdFontCaches();
    }
    // No rendering may run while the chapter mapper borrows the framebuffer.
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
    }
    // The destructor can no longer save the back-stack once epub is gone.
    if (footnoteDepth > 0) saveLinkStack();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  return activityManager.replaceActivityWith<KOReaderSyncActivity>(savedEpubPath, localPos, std::move(localKoPos),
                                                                   std::move(localChapterName));
}

#ifdef ENABLE_CHINESE_VERSION
bool EpubReaderActivity::launchWeReadSync() {
  if (!wereadBookId_[0]) return false;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  const float chapterFraction =
      totalPages > 1 ? static_cast<float>(currentPage) / static_cast<float>(totalPages - 1) : 0.0f;
  const float localFraction = epub->calculateProgress(currentSpineIndex, chapterFraction);
  const CrossPointPosition localPosition = getCurrentPosition();
  std::string savedEpubPath = epub->getSourcePath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("WRSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  const auto context = WeReadProgressSyncActivity::makeContext(*epub, wereadBookId_, localFraction, localPosition);
  auto sync = makeUniqueNoThrow<WeReadProgressSyncActivity>(renderer, mappedInput, std::move(savedEpubPath),
                                                            wereadBookId_, context);
  if (!sync) {
    LOG_ERR("WRSync", "OOM: WeReadProgressSyncActivity (%u bytes)",
            static_cast<unsigned>(sizeof(WeReadProgressSyncActivity)));
    pendingSyncLaunchError = true;
    requestUpdate();
    return true;
  }

  {
    RenderLock lock;
    if (section) nextPageNumber = section->currentPage;
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }
  activityManager.replaceActivity(std::move(sync));
  return true;
}
#endif

void EpubReaderActivity::applyInitialOrientation() {
  ReaderActivity::applyInitialOrientation();
  appliedOrientation = SETTINGS.orientation;
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // Also runs when SETTINGS already holds the new value but this layout was
  // built for the old one — that is what an external change looks like here.
  if (SETTINGS.orientation == orientation && appliedOrientation == orientation) {
    return;
  }

  RenderLock lock(*this);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  if (SETTINGS.orientation != orientation) {
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  appliedOrientation = orientation;
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t requestedPageTurnRate) {
  if (requestedPageTurnRate == 0 || requestedPageTurnRate > MAX_PAGE_TURN_RATE) {
    automaticPageTurnActive = false;
    return;
  }

  pageTurnRate = requestedPageTurnRate;
  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / pageTurnRate;
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

bool EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (!section) return false;
  READING_STATS.noteActivity();
  // 水波纹请求随翻页动作记录；只有 renderContents 的顺序文字页提交会消费它，
  // 书末分支会清掉。没有该引擎的面板在 PanelDriver 默认实现里是 no-op。
  // / The water-ripple request is recorded with the turn itself; only the sequential
  // text-page commit in renderContents consumes it, and the end-of-book branch below
  // clears it. Panels without the engine no-op via the PanelDriver default.
  waterTurnDirection = SETTINGS.pageTurnEffect == 1 ? (isForwardTurn ? 1 : -1) : 0;
  {
    RenderLock lock;
    clearDeferredReposition();
  }
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
      lastPageTurnTime = millis();
      return true;
    } else {
      currentSpineIndex = epub->getSpineItemsCount();
      waterTurnDirection = 0;  // 书末选项界面不是顺序翻页 / the end-of-book screen is not a turn
      lastPageTurnTime = millis();
      return true;
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      currentSpineIndex--;
      section.reset();
      lastPageTurnTime = millis();
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::skipPages(int amount) {
  if (!section) return false;
  READING_STATS.noteActivity();
  if (amount > 0) {
    RenderLock lock;
    nextPageNumber = 0;
    currentSpineIndex++;
    section.reset();
    return true;
  } else {
    if (section->currentPage > 0) {
      section->currentPage = 0;
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      section.reset();
      return true;
    }
  }
  return false;
}

// Failed protected open: show the standard option dialog (wrapped message)
// instead of silently falling back to the previous screen. Exact error
// strings are set by openProtectedBook (ContentProtection.cpp).
bool EpubReaderActivity::handleLoadFailure() {
  if (loadProtectionError.empty()) return false;
  const std::string& perr = loadProtectionError;
  StrId msg = StrId::STR_DRM_PROTECTED_FILE;
  bool offerSync = false;
  if (perr == "txt progress migration failed") {
    msg = StrId::STR_TXT_PROGRESS_MIGRATION_FAILED;
  } else if (perr == "fb2 conversion failed") {
    msg = StrId::STR_FILE_OPEN_FAILED;
  } else if (perr == "access expired") {
    msg = StrId::STR_LOAN_EXPIRED;
  } else if (perr == "loan date unverified") {
    msg = StrId::STR_LOAN_TIME_UNVERIFIED;
    offerSync = true;
  }
  const char* options[2] = {I18N.get(offerSync ? StrId::STR_CLOCK_SYNC_NOW : StrId::STR_OK_BUTTON),
                            I18N.get(StrId::STR_OK_BUTTON)};
  loadFailurePopup.showMessage("", I18N.get(msg), options, offerSync ? 2 : 1, 0, [this, offerSync](const int index) {
    if (offerSync && index == 0) {
      beginLoanTimeSync();
      return;
    }
    finish();
  });
  requestUpdate();
  return true;  // stay alive; the popup's Back dismiss lands in loop()'s !epub finish
}

void EpubReaderActivity::beginLoanTimeSync() {
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("ERS", "OOM: Wi-Fi selection for loan time sync");
    finish();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
      finish();
      return;
    }
    GUI.drawPopup(renderer, tr(STR_SYNCING_TIME));
    const bool synced = trustedtime::syncNow(5000);
    WiFi.disconnect(false);
    delay(30);
    if (!synced) {
      // Reopening would hit the same unverified-time refusal: offer a retry.
      const char* options[2] = {I18N.get(StrId::STR_RETRY), I18N.get(StrId::STR_OK_BUTTON)};
      loadFailurePopup.showMessage("", I18N.get(StrId::STR_CLOCK_SYNC_FAIL), options, 2, 0, [this](const int index) {
        if (index == 0) {
          beginLoanTimeSync();
          return;
        }
        finish();
      });
      requestUpdate();
      return;
    }
    APP_STATE.openEpubPath = bookPath;
    APP_STATE.saveToFile();
    // Reboot straight back into this book with a clean heap (no-op on touch
    // boards, which fall through to the in-place relaunch below).
    silentRestartToReader();
    activityManager.goToReader(bookPath);
  });
}

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (epub && epub->getSpineItemsCount() > 0) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
  }
}

bool EpubReaderActivity::backgroundBuildWanted() const {
  return section && section->isBuilding() &&
         (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
}

void EpubReaderActivity::render(RenderLock&& lock) {
  if (loadFailurePopup.isActive()) {
    renderer.clearScreen();
    loadFailurePopup.processRender(renderer, mappedInput);
    return;
  }
  if (fontPromptState == FontPromptState::TooLarge) {
    fontpreload::drawTooLargeNotice(renderer);
    renderer.displayBuffer();
    return;
  }
  ReaderActivity::render(std::move(lock));
  // Rebuild the page underneath, including after a global control-center visit.
  if (fontPromptState != FontPromptState::Idle) {
    overlayPopup.processRender(renderer, mappedInput);
  }
}

bool EpubReaderActivity::skipLoopDelay() {
  // The main loop holds the render lock while querying this hint.
  return !buildHeapPaused && backgroundBuildWanted();
}

void EpubReaderActivity::renderBook() {
  currentPageLinks.clear();
#ifdef BOARD_HAS_PSRAM
  // Cleared here rather than only in renderContents: the empty/out-of-bounds
  // early returns below never reach renderContents, and a stale rectangle must
  // not survive onto a page that has no bubbles.
  commentBubbleCount_ = 0;
#endif
  if (!epub) return;
  // Runs under the render task's RenderLock; catches every requestUpdate()
  // exit from the overlay while its deferred chrome refresh is still pending.
  settleOverlayRefresh();

  // Apply the reader's image-resampling choice before anything is drawn. The
  // setting can change from the reader menu, which does NOT reload the book, so
  // syncing only in loadBook() left the previous filter in force for the rest of
  // the session — the menu looked like it did nothing. The setter is a no-op
  // when the filter has not changed, so this costs a compare per render.
  ImageBlock::setBilinearScaling(SETTINGS.imageScaling == CrossPointSettings::IMAGE_SCALING_BILINEAR);

  const auto showPendingSyncSaveError = [this]() {
    if (pendingSyncSaveError) {
      pendingSyncSaveError = false;
      GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
    } else if (pendingSyncLaunchError) {
      pendingSyncLaunchError = false;
      GUI.drawPopup(renderer, tr(STR_SYNC_FAILED_MSG));
    }
  };

  const auto showBuildError = [this]() {
    renderer.clearScreen();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
  };

  if (failedBuildSpine_ == currentSpineIndex) {
    showBuildError();
    return;
  }
  failedBuildSpine_ = -1;

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;
  }

#ifdef BOARD_HAS_PSRAM
  // One SD read per chapter. Done here, under the render lock and before any
  // page is drawn, so the bubbles and their hit rectangles come from the same
  // sidecar state.
  resolveJjwxcBook();
  if (jjwxcBook_ && jjwxcTocMeta_.empty()) loadJjwxcTocMeta();
  if (jjwxcBook_ && currentSpineIndex != jjwxcCommentSpine_) loadJjwxcCommentCounts();
#endif

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  // refreshed UI chrome (frame 04): the header sits above the body and the wave +
  // page numbers below it, so the body reserves both bands instead of the old
  // bottom status strip.
  const ReaderChromeMetrics chrome = readerChromeMetrics();
  orientedMarginTop += std::max(0, chrome.headerBottom + kChromeHeaderGap - orientedMarginTop);
  const int chromeBottom = renderer.getScreenHeight() - chrome.footerTop + kChromeFooterGap;
  orientedMarginBottom = std::max(orientedMarginBottom, chromeBottom);
#if FREEINK_DEVICE_EEGO_A4
  // The A4's status bar is lifted 4 px so the bezel does not cover it (see
  // BaseTheme::drawStatusBar); reserve the same space for the content.
  orientedMarginBottom += 4;
#endif

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = effectiveRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer);
    if (!section) {
      LOG_ERR("ERS", "OOM: Section (%u bytes)", static_cast<unsigned>(sizeof(Section)));
      if (handleBuildFailure("section allocation", Section::BuildError::OutOfMemory)) return;
      showBuildError();
      return;
    }

#if FREEINK_DEVICE_READPICO
    ++sectionGeneration_;
#endif
    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    if (Txt::isTxtOrMd(epub->getSourcePath()) && !pendingAnchor.empty()) {
      uint32_t visibleOffset = 0;
      if (!Txt::resolveChapterPosition(epub->getSourcePath(), epub->getCachePath(), pendingAnchor, visibleOffset)) {
        LOG_ERR("ERS", "Failed to resolve TXT chapter %s", pendingAnchor.c_str());
        showBuildError();
        return;
      }
      pendingOffsetJump = visibleOffset;
      pendingAnchor.clear();
    }
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        prepareChapterBuild();
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          loan.end();
          if (handleBuildFailure("full build", section->buildError())) return;
          showBuildError();
          return;
        }
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          // Section (re)builds are the heap-hungriest path (per-word
          // allocations for the whole section). Under TTF heap pressure, shed
          // every rebuildable font cache first — dropped glyphs re-fault on
          // demand after the build. Skipped for cpfont/builtin reading: the
          // release drops the persistent advance table and mini tables, which
          // would force SD metric re-reads on every fresh chapter for no gain.
          if (!renderer.getTtfFonts().empty()) {
            if (auto* fcm = renderer.getFontCacheManager()) {
              fcm->releaseSdFontCaches();
            }
          }
          LOG_DBG("ERS", "Heap before section build: %u (max block %u)", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
          const unsigned long buildStartMs = millis();
          bool started;
          {
            prepareChapterBuild();
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(renderer, pagesUntilFullRefresh); });
          }
          if (!started) {
            if (handleBuildFailure("start", section->buildError())) return;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup(renderer, pagesUntilFullRefresh);
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              if (handleBuildFailure("foreground build", section->buildError())) return;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        legacyProgressPending = false;
        clearDeferredReposition();
      }
    }
    if (legacyProgressPending) {
      loadProtectionError = "txt progress migration failed";
      handleLoadFailure();
      return;
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
    if (stylesDisabledForSession_) {
      LOG_INF("ERS", "Basic layout ready: spine=%d page=%d (free=%u, min=%u, maxAlloc=%u)", currentSpineIndex,
              section->currentPage, static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }
  }

  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding()) prepareChapterBuild();
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      if (handleBuildFailure("partial start", section->buildError())) return;
      showBuildError();
      return;
    }
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        if (handleBuildFailure("extension build", section->buildError())) return;
        showBuildError();
        return;
      }
    }
  }
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        if (handleBuildFailure("extension build", section->buildError())) return;
        showBuildError();
        return;
      }
    }
  }

  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

#ifdef BOARD_HAS_PSRAM
  // A deferred reposition can move the spine after the load above; the counts
  // and the section must always describe the same chapter.
  if (jjwxcBook_ && currentSpineIndex != jjwxcCommentSpine_ &&
      currentSpineIndex < epub->getSpineItemsCount()) {
    loadJjwxcCommentCounts();
  }
#endif

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  // Serialize SD access in this render path against the main task's SD writes
  // (progress, bookmarks, background build) so they cannot interleave mid-FAT-op.
#if FREEINK_DEVICE_EEGO_A4 && !defined(SIMULATOR)
  HalStorage::StorageLock storageLock;
#endif

  {
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    renderedSpineIndex_ = currentSpineIndex;
    currentPageFootnotes = std::move(p->footnotes);
    currentPageLinks = std::move(p->links);
    currentPageLinkMarginLeft = orientedMarginLeft;
    currentPageLinkMarginTop = orientedMarginTop;

    // The overlay and non-tiled grayscale renderer share the renderer's single
    // stored-BW slot. Release the old page snapshot before renderContents()
    // needs that slot, then snapshot the newly rendered page below.
    discardOverlayPage();

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
    markPageRendered();
  }

  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }

  // Toolbar menu: overlay the toolbar / panel on top of the freshly rendered page.
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    overlayPageStored = renderer.storeBwBuffer();
    renderOverlay();
    // An open option picker rides on top of the freshly drawn panel.
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    // FAST, same as openOverlay: HALF's inverting pass flashes the sheet
    // (white, in night mode) on every repaint under an open panel. Any AA
    // residue a FAST differential leaves under the chrome has not shown in
    // practice; restore a HALF cleanup here if text ever visibly ghosts
    // through the sheet (see #2190 for the mechanism).
    pushOverlayRefresh();
  }
}

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  } else if (pendingSyncLaunchError) {
    pendingSyncLaunchError = false;
    GUI.drawPopup(renderer, tr(STR_SYNC_FAILED_MSG));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      } else {
        // The rebuilt section has not laid this offset out yet (the build is
        // incremental). Keep the request and retry on a later tick: falling back
        // to a proportional estimate here used currentPage == 0, which is exactly
        // why changing the font size jumped back to the top of the chapter.
        return false;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
#if FREEINK_DEVICE_EEGO_A4
    // Suppress A4's duplicate grayscale flash when the deferred reposition
    // resolves to the page already on screen.
    if (mappedOffset && currentPageVisibleOffset.has_value()) {
      if (const auto newPageOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(newPage));
          newPageOffset == currentPageVisibleOffset) {
        cachedChapterTotalPageCount = 0;  // consumed; don't read cached progress again
        cachedVisibleTextOffset.reset();
        return false;
      }
    }
#endif
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  if (legacyProgressPending) return false;
  int progressPercent = 0;
  if (epub->getBookSize() > 0 && pageCount > 0) {
    const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
    progressPercent =
        clampPercent(static_cast<int>(epub->calculateProgress(spineIndex, chapterProgress) * 100.0f + 0.5f));
  }

  std::optional<uint32_t> offset;
  if (footnoteDepth > 0 && savedPositions[0].spineIndex == spineIndex && savedPositions[0].pageNumber == currentPage) {
    offset = savedPositions[0].visibleTextOffset;
  } else if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                 ? currentPageVisibleOffset
                 : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  uint8_t chapterProgress = getStatsChapterProgressPercent(currentPage, pageCount);
  const std::string chapterTitle = getStatsChapterTitle(*epub, spineIndex, offset, &chapterProgress);
  READING_STATS.updateProgress(static_cast<uint8_t>(progressPercent), progressPercent >= 100, chapterTitle,
                               chapterProgress);
  const bool saved = EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, offset);
#ifdef ENABLE_CHINESE_VERSION
  if (saved && clearInitialProgressAfterSave_ && WeReadStore::clearInitialProgress(wereadBookId_)) {
    clearInitialProgressAfterSave_ = false;
  }
#endif
  return saved;
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

#if FREEINK_DEVICE_READPICO
bool EpubReaderActivity::pageCacheEligible() const {
  // No !section->isBuilding() here, deliberately. The page cache renders ONE page --
  // the current one or the next -- and Section::loadPage() is written to serve exactly
  // that while a build is in flight: it reads from the in-RAM build LUT, and returns
  // null for a page the build has not laid out yet. The caller already treats that null
  // as a clean skip. Gating the whole feature on the chapter being finished coupled two
  // independent things, and on this board the chapter build routinely does not finish:
  // the CSS parse needs 65,536 B and the device has ~48 KB free while reading, so the
  // build stalls (the log stops at "Page 8 processed" and never reports completion),
  // isBuilding() stays true for the rest of the session, and the cache then never
  // engages at all -- every turn is a full render instead of the cached one.
  return !pageCacheFailed_ && section && renderer.hasFrameBuffer() && !activityManager.isSwitchPending() &&
         overlay == Overlay::None && fontPromptState == FontPromptState::Idle && !renderer.isInverted() &&
         SETTINGS.textAntiAliasing && !SETTINGS.readingBackgroundEnabled && !renderer.supportsStripGrayscale() &&
         renderer.supportsTextOnlyCombinedBase();
}

ReaderPageCacheKey EpubReaderActivity::pageCacheKey(const int page, const int top, const int right, const int bottom,
                                                    const int left) const {
  return {
      .spec = effectiveRenderSpec(renderer.getScreenWidth() - left - right, renderer.getScreenHeight() - top - bottom),
      .sectionGeneration = sectionGeneration_,
      .renderEpoch = renderEpoch_,
      .commentGeneration = commentGeneration_,
      .spine = currentSpineIndex,
      .page = page,
      .top = top,
      .right = right,
      .bottom = bottom,
      .left = left,
      .orientation = SETTINGS.orientation,
      .fakeBold = SETTINGS.fakeBold,
      // A fast turn caches B/W pixels only; keep it a distinct key so switching
      // the setting cannot hand a base-only slot to the anti-aliased path.
      .antiAliasing = SETTINGS.textAntiAliasing != 0 && SETTINGS.fastPageTurn == 0,
      .inverted = renderer.isInverted(),
      .background = SETTINGS.readingBackgroundEnabled != 0,
      .guideLine = SETTINGS.readingGuideLineEnabled != 0,
      .guideStyle = SETTINGS.readingGuideLineStyle,
      .guideOffset = SETTINGS.readingGuideLineOffset};
}

// The forward slot is now built at the end of renderContents itself, so the
// scheduled idle pass would only duplicate work (and the old 10 ms window was
// still missed often). 0 means "never arm the idle pass" in the render loop.
uint32_t EpubReaderActivity::idleRenderDelayMs() const { return 0; }

void EpubReaderActivity::freePageCache() {
  for (int slot = 0; slot < kPageCacheSlots; ++slot) {
    pageCacheBase_[slot].reset();
    pageCacheLsb_[slot].reset();
    pageCacheMsb_[slot].reset();
    pageCacheStash_[slot].reset();
    pageCache_[slot].state = ReaderPageCache::State::Empty;
  }
  clockScratch_.reset();
}

void EpubReaderActivity::renderIdle(const uint32_t generation) {
  const auto cancelled = [&] { return activityManager.idleRenderCancelled(generation); };
  // Every exit below names the condition it took. Without that, "the cache never hits"
  // is indistinguishable from "the build was cancelled", "the layout moved since the
  // last render" and "the next page has images" -- and each of those needs a different
  // fix. The lines are cheap: this hook runs at most once between two renders.
  if (cancelled()) {
    LOG_DBG("ERS", "Page cache: skip, cancelled before start");
    return;
  }
  if (!pageCacheEligible()) {
    // Name every input to pageCacheEligible(), because "not eligible" is ten conditions
    // at once and only one of them is the one that fires. The display-capability pair at
    // the end is the reason this needs saying out loud: on readpico the panel is driven
    // row by row from an LCD_CAM path, so supportsStripGrayscale() and
    // supportsTextOnlyCombinedBase() can legitimately differ from the SSD1677 boards the
    // gate was written against.
    LOG_DBG("ERS",
            "Page cache: skip, not eligible (failed=%d section=%d building=%d fb=%d switch=%d ovl=%d prompt=%d "
            "inv=%d aa=%d bg=%d strip=%d combined=%d)",
            static_cast<int>(pageCacheFailed_), static_cast<int>(section != nullptr),
            static_cast<int>(section != nullptr && section->isBuilding()), static_cast<int>(renderer.hasFrameBuffer()),
            static_cast<int>(activityManager.isSwitchPending()), static_cast<int>(overlay != Overlay::None),
            static_cast<int>(fontPromptState != FontPromptState::Idle), static_cast<int>(renderer.isInverted()),
            static_cast<int>(SETTINGS.textAntiAliasing), static_cast<int>(SETTINGS.readingBackgroundEnabled),
            static_cast<int>(renderer.supportsStripGrayscale()),
            static_cast<int>(renderer.supportsTextOnlyCombinedBase()));
    return;
  }
  const auto& layout = renderedPageKey_;
  // A main-loop reflow/jump may have replaced the section since the last render.
  if (layout != pageCacheKey(section->currentPage, layout.top, layout.right, layout.bottom, layout.left)) {
    LOG_DBG("ERS", "Page cache: skip, layout moved since the last render");
    return;
  }

  // Forward only. Building both directions took ~1.4 s at ~700 ms per slot, so a
  // normal ~1 s turn cancelled the pass before either slot was ready and the cache
  // almost never hit (measured: one hit in dozens of turns). One forward slot fits
  // inside the gap between turns; a backward turn simply renders as before.
  auto forward = layout;
  ++forward.page;
  if (forward.page < static_cast<int>(section->pageCount)) buildPageCacheSlot(0, forward, generation);
}

// Build the page a forward turn lands on, right after the current page was pushed.
// The old idle pass started 10–400 ms later and was cancelled by the next tap in
// most turns (measured: one hit in 36). Cancellation still applies through the
// generation snapshot: input increments it or sets requestedUpdate.
void EpubReaderActivity::prebuildForwardSlot() {
  if (!pageCacheEligible()) return;
  const auto& layout = renderedPageKey_;
  if (layout != pageCacheKey(section->currentPage, layout.top, layout.right, layout.bottom, layout.left)) return;
  auto forward = layout;
  ++forward.page;
  if (forward.page >= static_cast<int>(section->pageCount)) return;
  buildPageCacheSlot(0, forward, activityManager.idleRenderGenerationNow());
}

// Build one page into one slot. Returns true when the slot ends up holding a finished
// page; every other exit leaves it Skipped and logs why. Runs on the render task while
// holding the render lock. Cancellation must be bounded.
bool EpubReaderActivity::buildPageCacheSlot(const int slot, const ReaderPageCacheKey& key, const uint32_t generation) {
  const auto cancelled = [&] { return activityManager.idleRenderCancelled(generation); };
  if (pageCache_[slot].attempted(key)) {
    LOG_DBG("ERS", "Page cache: slot %d skip, page %d already attempted (state=%d)", slot, key.page,
            static_cast<int>(pageCache_[slot].state));
    return false;
  }
  pageCache_[slot].key = key;
  pageCache_[slot].state = ReaderPageCache::State::Skipped;

  auto page = section->loadPage(key.page);
  if (!page) {
    LOG_ERR("ERS", "Page cache: slot %d, page %d failed to load", slot, key.page);
    return false;
  }
  if (cancelled()) {
    LOG_DBG("ERS", "Page cache: slot %d, page %d cancelled after the load", slot, key.page);
    return false;
  }
  if (page->hasImages()) {
    if (slot != 0 || !page->hasImagesNeedingDecode()) return false;
    const size_t bytes = renderer.getBufferSize();
    if (!bytes) return false;
    if (!pageCacheStash_[slot]) {
      if (!memory::psramHasHeadroom(bytes, bytes, 16 * 1024)) return false;
      pageCacheStash_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
      if (!pageCacheStash_[slot]) return false;  // Optional work must not disable the text cache.
    }
    if (cancelled()) return false;
    auto* live = renderer.getFrameBuffer();
    memcpy(pageCacheStash_[slot].get(), live, bytes);
    const auto mode = renderer.getRenderMode();
    const ScopedCleanup restore{[&] {
      renderer.setRenderMode(mode);
      memcpy(live, pageCacheStash_[slot].get(), bytes);
      ImageBlock::releaseRenderCache();
    }};
    uint32_t idleGeneration = generation;
    const CancelCheck cancellation{&idleGeneration, [](void* context) {
                                     return activityManager.idleRenderCancelled(*static_cast<uint32_t*>(context));
                                   }};
    const auto started = millis();
    GfxRenderer::FrameBufferLoan loan(renderer);
    const bool ready = page->warmImages(renderer, key.left, key.top, cancellation);
    LOG_DBG("ERS", "Image prewarm: page=%d ready=%d cancelled=%d time=%lums", key.page, ready,
            cancellation.isCancelled(), millis() - started);
    return false;  // Only .pxc is warmed; foreground still renders the grayscale image.
  }

  const size_t bytes = renderer.getBufferSize();
  if (bytes == 0) return false;
  if (!pageCacheBase_[slot]) {
    constexpr size_t kContiguousReserve = 16 * 1024;
    // Reserve the slots still to allocate; the forward slot is already charged for slot 1.
    if (memory::psramHasHeadroom(static_cast<size_t>(kPageCacheSlots - slot) * 4 * bytes, bytes, kContiguousReserve)) {
      pageCacheBase_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
      pageCacheLsb_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
      pageCacheMsb_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
      if (!pageCacheStash_[slot]) pageCacheStash_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
    }
    if (!pageCacheBase_[slot] || !pageCacheLsb_[slot] || !pageCacheMsb_[slot] || !pageCacheStash_[slot]) {
      freePageCache();
      pageCacheFailed_ = true;
      LOG_ERR("ERS", "Page cache off: PSRAM allocation of %u B failed", static_cast<unsigned>(4 * bytes));
      return false;
    }
  }
  if (cancelled()) return false;

  uint8_t* const live = renderer.getFrameBuffer();
  auto* fcm = renderer.getFontCacheManager();
  const auto started = millis();
#ifdef ENABLE_CHINESE_VERSION
  if (fcm) fcm->consumeMissingChineseCodepoint();
#endif
  memcpy(pageCacheStash_[slot].get(), live, bytes);
  struct RestoreFrame {
    GfxRenderer& renderer;
    uint8_t* live;
    const uint8_t* stash;
    size_t bytes;
    GfxRenderer::RenderMode mode;
    ~RestoreFrame() {
      renderer.setRenderMode(mode);
      memcpy(live, stash, bytes);
      if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
    }
  } restore{renderer, live, pageCacheStash_[slot].get(), bytes, renderer.getRenderMode()};
  if (fcm) {
    fcm->clearCache();
    fcm->resetStats();
  }

  // Load glyphs on demand: a whole-page prewarm cannot be interrupted. Check
  // between elements instead; an individual SD read/glyph render must finish.
  const auto renderPlane = [&](const GfxRenderer::RenderMode mode, uint8_t* target) {
    if (cancelled()) return false;
    renderer.setRenderMode(mode);
    // clearScreen() reaches the HAL on Read Pico; this is scratch only.
    memset(live, mode == GfxRenderer::BW ? 0xFF : 0x00, bytes);
    GfxRenderer::SyntheticBoldScope syntheticBold(renderer, key.fakeBold);
    for (const auto& element : page->elements) {
      if (cancelled()) return false;
      // Cached-page rebuild bypasses Page::render(), so the writing mode has to be
      // pushed onto each line here too — otherwise a cached page renders sideways.
      if (element->getTag() == TAG_PageLine) {
        static_cast<PageLine*>(element.get())
            ->setVertical(key.spec.verticalText != 0, key.spec.viewportWidth,
                          renderer.getLineHeight(key.spec.fontId, key.spec.lineCompression));
      }
      element->render(renderer, key.spec.fontId, key.left, key.top);
    }
    // Match foreground ordering: all text first, then the guide lines, then the
    // JJWXC comment bubbles (the same order renderPageWithGuideLines uses, so a
    // cached page and a freshly rendered one are pixel-identical).
    for (const auto& element : page->elements) {
      if (cancelled()) return false;
      if (!key.guideLine || element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      if (line.getBlock()->isEmpty()) continue;
      const int lineHeight = renderer.getLineHeight(key.spec.fontId, key.spec.lineCompression) +
                             line.getBlock()->getRubyShift(renderer.getFontAscenderSize(key.spec.fontId));
      const int guideY = key.top + line.yPos + lineHeight + key.guideOffset;
      if (readingGuideLine::fitsVertically(key.guideStyle, guideY, key.top, renderer.getScreenHeight() - key.bottom)) {
        readingGuideLine::draw(renderer, key.left, guideY, renderer.getScreenWidth() - key.right - 1, key.guideStyle);
      }
    }
#ifdef BOARD_HAS_PSRAM
    {
      CommentBubble bubbles[kMaxCommentBubbles];
      const int bubbleCount = layoutCommentBubbles(*page, key.spec.fontId, key.top, key.right, key.bottom, key.left,
                                                   bubbles, kMaxCommentBubbles);
      drawCommentBubbleList(bubbles, bubbleCount);
    }
#endif
    if (cancelled()) return false;
    memcpy(target, live, bytes);
    return !cancelled();
  };

  if (!renderPlane(GfxRenderer::BW, pageCacheBase_[slot].get())) return false;
  // A fast turn never pushes the gray planes, so do not render them (~2/3 of the
  // build time). The key's antiAliasing flag keeps these slots apart.
  if (SETTINGS.fastPageTurn == 0) {
    if (!renderPlane(GfxRenderer::GRAYSCALE_LSB, pageCacheLsb_[slot].get()) ||
        !renderPlane(GfxRenderer::GRAYSCALE_MSB, pageCacheMsb_[slot].get()) || cancelled())
      return false;
  } else if (cancelled()) {
    return false;
  }
#ifdef ENABLE_CHINESE_VERSION
  pageCacheMissingCodepoint_[slot] = fcm ? fcm->consumeMissingChineseCodepoint() : 0;
#endif
  pageCache_[slot].state = ReaderPageCache::State::Ready;
  LOG_DBG("ERS", "Page cache: slot %d, page %d ready in %lums", slot, key.page, millis() - started);
  return true;
}
#endif

#ifdef BOARD_HAS_PSRAM
namespace {

// "/JJWXC/<title>-<novelId>.epub". The id is taken from after the LAST '-'
// because a title may contain dashes, and it must be all digits: any other
// book in that folder was not written by this app.
bool parseJjwxcNovelIdFromPath(const std::string& path, char* out, const size_t cap) {
  constexpr const char* kPrefix = "/JJWXC/";
  constexpr size_t kPrefixLen = 7;
  constexpr const char* kSuffix = ".epub";
  constexpr size_t kSuffixLen = 5;
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  if (path.size() <= kPrefixLen + kSuffixLen) return false;
  if (path.compare(0, kPrefixLen, kPrefix) != 0) return false;
  if (path.compare(path.size() - kSuffixLen, kSuffixLen, kSuffix) != 0) return false;
  const std::string name = path.substr(kPrefixLen, path.size() - kPrefixLen - kSuffixLen);
  const size_t dash = name.rfind('-');
  if (dash == std::string::npos || dash + 1 >= name.size()) return false;
  const std::string id = name.substr(dash + 1);
  if (id.empty() || id.size() + 1 > cap) return false;
  for (const char c : id) {
    if (c < '0' || c > '9') return false;
  }
  memcpy(out, id.data(), id.size());
  out[id.size()] = '\0';
  return true;
}

}  // namespace

void EpubReaderActivity::resolveJjwxcBook() {
  if (jjwxcBookResolved_) return;
  jjwxcBookResolved_ = true;
  jjwxcNovelId_[0] = '\0';
  jjwxcBook_ = epub && parseJjwxcNovelIdFromPath(epub->getSourcePath(), jjwxcNovelId_, sizeof(jjwxcNovelId_));
  if (jjwxcBook_) LOG_DBG("ERS", "JJWXC book: novelId=%s", jjwxcNovelId_);
}

void EpubReaderActivity::loadJjwxcTocMeta() {
  jjwxcTocMeta_.clear();
  if (!jjwxcBook_ || !epub) return;
  JjwxcStore::TocReader toc;
  if (!toc.open(jjwxcNovelId_)) return;
  // A legacy EPUB packaged before placeholders were added can omit chapters
  // from its nav; toc order and nav order would then diverge and a chip would
  // describe the wrong chapter. Disable chips until "update chapter list"
  // repackages the full catalogue (which is a safe, visible no-op state).
  if (toc.count() != static_cast<uint32_t>(epub->getTocItemsCount())) {
    LOG_INF("ERS", "JJWXC toc nav mismatch (%u vs %d); chips disabled until repackage", (unsigned)toc.count(),
            epub->getTocItemsCount());
    toc.close();
    return;
  }
  // The app's in-memory catalogue is capped at 1500 chapters; states beyond
  // that cannot be acted on from the chip, and walking a 20k-record toc while
  // holding the render lock would stall the panel.
  constexpr uint32_t kMaxMeta = 1500;
  const uint32_t cap =
      std::min<uint32_t>({toc.count(), static_cast<uint32_t>(epub->getTocItemsCount()), kMaxMeta});
  if (cap == 0) {
    toc.close();
    return;
  }
  const size_t bytes = static_cast<size_t>(cap) * sizeof(JjwxcTocMeta);
  if (jjwxcTocMeta_.capacity() < cap) {
    if (!memory::psramHasHeadroom(bytes, bytes, 32 * 1024)) {
      LOG_ERR("ERS", "JJWXC toc meta: no PSRAM for %u chapters", (unsigned)cap);
      toc.close();
      return;
    }
    jjwxcTocMeta_.reserve(cap);
  }
  jjwxcTocMeta_.resize(cap);
  uint32_t loaded = 0;
  JjwxcStore::TocRecord rec;
  for (uint32_t i = 0; i < cap; ++i) {
    if (!toc.read(i, rec)) break;
    jjwxcTocMeta_[i] = JjwxcTocMeta{rec.chapterId, rec.state, rec.isVip};
    ++loaded;
  }
  toc.close();
  if (loaded < cap) jjwxcTocMeta_.resize(loaded);
  rebuildContentsRows();
  LOG_DBG("ERS", "JJWXC toc meta: %u/%u chapters", (unsigned)loaded, (unsigned)cap);
}

bool EpubReaderActivity::contentsFilterable() const {
#ifdef BOARD_HAS_PSRAM
  return jjwxcBook_ && epub != nullptr && !jjwxcTocMeta_.empty() &&
         static_cast<size_t>(epub->getTocItemsCount()) <= jjwxcTocMeta_.size();
#else
  return false;
#endif
}

void EpubReaderActivity::rebuildContentsRows() {
  contentsRows_.clear();
  if (!epub) return;
  const int n = epub->getTocItemsCount();
  if (n <= 0) return;
  bool filter = false;
#ifdef BOARD_HAS_PSRAM
  filter = contentsFilterable() && contentsFilter_ != 0;
#endif
  contentsRows_.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n && i < 32000; ++i) {
#ifdef BOARD_HAS_PSRAM
    if (filter) {
      const bool cached =
          jjwxcTocMeta_[static_cast<size_t>(i)].state == static_cast<uint8_t>(JjwxcStore::ChapterState::Cached);
      if ((contentsFilter_ == 1 && !cached) || (contentsFilter_ == 2 && cached)) continue;
    }
#endif
    contentsRows_.push_back(static_cast<int16_t>(i));
  }
}

int EpubReaderActivity::contentsTocForRow(const int row) const {
  if (row < 0 || static_cast<size_t>(row) >= contentsRows_.size()) return -1;
  return contentsRows_[static_cast<size_t>(row)];
}

int EpubReaderActivity::contentsRowForToc(const int toc) const {
  for (size_t i = 0; i < contentsRows_.size(); ++i) {
    if (contentsRows_[i] == toc) return static_cast<int>(i);
  }
  return -1;
}

void EpubReaderActivity::drawJjwxcPlaceholderAction(const int fontId, const int bottomMargin) {
  (void)fontId;
  jjwxcActionActive_ = false;
  jjwxcMoreActive_ = false;
  if (!jjwxcBook_ || !epub || jjwxcTocMeta_.empty()) return;
  const int tocIndex = currentTocIndex();
  if (tocIndex < 0 || static_cast<size_t>(tocIndex) >= jjwxcTocMeta_.size()) return;
  const auto& meta = jjwxcTocMeta_[static_cast<size_t>(tocIndex)];
  const auto state = static_cast<JjwxcStore::ChapterState>(meta.state);
  if (state == JjwxcStore::ChapterState::Cached || state == JjwxcStore::ChapterState::Locked ||
      meta.chapterId == 0) {
    return;
  }
  // VIP ownership is unknown here: the app's price page decides between caching
  // an owned chapter and asking for a purchase. The label names the intent, not
  // an assumption about the account.
  const bool purchase = meta.isVip != 0;
  const char* label = purchase ? tr(STR_JJWXC_BUY_CHAPTER) : tr(STR_JJWXC_CHAPTER_CACHE);
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int side = 32;
  const int width = std::min(360, std::max(0, screenW - 2 * side));
  constexpr int kHeight = 64;
  const int x = (screenW - width) / 2;
  const int y = screenH - bottomMargin - kHeight - 12;
  if (width < 80 || y < 0) return;
  renderer.fillRoundedRect(x, y, width, kHeight, 16, Color::White);
  renderer.drawRoundedRect(x, y, width, kHeight, 2, 16, true);
  const int labelW = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, x + (width - labelW) / 2, y + (kHeight - lineH) / 2, label, true,
                    EpdFontFamily::BOLD);
  jjwxcActionRect_ = Rect{x, y, width, kHeight};
  jjwxcActionChapterId_ = meta.chapterId;
  jjwxcActionOrdinal_ = tocIndex;
  jjwxcActionPurchase_ = purchase;
  jjwxcActionActive_ = true;
}

void EpubReaderActivity::drawJjwxcMoreChapters(const int bottomMargin) {
  jjwxcMoreActive_ = false;
  jjwxcMoreChapters_ = 0;
  if (!jjwxcBook_ || !epub || !section || jjwxcActionActive_) return;
  // Only the end of the book the reader can actually reach: the last packaged
  // chapter's last page. Anywhere else "more chapters" would compete with the
  // normal page turn for the same tap.
  if (epub->getSpineItemsCount() <= 0 || currentSpineIndex != epub->getSpineItemsCount() - 1) return;
  if (section->pageCount == 0 || section->currentPage != static_cast<int>(section->pageCount) - 1) return;
  const auto gap = jjwxcUpdates::gapForPath(epub->getSourcePath().c_str());
  if (!gap.known || gap.newChapters == 0) return;

  char label[96];
  snprintf(label, sizeof(label), I18N.get(StrId::STR_JJWXC_MORE_CHAPTERS_FMT),
           static_cast<unsigned>(std::min<uint32_t>(gap.newChapters, 9999)));
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int side = 32;
  const int width = std::min(420, std::max(0, screenW - 2 * side));
  constexpr int kHeight = 64;
  const int x = (screenW - width) / 2;
  const int y = screenH - bottomMargin - kHeight - 12;
  if (width < 80 || y < 0) return;
  renderer.fillRoundedRect(x, y, width, kHeight, 16, Color::Black);
  const int labelW = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, x + (width - labelW) / 2, y + (kHeight - lineH) / 2, label, false,
                    EpdFontFamily::BOLD);
  jjwxcMoreRect_ = Rect{x, y, width, kHeight};
  jjwxcMoreChapters_ = gap.newChapters;
  jjwxcMoreActive_ = true;
}

void EpubReaderActivity::loadJjwxcCommentCounts() {
  commentCounts_.clear();
  commentBubbleCount_ = 0;
  jjwxcChapterId_ = 0;
  jjwxcCommentSpine_ = currentSpineIndex;
  // Even a failed load must invalidate page-cache slots built from the previous
  // chapter's bubbles.
  ++commentGeneration_;
  if (!jjwxcBook_ || !epub) return;
  if (currentSpineIndex < 0 || currentSpineIndex >= epub->getSpineItemsCount()) return;

  JjwxcStore::SpineEntry entry;
  if (!JjwxcStore::readSpineEntry(jjwxcNovelId_, static_cast<uint32_t>(currentSpineIndex), entry) ||
      entry.chapterId == 0) {
    LOG_DBG("ERS", "JJWXC comments: no spine map entry for spine %d", currentSpineIndex);
    return;
  }
  jjwxcChapterId_ = entry.chapterId;

  // Sized before load: a throwing growth under -fno-exceptions aborts, and PSRAM
  // pressure is the one condition we can check cheaply.
  constexpr size_t kBytes = JjwxcCommentSidecar::kMaxEntries * sizeof(JjwxcCommentSidecar::Count);
  if (!commentCounts_.capacity()) {
    if (!memory::psramHasHeadroom(kBytes, kBytes, 32 * 1024)) {
      LOG_ERR("ERS", "JJWXC comments: no PSRAM for %u entries", (unsigned)JjwxcCommentSidecar::kMaxEntries);
      return;
    }
    commentCounts_.reserve(JjwxcCommentSidecar::kMaxEntries);
  }
  commentCounts_.resize(JjwxcCommentSidecar::kMaxEntries);
  size_t count = 0;
  if (!JjwxcStore::loadCommentCounts(jjwxcNovelId_, jjwxcChapterId_, commentCounts_.data(), commentCounts_.size(),
                                     count)) {
    commentCounts_.clear();
    return;
  }
  if (count > commentCounts_.size()) count = commentCounts_.size();
  commentCounts_.resize(count);
  LOG_DBG("ERS", "JJWXC comments: chapter=%lu %u paragraphs with counts", (unsigned long)jjwxcChapterId_,
          (unsigned)commentCounts_.size());
}

uint16_t EpubReaderActivity::commentTotalFor(const uint32_t paragraphId) const {
  // Linear is right at this size: a page asks about ~30 paragraphs and the table
  // is bounded at 512 entries, so a scan is cheaper than keeping it sorted (and
  // the sidecar's order is the server's, which nothing guarantees).
  for (const auto& entry : commentCounts_) {
    if (entry.paragraphId == paragraphId) return entry.total;
  }
  return 0;
}

int EpubReaderActivity::layoutCommentBubbles(const Page& page, const int fontId, const int top, const int right,
                                             const int bottom, const int left, CommentBubble* out,
                                             const int maxOut) const {
  if (out == nullptr || maxOut <= 0 || commentCounts_.empty()) return 0;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int contentRight = screenWidth - right;
  const int contentBottom = screenHeight - bottom;
  const int baseLineHeight = renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression());
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int numberHeight = renderer.getLineHeight(SMALL_FONT_ID);

  int count = 0;
  for (size_t i = 0; i < page.elements.size() && count < maxOut; ++i) {
    const auto& element = page.elements[i];
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const TextBlock& block = *line.getBlock();
    const uint16_t paragraphIndex = block.getParagraphIndex();
    if (paragraphIndex == 0xFFFF) continue;

    // Only the last line of the paragraph on this page gets a bubble. "Last on
    // this page" is deliberate: a paragraph that continues onto the next page
    // puts its bubble at the bottom edge, which is where the reader saw it end.
    bool lastOnPage = true;
    for (size_t j = i + 1; j < page.elements.size(); ++j) {
      if (page.elements[j]->getTag() != TAG_PageLine) continue;
      const auto& next = static_cast<const PageLine&>(*page.elements[j]);
      lastOnPage = next.getBlock()->getParagraphIndex() != paragraphIndex;
      break;
    }
    if (!lastOnPage) continue;

    const uint16_t total = commentTotalFor(paragraphIndex);
    if (total == 0) continue;

    char label[12];
    snprintf(label, sizeof(label), "%u", static_cast<unsigned>(total));
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
    const int bubbleHeight = numberHeight + 4;
    const int bubbleWidth = std::max(bubbleHeight, textWidth + 8);
    const int lineHeight = baseLineHeight + block.getRubyShift(ascender);

    int bubbleY = top + line.yPos + (lineHeight - bubbleHeight) / 2;
    if (bubbleY + bubbleHeight > contentBottom) bubbleY = contentBottom - bubbleHeight;
    if (bubbleY < top) bubbleY = top;
    if (bubbleY + bubbleHeight > contentBottom || bubbleY < 0) continue;

    int lineEnd = left + line.xPos;
    if (block.wordCount() > 0) {
      const uint16_t lastWord = block.wordCount() - 1;
      lineEnd += block.wordXpos(lastWord) +
                 renderer.getTextWidth(fontId, block.wordText(lastWord), block.wordStyle(lastWord));
    }
    int bubbleX = lineEnd + 3;
    if (bubbleX + bubbleWidth > contentRight) {
      // A full line leaves no room after its last glyph. Prefer the right
      // margin (it is outside the text area) over covering the last character.
      bubbleX = contentRight + 2;
      if (bubbleX + bubbleWidth > screenWidth - 2) bubbleX = contentRight - bubbleWidth;
    }
    if (bubbleX < left || bubbleX + bubbleWidth > screenWidth) continue;

    out[count++] = CommentBubble{static_cast<int16_t>(bubbleX), static_cast<int16_t>(bubbleY),
                                 static_cast<int16_t>(bubbleWidth), static_cast<int16_t>(bubbleHeight),
                                 paragraphIndex, total, static_cast<uint32_t>(paragraphIndex)};
  }
  return count;
}

void EpubReaderActivity::drawCommentBubbleList(const CommentBubble* bubbles, const int count) const {
  if (bubbles == nullptr || count <= 0) return;
  // Fills first, then labels: a fill must never land on a number already drawn.
  for (int i = 0; i < count; ++i) {
    const auto& bubble = bubbles[i];
    renderer.fillRoundedRect(bubble.x, bubble.y, bubble.w, bubble.h, bubble.h / 2, Color::Black);
  }
  const int numberHeight = renderer.getLineHeight(SMALL_FONT_ID);
  for (int i = 0; i < count; ++i) {
    const auto& bubble = bubbles[i];
    if (bubble.total == 0) continue;
    char label[12];
    snprintf(label, sizeof(label), "%u", static_cast<unsigned>(bubble.total));
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, bubble.x + (bubble.w - textWidth) / 2,
                      bubble.y + (bubble.h - numberHeight) / 2, label, /*black=*/false);
  }
}

void EpubReaderActivity::drawCommentBubbles() const { drawCommentBubbleList(commentBubbles_, commentBubbleCount_); }

void EpubReaderActivity::openCommentBubble(const CommentBubble& bubble) {
  if (!jjwxcBook_ || jjwxcNovelId_[0] == '\0' || jjwxcChapterId_ == 0) return;
  // Opening a comment panel is an interruption, not a page turn: stop auto-turn
  // and drop any queued turn so coming back does not flip the page underneath
  // the paragraph the user was reading.
  automaticPageTurnActive = false;
  clearPendingTurns();
  const bool started = startActivityForResultWith<JjwxcCommentActivity>(
      [this](const ActivityResult&) {
        {
          RenderLock lock;
          // The comment activity brings up Wi-Fi for its first fetch, and
          // NetworkStartup::prepare() unloads the SD-card font when internal RAM
          // is tight. Nothing re-enters the reader on resume (ActivityManager
          // only calls onEnter for a newly pushed activity), so without this the
          // page would render with the built-in font while the section still
          // carries SD-font metrics -- "the reader lost my layout until I reopen
          // the book".
          sdFontSystem.ensureLoaded(renderer);
        }
        requestUpdate();
      },
      std::string(jjwxcNovelId_), jjwxcChapterId_, bubble.paragraphId, bubble.paragraphIndex);
  if (!started) {
    LOG_ERR("ERS", "OOM: comment activity");
  }
}

void EpubReaderActivity::prepareForJjwxcChild() {
  {
    // Free the section's internal-RAM word blocks and the page cache before the
    // app starts Wi-Fi/TLS: the app is a network-bound child and this is the
    // difference between a handshake that fits and one that stalls. The position
    // is carried through cachedVisibleTextOffset/nextPageNumber.
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
      section.reset();
    }
    discardOverlayPage();
    freePageCache();
  }
  // A chip/top-row action starts from the Contents sheet; close it so the child
  // activity does not open behind a stale sheet snapshot.
  if (overlay != Overlay::None) closeOverlayToPage();
}

void EpubReaderActivity::openJjwxcUpdate(const bool currentChapter, const bool returnToContents) {
  resolveJjwxcBook();
  if (!jjwxcBook_ || jjwxcNovelId_[0] == '\0') return;

  prepareForJjwxcChild();
  JjwxcActivity::requestDeepLink(jjwxcNovelId_, currentChapter ? JjwxcActivity::DeepLinkAction::UpdateCurrentChapter
                                                               : JjwxcActivity::DeepLinkAction::UpdateChapterList);
  const bool started = startActivityForResultWith<JjwxcActivity>([this, returnToContents](const ActivityResult& result) {
    const auto* jjwxcResult = std::get_if<JjwxcReaderResult>(&result.data);
    const bool openChapterList =
        jjwxcResult != nullptr && jjwxcResult->completed &&
        jjwxcResult->action == JjwxcReaderResult::Action::ChapterList;
    bool reopened = false;
    {
      RenderLock lock;
      // The app forces Portrait on entry and the reader's SETTINGS did not change,
      // so its loop would not notice; put the requested orientation back before the
      // next layout.
      applyInitialOrientation();
      appliedOrientation = SETTINGS.orientation;
      // Same reason as the comment panel: NetworkStartup::prepare() may have
      // unloaded the SD font while the app was up.
      sdFontSystem.ensureLoaded(renderer);
      reopened = reopenBookAfterExternalChange();
    }
    if (!reopened) {
      LOG_ERR("ERS", "Cannot reopen the book after the JJWXC update");
      if (!handleLoadFailure()) finish();
      return;
    }
    requestUpdate();
    if (usesToolbarMenu() && (returnToContents || openChapterList)) {
      // The toolbar's Contents panel is the surface the user came from; the
      // classic full-screen TOC is a different screen and was the old bug.
      openContentsPanel(-1);
    } else if (openChapterList) {
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER);
    }
  });
  if (!started) {
    LOG_ERR("ERS", "OOM: JJWXC update activity");
    // Do not let a failed launch run the update on the next ordinary app entry.
    JjwxcActivity::requestDeepLink("", JjwxcActivity::DeepLinkAction::None);
  }
}

void EpubReaderActivity::openJjwxcBookDetail() {
  resolveJjwxcBook();
  if (!jjwxcBook_ || jjwxcNovelId_[0] == '\0') return;

  prepareForJjwxcChild();
  JjwxcActivity::requestDeepLink(jjwxcNovelId_, JjwxcActivity::DeepLinkAction::OpenBookDetail);
  const bool started = startActivityForResultWith<JjwxcActivity>([this](const ActivityResult&) {
    bool reopened = false;
    {
      RenderLock lock;
      applyInitialOrientation();
      appliedOrientation = SETTINGS.orientation;
      sdFontSystem.ensureLoaded(renderer);
      reopened = reopenBookAfterExternalChange();
    }
    if (!reopened) {
      LOG_ERR("ERS", "Cannot reopen the book after the JJWXC detail page");
      if (!handleLoadFailure()) finish();
      return;
    }
    requestUpdate();
  });
  if (!started) {
    LOG_ERR("ERS", "OOM: JJWXC book-detail activity");
    JjwxcActivity::requestDeepLink("", JjwxcActivity::DeepLinkAction::None);
  }
}

void EpubReaderActivity::activateJjwxcChapterChip(const int tocIndex) {
  if (!jjwxcBook_ || tocIndex < 0 || tocIndex >= epub->getTocItemsCount()) return;
  const auto item = epub->getTocItem(tocIndex);
  uint32_t ordinal = static_cast<uint32_t>(tocIndex);
  if (item.spineIndex >= 0) {
    JjwxcStore::SpineEntry entry;
    if (JjwxcStore::readSpineEntry(jjwxcNovelId_, static_cast<uint32_t>(item.spineIndex), entry)) {
      ordinal = entry.ordinal;
    }
  }
  JjwxcStore::TocReader toc;
  if (!toc.open(jjwxcNovelId_)) return;
  JjwxcStore::TocRecord rec;
  const bool ok = toc.read(ordinal, rec);
  toc.close();
  if (!ok || rec.chapterId == 0) return;
  if (rec.state == static_cast<uint8_t>(JjwxcStore::ChapterState::Locked)) return;
  const bool purchase = rec.isVip != 0 &&
                        (rec.state == static_cast<uint8_t>(JjwxcStore::ChapterState::Unpurchased) ||
                         rec.state == static_cast<uint8_t>(JjwxcStore::ChapterState::Missing));
  openJjwxcChapterAction(rec.chapterId, purchase, tocIndex);
}

void EpubReaderActivity::openJjwxcChapterAction(const uint32_t chapterId, const bool purchase, const int tocIndex) {
  resolveJjwxcBook();
  if (!jjwxcBook_ || jjwxcNovelId_[0] == '\0' || chapterId == 0) return;

  prepareForJjwxcChild();
  JjwxcActivity::requestDeepLink(
      jjwxcNovelId_, purchase ? JjwxcActivity::DeepLinkAction::PurchaseChapter : JjwxcActivity::DeepLinkAction::CacheChapter,
      chapterId, /*openChapter=*/false);
  const bool started =
      startActivityForResultWith<JjwxcActivity>([this, chapterId, tocIndex](const ActivityResult& result) {
        (void)result;  // a cancelled child still reopens the book and the panel
        bool reopened = false;
        {
          RenderLock lock;
          applyInitialOrientation();
          appliedOrientation = SETTINGS.orientation;
          sdFontSystem.ensureLoaded(renderer);
          reopened = reopenBookAfterExternalChange();
        }
        if (!reopened) {
          LOG_ERR("ERS", "Cannot reopen the book after the JJWXC chapter action");
          if (!handleLoadFailure()) finish();
          return;
        }
        int focus = tocIndex;
        uint32_t ordinal = 0;
        if (JjwxcStore::findOrdinalForChapter(jjwxcNovelId_, chapterId, ordinal)) focus = static_cast<int>(ordinal);
        requestUpdate();
        if (usesToolbarMenu()) openContentsPanel(focus);
      });
  if (!started) {
    LOG_ERR("ERS", "OOM: JJWXC chapter action activity");
    JjwxcActivity::requestDeepLink("", JjwxcActivity::DeepLinkAction::None);
  }
}

bool EpubReaderActivity::reopenBookAfterExternalChange() {
  if (!epub) return false;

  const int savedSpine = currentSpineIndex;
  int savedPage = nextPageNumber;
  std::optional<uint32_t> savedOffset = cachedVisibleTextOffset;
  if (section) {
    savedPage = section->currentPage;
    if (savedPage >= 0 && savedPage < section->pageCount) {
      savedOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(savedPage));
    }
  }

  section.reset();
  freePageCache();
  discardOverlayPage();
  currentPageLinks.clear();
  currentPageFootnotes.resize(0);
  overlay = Overlay::None;

  if (epub) {
    // The derived chapter text is plaintext and the repackaged EPUB is not; a
    // stale extraction would either mask the new body or fail to parse.
    const std::string cache = epub->getCachePath();
    Storage.removeDir((cache + "/sections").c_str());
    Storage.removeDir((cache + "/html").c_str());
  }
  releaseEmbeddedFont();

  // A title change at the site moves the JJWXC artefact to a new
  // <title>-<id>.epub path. Re-resolve it from novel.bin before reopening;
  // otherwise the reader keeps trying the old path (and a cache repackage may
  // already have removed it).
  if (jjwxcBook_ && jjwxcNovelId_[0] != '\0') {
    JjwxcStore::NovelRecord latest;
    if (JjwxcStore::loadNovel(jjwxcNovelId_, latest)) {
      const std::string path = JjwxcStore::finalBookPath(latest);
      if (!path.empty() && Storage.exists(path.c_str())) bookPath = path;
    }
  }

  // A fresh Epub is required: the old object's `decryptor` was opened when the
  // file was still plaintext, so it would keep reading ciphertext as HTML.
  auto loaded = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loaded) {
    LOG_ERR("ERS", "OOM: reopening EPUB after update");
    return false;
  }
  if (!loaded->load(/*buildIfMissing=*/true, SETTINGS.embeddedStyle == 0)) {
    LOG_ERR("ERS", "Reopen failed%s%s", loaded->getProtectionError().empty() ? "" : ": ",
            loaded->getProtectionError().c_str());
    return false;
  }
  epub = std::move(loaded);
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest, CancelCheck cancellation) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest, cancellation);
  });
  epub->setupCacheDir();
  loadEmbeddedFontIfPresent();

  currentSpineIndex = savedSpine;
  cachedSpineIndex = savedSpine;
  nextPageNumber = savedPage;
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset = savedOffset;
  pendingOffsetJump = savedOffset;
  pendingPageJump.reset();
  pendingAnchor.clear();
  pendingPercentJump = false;
  // Force the next page render to re-read this chapter's paragraph counts.
  jjwxcCommentSpine_ = -1;
  jjwxcChapterId_ = 0;
  loadJjwxcTocMeta();
  return true;
}

void EpubReaderActivity::openContentsPanel(const int focusTocIndex) {
  pendingContentsFocus_ = focusTocIndex;
  openOverlay(Overlay::Contents);
}
#endif  // BOARD_HAS_PSRAM

// First-run page (refreshed UI frame 16): "body text lives in three zones, the
// device only has three keys". It is a real page of its own -- white paper,
// no wash over the body -- and still appears exactly once. Every label comes
// from an existing StrId; the design's heading and its "no confirm key" line
// have no translation key yet, so the page leads with the touch-controls
// caption and the paging sentence instead.
void EpubReaderActivity::drawGestureHint() {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  renderer.fillRect(0, 0, w, h, false);

  const int lh10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lh12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int margin = std::max(24, std::min(40, w / 16));
  const int contentW = w - margin * 2;
  const bool landscape = w > h;
  // Short landscape panels (X3/X4 at 480 px tall) cannot hold the portrait
  // rhythm; tighten the same blocks rather than clipping the page.
  const bool compact = landscape && h < 640;
  const int gap = compact ? 4 : (landscape ? 8 : 18);
  const int smallGap = compact ? 2 : std::max(4, gap / 2);
  const int zoneH = compact ? 52 : (landscape ? 72 : 112);
  const int keyH = compact ? 52 : (landscape ? 72 : 112);
  const int buttonH = compact ? 52 : (landscape ? 64 : 96);

  const bool tapsPage = ReaderUtils::gestureAllowsTap(SETTINGS.pageTurnGesture) ||
                        ReaderUtils::gestureAllowsTap(SETTINGS.previousPageGesture);
  const char* pagingLine = tapsPage ? tr(STR_GESTURE_HINT_PAGING_TAP) : tr(STR_GESTURE_HINT_PAGING_SWIPE);

  const int total = lh10 + smallGap + lh12 + gap + zoneH + gap + lh10 + smallGap +
                    std::max(48, lh10) + gap + lh10 + smallGap + keyH + gap + lh10 + smallGap + lh10 + smallGap +
                    lh10 + gap + buttonH + smallGap + lh10;
  int y = std::max(16, (h - total) / 2);
  // Frame 16's page title, with the same rule the reader header uses. It sits
  // above the centred diagram rather than inside the measured block so the
  // three-zone composition keeps its designed whitespace.
  renderer.drawText(NOTOSERIF_18_FONT_ID, margin, std::max(12, y - 76),
                    tr(STR_GESTURE_HINT_TITLE), true, EpdFontFamily::BOLD);
  renderer.drawLine(margin, std::max(12, y - 76) + renderer.getLineHeight(NOTOSERIF_18_FONT_ID) + 10, w - margin,
                    std::max(12, y - 76) + renderer.getLineHeight(NOTOSERIF_18_FONT_ID) + 10, true);

  const auto fitted = [&](const int fontId, const char* text) {
    return renderer.truncatedText(fontId, text, contentW);
  };
  const auto caption = [&](const char* text) {
    renderer.drawText(UI_10_FONT_ID, margin, y, fitted(UI_10_FONT_ID, text).c_str(), true);
    y += lh10 + smallGap;
  };
  const auto line = [&](const char* text) {
    renderer.drawText(UI_10_FONT_ID, margin, y, fitted(UI_10_FONT_ID, text).c_str(), true);
    y += lh10 + smallGap;
  };

  // Small vector arrows keep the three zones and the three keys legible
  // without another bitmap asset per direction.
  enum class Arrow { Left, Right, Up, Down };
  const auto drawArrow = [&](const int cx, const int cy, const Arrow direction, const bool ink) {
    constexpr int kReach = 11;
    constexpr int kHead = 6;
    switch (direction) {
      case Arrow::Left:
        renderer.drawLine(cx + kReach, cy, cx - kReach, cy, 2, ink);
        renderer.drawLine(cx - kReach, cy, cx - kReach + kHead, cy - kHead, 2, ink);
        renderer.drawLine(cx - kReach, cy, cx - kReach + kHead, cy + kHead, 2, ink);
        break;
      case Arrow::Right:
        renderer.drawLine(cx - kReach, cy, cx + kReach, cy, 2, ink);
        renderer.drawLine(cx + kReach, cy, cx + kReach - kHead, cy - kHead, 2, ink);
        renderer.drawLine(cx + kReach, cy, cx + kReach - kHead, cy + kHead, 2, ink);
        break;
      case Arrow::Up:
        renderer.drawLine(cx, cy + kReach, cx, cy - kReach, 2, ink);
        renderer.drawLine(cx, cy - kReach, cx - kHead, cy - kReach + kHead, 2, ink);
        renderer.drawLine(cx, cy - kReach, cx + kHead, cy - kReach + kHead, 2, ink);
        break;
      case Arrow::Down:
        renderer.drawLine(cx, cy - kReach, cx, cy + kReach, 2, ink);
        renderer.drawLine(cx, cy + kReach, cx - kHead, cy + kReach - kHead, 2, ink);
        renderer.drawLine(cx, cy + kReach, cx + kHead, cy + kReach - kHead, 2, ink);
        break;
    }
  };

  caption(tr(STR_TOUCH_READER_CONTROLS));
  renderer.drawText(UI_12_FONT_ID, margin, y, fitted(UI_12_FONT_ID, pagingLine).c_str(), true, EpdFontFamily::BOLD);
  y += lh12 + gap;

  // Touch zones: equal thirds of the body.
  {
    const int zoneGap = std::max(6, gap / 2);
    const int zoneW = (contentW - zoneGap * 2) / 3;
    const struct Zone {
      const char* label;
      Arrow arrow;
      bool filled;
    } zones[3] = {{tr(STR_GESTURE_HINT_PREV), Arrow::Left, false},
                  {tr(STR_GESTURE_HINT_MENU), Arrow::Right, true},  // replaced by the list mark below
                  {tr(STR_GESTURE_HINT_NEXT), Arrow::Right, false}};
    for (int i = 0; i < 3; ++i) {
      const int x = margin + i * (zoneW + zoneGap);
      if (zones[i].filled) {
        renderer.fillRoundedRect(x, y, zoneW, zoneH, 12, Color::LightGray);
      }
      renderer.drawRoundedRect(x, y, zoneW, zoneH, 2, 12, true);
      const int iconCx = x + 20 + 12;
      const int centerY = y + zoneH / 2;
      if (i == 1) {
        // The middle zone opens the reader panel: three stacked lines.
        for (int row = -1; row <= 1; ++row) {
          renderer.drawLine(iconCx - 10, centerY + row * 8, iconCx + 10, centerY + row * 8, 2, true);
        }
      } else {
        drawArrow(iconCx, centerY, zones[i].arrow, true);
      }
      const int labelX = iconCx + 22;
      const int labelW = zoneW - (labelX - x) - 12;
      if (labelW > 0) {
        const std::string label = renderer.truncatedText(UI_10_FONT_ID, zones[i].label, labelW);
        const int labelY = centerY - renderer.getLineHeight(UI_10_FONT_ID) / 2;
        renderer.drawText(UI_10_FONT_ID, labelX, labelY, label.c_str(), true);
      }
    }
    y += zoneH + gap;
  }

  line(tr(STR_GESTURE_HINT_POINTS));
  {
    // Bookmark callout: a miniature of the real 28x72 ribbon, flush right,
    // with the one-line explanation.
    const int ribbonW = 20;
    const int ribbonH = 48;
    const int ribbonX = w - margin - ribbonW;
    const int ribbonY = y;
    renderer.fillRect(ribbonX, ribbonY, ribbonW, ribbonH - 8, true);
    for (int row = ribbonH - 8; row < ribbonH; ++row) {
      const int d = (row - (ribbonH - 8)) * (ribbonW / 2) / 8;
      renderer.fillRect(ribbonX + d, ribbonY + row, ribbonW - d * 2, 1, true);
    }
    const int textW = contentW - ribbonW - 16;
    const std::string label = renderer.truncatedText(UI_10_FONT_ID, tr(STR_GESTURE_HINT_BOOKMARK), textW);
    renderer.drawText(UI_10_FONT_ID, margin, ribbonY + (ribbonH - lh10) / 2, label.c_str(), true);
    y += std::max(ribbonH, lh10) + gap;
  }

  caption(tr(STR_PAGE_TURN));

  // Hardware keys: three cards, the middle one inverted like the design's
  // "pressed" state.
  {
    const int keyGap = std::max(6, gap / 2);
    const int keyW = (contentW - keyGap * 2) / 3;
    const struct Key {
      const char* label;
      Arrow arrow;
      bool pressed;
    } keys[3] = {{tr(STR_GESTURE_HINT_PREV), Arrow::Up, false},
                 {tr(STR_BACK), Arrow::Left, false},
                 {tr(STR_GESTURE_HINT_NEXT), Arrow::Down, true}};
    for (int i = 0; i < 3; ++i) {
      const int x = margin + i * (keyW + keyGap);
      if (keys[i].pressed) {
        renderer.fillRoundedRect(x, y, keyW, keyH, 12, Color::Black);
      }
      renderer.drawRoundedRect(x, y, keyW, keyH, 2, 12, true);
      const bool ink = !keys[i].pressed;
      drawArrow(x + keyW / 2, y + keyH / 4 + 6, keys[i].arrow, ink);
      const std::string label = renderer.truncatedText(UI_10_FONT_ID, keys[i].label, keyW - 12);
      const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
      renderer.drawText(UI_10_FONT_ID, x + (keyW - labelW) / 2, y + keyH - lh10 - 10, label.c_str(), ink);
    }
    y += keyH + gap;
  }

  line(tr(STR_GESTURE_HINT_LONG_PRESS));
  line(tr(STR_GESTURE_HINT_PINCH));
  line(tr(STR_GESTURE_HINT_DISMISS));

  // Start button: the design's 604x96 black pill (full content width here).
  gestureHintStartRect_ = Rect{margin, y, contentW, buttonH};
  renderer.fillRoundedRect(margin, y, contentW, buttonH, 12, Color::Black);
  {
    const std::string label = renderer.truncatedText(UI_12_FONT_ID, tr(STR_START_READING), contentW - 24);
    const int textW = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, margin + (contentW - textW) / 2,
                      y + (buttonH - lh12) / 2, label.c_str(), false, EpdFontFamily::BOLD);
  }
  y += buttonH + smallGap;

  // Bottom note: a tappable shortcut to Settings -> Controls, where the paging
  // gestures live. The design's exact sentence has no key; the caption names
  // the destination instead.
  {
    const std::string label =
        std::string(renderer.truncatedText(UI_10_FONT_ID, tr(STR_TOUCH_READER_CONTROLS), contentW - 40)) + "  >";
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
    gestureHintSettingsRect_ = Rect{std::max(0, (w - textW) / 2 - 16), y - 8, textW + 32, lh10 + 16};
    renderer.drawText(UI_10_FONT_ID, (w - textW) / 2, y, label.c_str(), true);
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  // Annotations are per book and tiny (a few hundred bytes): load once per
  // reader instance, on the first page render.
  if (!annotationsLoaded) loadAnnotations();
  const auto t0 = millis();
  // Page turns change the answer; the flag used to be refreshed only when the
  // bookmark file was loaded or toggled, so the status-bar mark (and the new
  // corner glyph) could describe the previous page.
  updateBookmarkFlag();
  // Fresh reader instance before the one-shot hint has been dismissed: show the
  // zone map over this first page.
  if (!SETTINGS.readerGestureHintSeen && !gestureHintVisible) {
    gestureHintVisible = true;
    forcedRefreshPending = true;  // the hint page is a full-screen frame
  }
  const int fontId = SETTINGS.getReaderFontId();
#ifdef BOARD_HAS_PSRAM
  jjwxcActionActive_ = false;
  // Recompute the hit geometry for this page even on a page-cache hit: the cache
  // holds finished pixels, but taps still need the rectangles. The identical
  // layout ran when the slot was built, so the pixels and the rectangles agree.
  commentBubbleCount_ =
      layoutCommentBubbles(*page, fontId, orientedMarginTop, orientedMarginRight, orientedMarginBottom,
                           orientedMarginLeft, commentBubbles_, kMaxCommentBubbles);
#endif
  const auto renderPage = [&] {
    GfxRenderer::SyntheticBoldScope syntheticBold(renderer, SETTINGS.fakeBold);
    // Vertical writing: columns are counted from the right edge of the usable
    // width, so the page must know that width before rendering.
    page->setVertical(SETTINGS.verticalText != 0,
                      renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight,
                      renderer.getLineHeight(fontId, renderedPageKey_.spec.lineCompression));
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  };

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  if (pageHasImagesNeedingDecode) {
    // Lend the fixed framebuffer to the large JPEG/PNG decoder while it streams
    // the cold image into .pxc. Formal rendering happens after the loan ends.
    fcm->clearCache();
    fcm->releaseSdFontCaches();
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      page->extractImagesNeedingDecode();
      page->cacheImagesNeedingDecode(renderer, orientedMarginLeft, orientedMarginTop);
    }
    ImageBlock::releaseRenderCache();
    renderer.clearScreen();
  }
  if (pageHasImages) {
    LOG_DBG("ERS", "Image predecode=%lums cold=%u", millis() - t0, static_cast<unsigned>(pageHasImagesNeedingDecode));
  }

#ifdef ENABLE_CHINESE_VERSION
  fcm->consumeMissingChineseCodepoint();
#endif
  struct RawFontCacheGuard {
    FontCacheManager* manager = nullptr;
    ~RawFontCacheGuard() {
      if (manager) manager->clearCache();
    }
  } rawFontCacheGuard;
#if FREEINK_DEVICE_READPICO
  const auto key = pageCacheKey(section ? section->currentPage : -1, orientedMarginTop, orientedMarginRight,
                                orientedMarginBottom, orientedMarginLeft);
  // Either slot may hold this page: slot 0 was built for it as a forward turn, slot 1
  // as a backward one. Whichever matched is recorded, because the three plane copies
  // below must read that slot and not assume one of them.
  bool pageCacheHit = false;
  if (pageCacheEligible()) {
    for (int slot = 0; slot < kPageCacheSlots; ++slot) {
      if (pageCache_[slot].ready(key)) {
        pageCacheLiveSlot_ = slot;
        pageCacheHit = true;
        break;
      }
    }
  }
  ++renderEpoch_;
  renderedPageKey_ = key;
  renderedPageKey_.renderEpoch = renderEpoch_;
#else
  [[maybe_unused]] constexpr bool pageCacheHit = false;
#endif
  std::optional<FontCacheManager::PrewarmScope> prewarmScope;
  // A hit copies finished pixels; it does not need reader glyphs or a scan pass.
  if (!pageCacheHit && fcm->needsPrewarmScan(fontId)) {
    // Font prewarm reads glyph/kern data from SD, or from internal flash for a
    // flash-cached .cpfont. Internal-flash reads disable the caches, which must
    // never overlap a panel scan, and the prewarm's rasterisation would also
    // widen ISR latency. Join the previous scan first; this runs once per font
    // switch, so it does not cost the ordinary page turn its overlap.
    renderer.waitRefreshComplete();
    prewarmScope.emplace(*fcm);
    // Scan pass records the page text only (status bar glyphs are flash-resident
    // UI fonts and would otherwise shadow the reader font in the prewarm).
    renderPage();
    prewarmScope->endScanAndPrewarm();
  } else if (!pageCacheHit) {
    fcm->clearCache();
    fcm->resetStats();
    rawFontCacheGuard.manager = fcm;
  }
  const auto tPrewarm = millis();

  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  // Night mode renders crisp B/W; the SDK disables every grayscale display path.
  const bool grayscaleEnabled = !renderer.isInverted();
  // Fast page turn: B/W only, pushed with the single-phase DU waveform. Text pages
  // skip the two gray-plane passes and the AA waveform; image pages keep their own
  // grayscale path.
  const bool fastPageTurn = SETTINGS.fastPageTurn != 0 && !pageHasImages;
  // DU drives far less charge than GL16; at the default 15-page cadence ghosting
  // is obvious. Cap the interval at 5 pages while fast turn is on.
  if (fastPageTurn && pagesUntilFullRefresh > 5) pagesUntilFullRefresh = 5;
  // Modal chrome (action chips, annotation card, footnote card) is painted as
  // plain BW. On a 16-level page a white plate only writes the base plane, so a
  // card drawn over grayscale text came out as a black slab with invisible text.
  const bool hasModalChrome = selectionMenuActive || annotationCardActive || footnoteCardActive;
  const bool needsTextGrayscale =
      !hasModalChrome && grayscaleEnabled && SETTINGS.textAntiAliasing && !fastPageTurn;
  // Images write the BW buffer directly; keep them, backgrounds and faux bold on the existing path.
  // Synthetic bold no longer rules 16-level text out: the glyph renderer applies
  // the bold offset to the same 4-bit coverage the 16-level path consumes, so a
  // reader no longer has to choose between bold and 16 gray levels.
  const bool use16LevelText = needsTextGrayscale && !pageHasImages && renderer.getGrayscaleLevels() == 16 &&
                              sdFontSystem.readerFaceIsFourBit() && !SETTINGS.readingBackgroundEnabled;
#if FREEINK_DEVICE_EEGO_A4
  // A4 single-refresh design: displayGrayBuffer() replaces the B/W base on the
  // panel, so whatever the gray pass draws IS the final frame. With text AA
  // off, the gray pass would render only the page's images (see
  // renderGrayscalePass) and wipe the text the base frame just showed —
  // reported as "image pages show only the image, no text". When there is no
  // AA to render, skip the grayscale pipeline entirely: image pages fall back
  // to the plain B/W frame, which keeps text and image together.
  const bool needsAnyGrayscale = grayscaleEnabled && SETTINGS.textAntiAliasing;
#else
  // needsTextGrayscale already excludes fast page turns; images keep their own path.
  const bool needsAnyGrayscale = grayscaleEnabled && (needsTextGrayscale || pageHasImages);
#endif
  const bool absoluteImageGrayscale = grayscaleEnabled && pageHasImages && !gpio.deviceIsX3() &&
                                      display.getController() == HalDisplay::Controller::UC8279 &&
                                      renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  const auto grayscale = renderer.grayscaleCapabilities(absoluteImageGrayscale ? HalDisplay::GrayscaleMode::Absolute
                                                                               : HalDisplay::GrayscaleMode::Overlay);
  const bool tiledGrayscale = needsAnyGrayscale && grayscale.stripUploads;
  // Combined text AA: defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase =
#if FREEINK_DEVICE_READPICO
      // Read Pico has no strip uploads (whole-plane grayscale) but its panel CAN join
      // the base and the grey planes into a single waveform, so it must not be gated
      // behind tiledGrayscale. This is what removes the extra full panel refresh per
      // anti-aliased page: the base is stashed and displayGrayBuffer() presents both.
      //
      // Image pages are included here, unlike the shared branch below. Its
      // !pageHasImages exists because a strip-upload driver has to re-send the
      // affected strip to build the base; this whole-plane path stashes the entire
      // framebuffer, illustration and all, so displayGrayBuffer() composes the page
      // from it either way and the panel is still driven exactly once.
      needsAnyGrayscale && !SETTINGS.readingBackgroundEnabled && renderer.supportsTextOnlyCombinedBase();
#else
      tiledGrayscale && !pageHasImages && !SETTINGS.readingBackgroundEnabled && renderer.supportsTextOnlyCombinedBase();
#endif
#if FREEINK_DEVICE_EEGO_A4
  const bool overlapRefresh =
      tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages && !needsTextGrayscale;
#elif FREEINK_DEVICE_READPICO
  // Read Pico 没有 strip upload（tiledGrayscale 为假），但 epdiy 驱动在
  // displayStart() 返回前就把帧缓冲拷进自己的 4bpp 差分缓冲，扫描期间宿主可以
  // 立刻重画。所以普通翻页都能把 ~415ms 的扫描与下一页的排版重叠起来。
  // / Read Pico has no strip uploads (tiledGrayscale is false), but the epdiy
  // driver copies the frame into its own 4 bpp difference buffer before
  // displayStart() returns, so the host may redraw while the panel scans. Every
  // plain page turn therefore overlaps the ~415 ms scan with the next layout.
  const bool overlapRefresh = renderer.supportsAsyncRefresh() && !pageHasImages;
#else
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
#endif
  const auto drawGuideLines = [&] {
    if (!SETTINGS.readingGuideLineEnabled) return;
    const int x1 = orientedMarginLeft;
    const int x2 = renderer.getScreenWidth() - orientedMarginRight - 1;
    const int contentBottom = renderer.getScreenHeight() - orientedMarginBottom;
    const int baseLineHeight = renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression());
    const int ascender = renderer.getFontAscenderSize(fontId);
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      if (line.getBlock()->isEmpty()) continue;
      const int lineHeight = baseLineHeight + line.getBlock()->getRubyShift(ascender);
      const int guideY = orientedMarginTop + line.yPos + lineHeight + SETTINGS.readingGuideLineOffset;
      if (readingGuideLine::fitsVertically(SETTINGS.readingGuideLineStyle, guideY, orientedMarginTop, contentBottom)) {
        readingGuideLine::draw(renderer, x1, guideY, x2, SETTINGS.readingGuideLineStyle);
      }
    }
  };
  const auto renderPageWithGuideLines = [&] {
    renderPage();
    drawGuideLines();
#ifdef BOARD_HAS_PSRAM
    // After the text and its guide lines: the bubble sits on top of both. This
    // runs in every pass (base, LSB, MSB, 16-level) so the bubble reaches the
    // framebuffer that pass will push.
    drawCommentBubbles();
    // Same rule for the placeholder action: it must reach every pass too.
    drawJjwxcPlaceholderAction(fontId, orientedMarginBottom);
    drawJjwxcMoreChapters(orientedMarginBottom);
#endif
    // Footnote card rides the same rule as the bubbles: it must reach whichever
    // framebuffer the pass will push (base, LSB, MSB, 16-level).
    if (footnoteCardActive) drawFootnoteCard();
    // Annotations ride the same pass: underline + note bubble land in whichever
    // framebuffer this pass pushes (base, LSB, MSB, 16-level).
    if (!annotations_.empty()) drawAnnotations(*page, fontId, orientedMarginLeft, orientedMarginTop);
    if (selectionActive) drawSelection(*page, fontId, orientedMarginLeft, orientedMarginTop);
    if (selectionMenuActive) drawSelectionMenu();
    // The card was never called from any render path, which is why tapping a
    // bubble inverted the word but showed no popup at all.
    if (annotationCardActive) drawAnnotationCard();
    if (fontStepHintPointSize != 0 && static_cast<int32_t>(millis() - fontStepHintUntilMs) < 0) {
      // White plate + black frame + black text: a bare label collided with the
      // first line of body text at some densities.
      char label[24];
      snprintf(label, sizeof(label), "%u pt", static_cast<unsigned>(fontStepHintPointSize));
      constexpr int kPadX = 18;
      constexpr int kPadY = 8;
      const int textW = renderer.getTextAdvanceX(fontId, label, EpdFontFamily::BOLD);
      const int hintW = textW + kPadX * 2;
      const int hintH = renderer.getLineHeight(fontId) + kPadY * 2;
      const int hintX = std::max<int>(orientedMarginLeft, (renderer.getScreenWidth() - hintW) / 2);
      const int hintY = std::max(2, orientedMarginTop - hintH - 2);
      renderer.fillRect(hintX, hintY, hintW, hintH, false);
      renderer.drawRect(hintX, hintY, hintW, hintH, 2, true);
      renderer.drawText(fontId, hintX + kPadX, hintY + kPadY, label, true, EpdFontFamily::BOLD);
    }
  };
  auto renderGrayscalePass = [&]() {
    if (absoluteImageGrayscale || needsTextGrayscale) {
      renderPageWithGuideLines();
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    if (absoluteImageGrayscale) renderStatusBar();
#if FREEINK_DEVICE_EEGO_A4
    // A4: include the status bar in the gray pass so the final anti-aliased
    // frame keeps the bottom UI. Without this the gray pass only re-renders
    // the body and wipes the status bar that the earlier BW frame drew.
    // (Other devices keep the upstream gray-pass contents.)
    renderStatusBar();
#endif
  };

  unsigned long cacheBaseMs = 0;
  // A 16-level page clears the frame in beginGrayscale16() and renders the whole
  // page again; the base pass below would be discarded, so skip it. The cost is
  // that the missing-codepoint prompt is detected one page later.
  // / 16 级会清屏后整页重渲，下面这遍会被丢弃，跳过。代价是缺字提示延后一页。
  if (!use16LevelText) {
    if (SETTINGS.readingBackgroundEnabled && !readingBackground::load(renderer)) renderer.clearScreen();
#if FREEINK_DEVICE_READPICO
    if (pageCacheHit) {
      const auto tBase = millis();
      // The cache is the finished page, cleared margins and all, so no clearScreen
      // is needed on this path.
      memcpy(renderer.getFrameBuffer(), pageCacheBase_[pageCacheLiveSlot_].get(), renderer.getBufferSize());
      cacheBaseMs = millis() - tBase;
#ifdef BOARD_HAS_PSRAM
      // The cached pixels are the text page; the action button is chrome and is
      // intentionally not part of the cache, so paint it on this path too.
      drawJjwxcPlaceholderAction(fontId, orientedMarginBottom);
      drawJjwxcMoreChapters(orientedMarginBottom);
#endif
    } else
#endif
    {
      renderPageWithGuideLines();
    }
#ifdef ENABLE_CHINESE_VERSION
#if FREEINK_DEVICE_READPICO
    const uint32_t missingCodepoint =
        pageCacheHit ? pageCacheMissingCodepoint_[pageCacheLiveSlot_] : fcm->consumeMissingChineseCodepoint();
#else
    const uint32_t missingCodepoint = fcm->consumeMissingChineseCodepoint();
#endif
    if (missingCodepoint != 0 && !FontDownloadActivity::wasChineseFontPromptShownThisBoot()) {
      uint32_t expected = 0;
      pendingMissingChineseCodepoint_.compare_exchange_strong(expected, missingCodepoint, std::memory_order_relaxed);
    }
#endif
    renderStatusBar();
    drawBookmarkCorner(orientedMarginTop, orientedMarginRight);
    if (gestureHintVisible) drawGestureHint();
  }
  const auto tBwRender = millis();

  // 水波纹翻页只对顺序文字页生效：图片页、绝对灰阶图片页与清屏周期保持现有路径。
  // 请求交给显示层后由下一次推帧消费一次（pushFrame 内部还有 mode==Full 的第二道
  // 闸门），这里消费后立即清零，重排/换字体等非顺序渲染不会误触发动画。
  // / Water-ripple turns are for sequential text pages only: image pages, absolute
  // grayscale image pages and due cleans keep the existing path. The display layer
  // consumes the request on its next frame push (pushFrame gates on mode==Full as a
  // second check); clearing it here keeps reflows and font changes unanimated.
  const bool waterTurnOk = waterTurnDirection != 0 && !absoluteImageGrayscale && !pageHasImages &&
                           !cleanImageBasePending;
  renderer.setPageTurnDirection(waterTurnOk ? waterTurnDirection : 0);
  waterTurnDirection = 0;

  if (absoluteImageGrayscale) {
    const auto baseMode = cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
    if (!renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute, baseMode)) {
      LOG_ERR("ERS", "Could not start absolute image page; displaying B/W");
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
      return;
    }
    LOG_DBG("ERS", "UC8279 image page: absolute quality waveform");
    pagesUntilFullRefresh = 1;
  } else if (use16LevelText) {
    // 16 级文字：下面那一趟 commitGrayscale16() 自己把整帧推给面板，所以这里**不**推底图。
    // 多推一次会变成两次面板驱动（更闪、更慢），而 16 级用的本身就是整屏波形，残影照清。
    // / 16-level text: the single commitGrayscale16() below drives the panel with the whole
    // frame, so the base is deliberately NOT pushed here. Pushing it would drive the panel
    // twice (more flash, more time), and the 16-level waveform is a full-screen one anyway,
    // so ghosting is still cleared.
  } else if (combinedGrayscaleBase) {
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh, manualRefreshPending);
  } else if (pageHasImages) {
    // Image pages use one base refresh before the grayscale pass. FAST leaves
    // the panel receptive to the gray waveform; pending cleanup still honors
    // the scheduled/manual HALF refresh.
    if (renderer.supportsContinuousImageReading()) {
      renderer.displayBuffer(ReaderUtils::consumeRefreshMode(pagesUntilFullRefresh),
                             DisplayRefreshContext::ImageReading);
    } else {
      renderer.displayBuffer(cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH,
                             DisplayRefreshContext::ImageReading);
      pagesUntilFullRefresh = 1;
    }
  } else {
#if FREEINK_DEVICE_EEGO_A4
    if (needsTextGrayscale) {
      const auto mode = ReaderUtils::consumeRefreshMode(pagesUntilFullRefresh);
      if (mode == HalDisplay::HALF_REFRESH) renderer.displayGrayscaleBase(mode);
    } else {
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
    }
#else
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
#endif
  }
  const auto tDisplay = millis();

  if (use16LevelText) {
    // Consume the same refresh cadence as every other path: a due (or manual)
    // clean goes out as GC16, ordinary pages as GL16. The old code reset the
    // counter to 1 after every page, so periodic and manual cleans never ran.
    const auto grayMode = ReaderUtils::consumeRefreshMode(pagesUntilFullRefresh);
    const bool clean = grayMode == HalDisplay::FULL_REFRESH;
    // Native begin clears the frame, so render the complete page again.
    if (renderer.beginGrayscale16()) {
      renderPageWithGuideLines();
      renderStatusBar();
      drawBookmarkCorner(orientedMarginTop, orientedMarginRight);
      if (gestureHintVisible) drawGestureHint();
      if (activityManager.isSwitchPending()) {
        renderer.cancelGrayscale16();
        return;
      }
      if (renderer.commitGrayscale16(clean)) {
        const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: 16-level text total=%lums clean=%d", tEnd - t0, clean ? 1 : 0);
        // consumeRefreshMode() already advanced/reset the cadence.
        return;
      }
      LOG_ERR("ERS", "16-level text commit failed; displaying B/W");
    } else {
      LOG_ERR("ERS", "Could not start the 16-level text pass; displaying B/W");
    }
    renderer.cancelGrayscale16();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.clearScreen();
    renderPageWithGuideLines();
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    pagesUntilFullRefresh = 1;
    return;
  } else if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;
    const auto abandonGrayscale = [&] {
      renderer.setRenderMode(GfxRenderer::BW);
      if (combinedGrayscaleBase)
        renderer.cancelGrayscale();
      else
        renderer.cleanupGrayscaleWithFrameBuffer();
    };

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return memory::hasAllocationHeadroom(ESP.getFreeHeap(), ESP.getMaxAllocHeap(), planeBytes, planeBytes,
                                           PLANE_BUF_HEADROOM, PLANE_BUF_MAX_ALLOC_RESERVE);
    };
    const auto allocatePlane = [&] {
      if (planeBufFits()) {
        auto buffer = memory::makeInternalByteBufferNoThrow(planeBytes);
        if (buffer) return buffer;
      }
      if (memory::psramHasHeadroom(planeBytes, planeBytes, PLANE_BUF_MAX_ALLOC_RESERVE)) {
        auto buffer = memory::makePsramByteBufferNoThrow(planeBytes);
        if (buffer) return buffer;
      }
      return memory::ByteBuffer{};
    };
    auto lsbPlaneBuf = overlapRefresh ? allocatePlane() : memory::ByteBuffer{};
    auto msbPlaneBuf = lsbPlaneBuf ? allocatePlane() : memory::ByteBuffer{};

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      // Abort before expensive grayscale display if a push/pop is pending
      if (activityManager.isSwitchPending()) {
        abandonGrayscale();
        return;
      }

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          if (combinedGrayscaleBase && activityManager.isSwitchPending()) {
            abandonGrayscale();
          } else {
            renderer.cleanupGrayscaleWithFrameBuffer();
          }
        }
      } else {
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          if (activityManager.isSwitchPending()) {
            abandonGrayscale();
            return;
          }
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          if (activityManager.isSwitchPending()) {
            abandonGrayscale();
            return;
          }
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        if (combinedGrayscaleBase && activityManager.isSwitchPending()) {
          abandonGrayscale();
          return;
        }
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    if (needsAnyGrayscale) {
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        if (absoluteImageGrayscale) renderer.setRenderMode(GfxRenderer::BW);
        return;
      }
      const auto tBwStore = millis();

#if FREEINK_DEVICE_READPICO
      if (pageCacheHit) {
        // Both planes were rendered at idle; putting them in place is one copy
        // each instead of a full page render each.
        memcpy(renderer.getFrameBuffer(), pageCacheLsb_[pageCacheLiveSlot_].get(), renderer.getBufferSize());
        renderer.copyGrayscaleLsbBuffers();
      } else
#endif
      {
        renderer.clearScreen(absoluteImageGrayscale ? 0xFF : 0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        renderGrayscalePass();
        renderer.copyGrayscaleLsbBuffers();
      }
      const auto tGrayLsb = millis();

      // Abort early if a push/pop is pending (e.g. user opened menu)
      if (activityManager.isSwitchPending()) {
        renderer.setRenderMode(GfxRenderer::BW);
        // A combined base was only stashed, so it has to be flushed, not restored.
        if (combinedGrayscaleBase)
          renderer.cancelGrayscale();
        else
          renderer.restoreBwBuffer();
        return;
      }

#if FREEINK_DEVICE_READPICO
      if (pageCacheHit) {
        memcpy(renderer.getFrameBuffer(), pageCacheMsb_[pageCacheLiveSlot_].get(), renderer.getBufferSize());
        renderer.copyGrayscaleMsbBuffers();
      } else
#endif
      {
        renderer.clearScreen(absoluteImageGrayscale ? 0xFF : 0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        renderGrayscalePass();
        renderer.copyGrayscaleMsbBuffers();
      }
      const auto tGrayMsb = millis();

      // Abort before the expensive grayscale display if a push/pop is pending
      if (activityManager.isSwitchPending()) {
        renderer.setRenderMode(GfxRenderer::BW);
        // A combined base was only stashed, so it has to be flushed, not restored.
        if (combinedGrayscaleBase)
          renderer.cancelGrayscale();
        else
          renderer.restoreBwBuffer();
        return;
      }
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      if (pageCacheHit) {
        LOG_DBG("ERS", "Page render: page-cache hit (base_copy=%lums)", cacheBaseMs);
      }
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
#if FREEINK_DEVICE_READPICO
  prebuildForwardSlot();
#endif
}

EpubReaderActivity::ReaderChromeMetrics EpubReaderActivity::readerChromeMetrics() const {
  ReaderChromeMetrics m;
  int viewTop = 0, viewRight = 0, viewBottom = 0, viewLeft = 0;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  m.left = viewLeft + SETTINGS.screenMargin;
  m.right = screenW - viewRight - SETTINGS.screenMargin;
  m.top = viewTop + std::max(6, SETTINGS.screenMargin / 2);
  m.lineH = renderer.getLineHeight(kChromeTextFont);
  m.titleY = m.top + 2;
  m.metaY = m.titleY + m.lineH + 2;
  m.ruleY = m.metaY + m.lineH + 8;
  m.headerBottom = m.ruleY + 1;
  m.pageH = renderer.getLineHeight(kChromeTextFont);
  m.pageY = screenH - viewBottom - 8 - m.pageH;
  m.waveH = kChromeWaveHeight;
  m.waveY = m.pageY - kChromeWaveToPages - m.waveH;
  m.footerTop = m.waveY - kChromeFooterGap;
  m.footerHeight = screenH - m.footerTop;
  return m;
}

// Top line: book/chapter title on the left, clock + battery on the right.
// Second line: the real reading position (book percent and the live font
// size). Both obey the existing status-bar settings, so a user who hid the
// title or the clock keeps them hidden.
void EpubReaderActivity::drawReaderHeader() const {
  const auto m = readerChromeMetrics();
  const auto sb = SETTINGS.statusBarSpec();
  const int screenW = renderer.getScreenWidth();
  const int headerRight = std::min(m.right, screenW - kChromeRibbonReserve);
  int clusterRight = headerRight;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (sb.showBattery) {
    const int batteryW = metrics.batteryWidth;
    GUI.drawStatusBarBattery(renderer, clusterRight - batteryW, m.titleY, sb.showBatteryPercent);
    clusterRight -= batteryW + 10;
  }
  if (sb.showsClock()) {
    char timeBuf[9];
    if (!TimeUtils::formatCurrentTime(timeBuf, sizeof(timeBuf), sb.clock12h)) {
      snprintf(timeBuf, sizeof(timeBuf), "--:--");
    }
    const int clockW = renderer.getTextWidth(kChromeNumericFont, timeBuf);
    const int clockY = m.titleY + renderer.getFontAscenderSize(kChromeTextFont) -
                       renderer.getFontAscenderSize(kChromeNumericFont);
    renderer.drawText(kChromeNumericFont, clusterRight - clockW, clockY, timeBuf, true);
    clusterRight -= clockW + 10;
  }

  if (sb.showsTitle() && epub) {
    std::string title;
    if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
      title = epub->getTitle();
    } else {
      const std::string chapter = currentChapterTitle();
      const std::string book = epub->getTitle();
      title = (book.empty() || book == chapter) ? chapter : book + " · " + chapter;
    }
    const int available = clusterRight - m.left - 12;
    if (available > 0) {
      const std::string shown = renderer.truncatedText(kChromeTextFont, title.c_str(), available);
      renderer.drawText(kChromeTextFont, m.left, m.titleY, shown.c_str(), true);
    }
  }

  // Reading position. The percent is real book progress; the font size is the
  // live setting, so the line always has something true to say.
  std::string meta;
  if (automaticPageTurnActive) {
    meta = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
  } else {
    if (sb.showBookProgressPercent && epub && section) {
      const int pageCount = section->estimatedTotalPages();
      const float chapterProgress =
          pageCount > 0 ? static_cast<float>(section->currentPage + 1) / static_cast<float>(pageCount) : 0.0f;
      const int percent = clampPercent(static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) *
                                                           100.0f + 0.5f));
      meta = std::to_string(percent) + "%";
    }
    const std::string fontPart = std::string(tr(STR_FONT_SIZE)) + " " + std::to_string(SETTINGS.fontPointSize);
    meta = meta.empty() ? fontPart : meta + " · " + fontPart;
  }
  const int metaAvailable = m.right - m.left;
  if (metaAvailable > 0) {
    const std::string shown = renderer.truncatedText(kChromeTextFont, meta.c_str(), metaAvailable);
    renderer.drawText(kChromeTextFont, m.left, m.metaY, shown.c_str(), true);
  }

  const int ruleW = m.right - m.left;
  if (ruleW > 0) renderer.fillRect(m.left, m.ruleY, ruleW, 1, true);
}

// Foot: one full-width shallow wave whose filled part is the read portion,
// with the chapter/page counters on the line below it.
void EpubReaderActivity::drawReaderFooter() const {
  const auto m = readerChromeMetrics();
  const auto sb = SETTINGS.statusBarSpec();
  const int width = m.right - m.left;
  if (width <= 0) return;

  int percent = 0;
  if (epub && section) {
    const int pageCount = section->estimatedTotalPages();
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(section->currentPage + 1) / static_cast<float>(pageCount) : 0.0f;
    percent =
        clampPercent(static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f));
  }
  const int fillW = width * percent / 100;
  // Filled read portion first, then the wave rides on top of it so the line
  // stays crisp instead of dissolving into the dither.
  if (fillW > 0) {
    renderer.fillRectDither(m.left, m.waveY + 4, fillW, m.waveH - 4, Color::LightGray);
  }
  constexpr int kAmplitude = 3;
  constexpr int kPeriod = 28;
  int prevX = m.left;
  int prevY = m.waveY + kAmplitude;
  for (int x = m.left + 2; x <= m.right; x += 2) {
    const double phase = static_cast<double>(x - m.left) * 2.0 * 3.141592653589793 / kPeriod;
    const int y = m.waveY + kAmplitude + static_cast<int>(std::lround(std::sin(phase) * kAmplitude));
    renderer.drawLine(prevX, prevY, x, y, 2, true);
    prevX = x;
    prevY = y;
  }
  if (fillW > 0) {
    const int knob = 10;
    const int knobX = std::min(m.right - knob, m.left + fillW - knob / 2);
    renderer.fillRoundedRect(knobX, m.waveY + kAmplitude - knob / 2, knob, knob, knob / 2, Color::Black);
  }

  if (!sb.showChapterPageCount) return;
  const int pageCount = section ? section->estimatedTotalPages() : 1;
  const int page = section ? section->currentPage + 1 : 1;
  char pageText[32];
  snprintf(pageText, sizeof(pageText), "%d/%d", page, pageCount > 0 ? pageCount : 1);
  const int pageW = renderer.getTextWidth(kChromeNumericFont, pageText);
  renderer.drawText(kChromeNumericFont, m.right - pageW, m.pageY, pageText, true);

  const int tocIndex = currentTocIndex();
  if (tocIndex >= 0 && epub) {
    char chapterText[48];
    snprintf(chapterText, sizeof(chapterText), "%s %d/%d", tr(STR_CHAPTER), tocIndex + 1,
             epub->getTocItemsCount());
    const int available = m.right - m.left - pageW - 12;
    if (available > 0) {
      const std::string shown = renderer.truncatedText(kChromeTextFont, chapterText, available);
      renderer.drawText(kChromeTextFont, m.left, m.pageY, shown.c_str(), true);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  drawReaderHeader();
  drawReaderFooter();
#if FREEINK_DEVICE_READPICO
  // Remember which minute this paint shows, so the per-minute tick (A2) fires on
  // the minute boundary instead of immediately after every render.
  const uint32_t clockNow = TimeUtils::getCurrentValidTimestamp();
  lastClockMinute_ = TimeUtils::isClockValid(clockNow) ? static_cast<int64_t>(clockNow) / 60 : -1;
#endif
}

#if FREEINK_DEVICE_READPICO
void EpubReaderActivity::refreshStatusBarClockIfDue() {
  if (!section || !epub) return;
  if (!SETTINGS.statusBarSpec().showsClock()) return;
  if (renderer.isInverted()) return;
  // A reading background paints under the bar; clearing the strip would erase it.
  if (SETTINGS.readingBackgroundEnabled) return;
  if (section->isBuilding()) return;
  if (overlay != Overlay::None || fontPreview.active()) return;
  if (automaticPageTurnActive || showBookmarkMessage || showDictionaryMessage) return;
  if (loadFailurePopup.isActive() || endOfBookMenuActive()) return;
  if (activityManager.isSwitchPending()) return;
  // Only tick a page the panel already shows: the framebuffer must match the
  // differential baseline, or a windowed diff would compare against stale pixels.
  if (lastRenderCompleteMs == 0 || millis() - lastRenderCompleteMs < 900) return;
  if (currentSpineIndex != renderedPageKey_.spine || section->currentPage != renderedPageKey_.page) return;

  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (!TimeUtils::isClockValid(now)) return;
  const int64_t minute = static_cast<int64_t>(now) / 60;
  if (minute == lastClockMinute_) return;

  // Opportunistic: never block the render task; retry on a later loop tick.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  // Input may have moved the page between the test above and taking the lock.
  if (activityManager.isSwitchPending() || overlay != Overlay::None || section->isBuilding() ||
      currentSpineIndex != renderedPageKey_.spine || section->currentPage != renderedPageKey_.page) {
    return;
  }

  const size_t bufferSize = renderer.getBufferSize();
  if (!clockScratch_) {
    // The panel geometry is fixed for the life of the device, so one allocation
    // covers every tick.
    clockScratch_ = memory::makePsramByteBufferNoThrow(bufferSize);
    if (!clockScratch_) return;
  }
  uint8_t* fb = renderer.getFrameBuffer();
  memcpy(clockScratch_.get(), fb, bufferSize);

  HalPowerManager::Lock powerLock;
  renderer.setRenderMode(GfxRenderer::BW);
  // The clock lives in the top header band now. Repaint that band and the
  // bookmark ribbon (the ribbon hangs inside it) and leave the body alone:
  // the page layout reserved the band, so no glyph can sit there.
  const auto chrome = readerChromeMetrics();
  const int clearBottom = std::min(renderer.getScreenHeight(), chrome.headerBottom + 2);
  renderer.fillRect(0, 0, renderer.getScreenWidth(), clearBottom, false);
  drawReaderHeader();
  drawBookmarkCorner(0, 0);
  lastClockMinute_ = minute;

  // The clock paint is the only change, so the bounding box of the differences
  // stays inside the status strip and never reaches the body. Report it in
  // physical panel coords (framebuffer row bytes) for displayWindow().
  const auto region = readerutil::changedRegion(clockScratch_.get(), fb, renderer.getDisplayHeight(),
                                                renderer.getDisplayWidthBytes());
  if (!region.valid) return;  // nothing actually changed

  // ContinuousReading: the clock tick belongs to the reader's own cadence, not
  // the system-UI GC16 sweep. Without this an idle read flashed black/white
  // every Nth minute (N = the refresh interval) with no user action -- the most
  // jarring possible moment. The reader's page-turn cadence still sweeps.
  renderer.displayWindow(region.x, region.y, region.width, region.height,
                         DisplayRefreshContext::ContinuousReading);
  LOG_DBG("ERS", "Clock tick: minute=%lld window=%d,%d %dx%d", static_cast<long long>(minute), region.x, region.y,
          region.width, region.height);
}
#endif

// ---------------------------------------------------------------------------
// Toolbar reader menu
// ---------------------------------------------------------------------------

namespace {
// refreshed UI frame 05: the type panel is a strict 2x4 grid. Every cell maps to a
// setting that already exists; "paragraph spacing" is the reader's extra
// paragraph spacing, and "page margin" is screenMargin.
constexpr StrId kTextRowNames[] = {StrId::STR_FONT,          StrId::STR_FONT_SIZE,      StrId::STR_LINE_SPACING,
                                   StrId::STR_EXTRA_SPACING_SHORT, StrId::STR_SCREEN_MARGIN,  StrId::STR_FAST_PAGE_TURN,
                                   StrId::STR_TURN_EFFECT_WATER, StrId::STR_DEFAULT_VALUE};
constexpr StrId kSpacingIds[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
constexpr StrId kExtraSpacingIds[] = {StrId::STR_EXTRA_SPACING_OFF,  StrId::STR_EXTRA_SPACING_0_5,
                                      StrId::STR_EXTRA_SPACING_0_75, StrId::STR_EXTRA_SPACING_1,
                                      StrId::STR_EXTRA_SPACING_1_25, StrId::STR_EXTRA_SPACING_1_5};
constexpr int kTextRowCount = static_cast<int>(std::size(kTextRowNames));
static_assert(std::size(kSpacingIds) == CrossPointSettings::LINE_COMPRESSION_COUNT, "line spacing labels");
static_assert(std::size(kExtraSpacingIds) == 6, "extra paragraph spacing labels");
static_assert(kTextRowCount == 8, "text panel is a 2x4 grid");
}  // namespace

bool EpubReaderActivity::usesToolbarMenu() const {
  // Both board classes drive the same chrome: touch through the FreeInkUI tap
  // targets, buttons through the focused-tool pill and the panel cursor.
  return SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

int EpubReaderActivity::currentTocIndex() const {
  if (!epub) return -1;
  const auto offset = section ? currentPageVisibleOffset : cachedVisibleTextOffset;
  return getChapterTocIndex(*epub, currentSpineIndex, offset);
}

std::string EpubReaderActivity::currentChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = currentTocIndex();
  if (tocIndex != -1) {
    return epub->getTocItem(tocIndex).title;
  }
  return tr(STR_UNNAMED);
}

std::string EpubReaderActivity::textRowName(int row) const {
  return row >= 0 && row < kTextRowCount ? I18N.get(kTextRowNames[row]) : "";
}

std::string EpubReaderActivity::textRowValue(int row) const {
  static constexpr StrId kFamily[] = {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS};
  const auto onOff = [](const bool on) { return std::string(I18N.get(on ? StrId::STR_YES : StrId::STR_NO)); };
  switch (row) {
    case 0:
      if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
      return I18N.get(kFamily[SETTINGS.fontFamily % CrossPointSettings::FONT_FAMILY_COUNT]);
    // Value rows keep the inline stepper affordance the panel strips for its
    // own minus/plus buttons (see stepValueText() in ReaderToolbarUi).
    case 1:
      return "- " + std::to_string(SETTINGS.fontPointSize) + " pt ＋";
    case 2:
      return std::string("- ") +
             I18N.get(kSpacingIds[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]) + " +";
    case 3:
      return std::string("- ") +
             I18N.get(kExtraSpacingIds[std::min<uint8_t>(SETTINGS.extraParagraphSpacing,
                                                         static_cast<uint8_t>(std::size(kExtraSpacingIds) - 1))]) +
             " +";
    case 4:
      return "- " + std::to_string(SETTINGS.screenMargin) + " ＋";
    case 5:
      return onOff(SETTINGS.fastPageTurn != 0);
    case 6:
      return onOff(SETTINGS.pageTurnEffect != 0);
    default:
      return "";
  }
}

// Live apply: persist, re-paginate, and let renderBook() redraw the page with
// the open panel back on top -- the book itself is the preview.
void EpubReaderActivity::applyTextSettingLive() {
  applyReaderTextSettings();
  discardOverlayPage();  // the stored page is laid out with the old settings
  requestUpdate();
}

// D5: the panel's inline steppers. Font size walks the shipped sizes (the
// same list the picker shows); line spacing / paragraph spacing / page margin
// step through their ranges. Each step applies live -- no dialog, the page
// behind the sheet is the preview.
void EpubReaderActivity::stepTextRow(const int row, const int delta) {
  if (delta == 0) return;
  switch (row) {
    case 1: {
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      if (sizes.size() < 2) return;
      const uint8_t current = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      size_t index = 0;
      while (index < sizes.size() && sizes[index] != current) ++index;
      if (index >= sizes.size()) index = 0;
      const int next = std::clamp(static_cast<int>(index) + delta, 0, static_cast<int>(sizes.size()) - 1);
      if (sizes[static_cast<size_t>(next)] == SETTINGS.fontPointSize) return;
      SETTINGS.fontPointSize = sizes[static_cast<size_t>(next)];
      applyTextSettingLive();
      return;
    }
    case 2: {
      constexpr int wrap = CrossPointSettings::LINE_COMPRESSION_COUNT;
      const uint8_t next = static_cast<uint8_t>((SETTINGS.lineSpacing + delta % wrap + wrap) % wrap);
      if (next == SETTINGS.lineSpacing) return;
      SETTINGS.lineSpacing = next;
      applyTextSettingLive();
      return;
    }
    case 3: {
      const int wrap = static_cast<int>(std::size(kExtraSpacingIds));
      const int next = std::clamp(static_cast<int>(SETTINGS.extraParagraphSpacing) + delta, 0, wrap - 1);
      if (next == SETTINGS.extraParagraphSpacing) return;
      SETTINGS.extraParagraphSpacing = static_cast<uint8_t>(next);
      applyTextSettingLive();
      return;
    }
    case 4: {
      constexpr int kMin = CrossPointSettings::SCREEN_MARGIN_MIN;
      constexpr int kMax = CrossPointSettings::SCREEN_MARGIN_MAX;
      constexpr int kStep = CrossPointSettings::SCREEN_MARGIN_STEP;
      const int next = std::clamp(static_cast<int>(SETTINGS.screenMargin) + delta * kStep, kMin, kMax);
      if (next == SETTINGS.screenMargin) return;
      SETTINGS.screenMargin = static_cast<uint8_t>(next);
      applyTextSettingLive();
      return;
    }
    default:
      return;
  }
}

// Switch cells of the type grid (fast page turn / water ripple). Toggles do
// not re-paginate: both change the push path, not the text layout.
void EpubReaderActivity::toggleReaderSwitch(const int cell) {
  if (cell == 5) {
    SETTINGS.fastPageTurn = SETTINGS.fastPageTurn ? 0 : 1;
  } else if (cell == 6) {
    SETTINGS.pageTurnEffect = SETTINGS.pageTurnEffect ? 0 : 1;
  } else {
    return;
  }
  SETTINGS.saveToFile();
  discardOverlayPage();  // the snapshot was built with the old effect
}

// Cell 7: every value the grid exposes goes back to the shipped default. The
// font family returns to the built-in face; the full text-settings screen still
// owns alignment / indent / vertical text, which the grid does not surface.
void EpubReaderActivity::restoreReaderDefaults() {
  SETTINGS.clearSdFontFamily();
  SETTINGS.fontPointSize = CrossPointSettings::DEFAULT_FONT_POINT_SIZE;
  SETTINGS.lineSpacing = CrossPointSettings::NORMAL;
  SETTINGS.paragraphAlignment = CrossPointSettings::JUSTIFIED;
  SETTINGS.firstLineIndent = 0;
  SETTINGS.extraParagraphSpacing = 0;
  SETTINGS.screenMargin =
      UiHighDpiProfile::enabled ? 25 : CrossPointSettings::SCREEN_MARGIN_MIN;
  SETTINGS.fastPageTurn = 0;
  SETTINGS.pageTurnEffect = 0;
  SETTINGS.saveToFile();
  applyTextSettingLive();
}

// Settings-style option pickers for the Text panel's value rows. Every
// selection applies immediately to the page under the sheet.
void EpubReaderActivity::showTextRowPopup(const int row) {
  switch (row) {
    case 1: {
      // The point sizes the active family actually ships.
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      if (sizes.empty()) return;
      std::vector<std::string> labels;
      labels.reserve(sizes.size());
      for (const uint8_t size : sizes) labels.push_back(std::to_string(size) + " pt");
      const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      int curIdx = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == cur) curIdx = static_cast<int>(i);
      }
      overlayPopup.show(StrId::STR_FONT_SIZE, labels, curIdx, [this, sizes](int idx) {
        if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
        if (SETTINGS.fontPointSize == sizes[idx]) return;
        SETTINGS.fontPointSize = sizes[idx];
        SETTINGS.sdFontFlashPreload = 0;
        applyTextSettingLive();
      });
      break;
    }
    case 2:
      overlayPopup.show(StrId::STR_LINE_SPACING, kSpacingIds, static_cast<int>(std::size(kSpacingIds)),
                        SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT, [this](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case 3:
      overlayPopup.show(StrId::STR_EXTRA_SPACING, kExtraSpacingIds, static_cast<int>(std::size(kExtraSpacingIds)),
                        std::min<uint8_t>(SETTINGS.extraParagraphSpacing,
                                          static_cast<uint8_t>(std::size(kExtraSpacingIds) - 1)),
                        [this](int idx) {
                          SETTINGS.extraParagraphSpacing = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case 4: {
      std::vector<std::string> labels;
      constexpr int kMin = CrossPointSettings::SCREEN_MARGIN_MIN;
      constexpr int kMax = CrossPointSettings::SCREEN_MARGIN_MAX;
      constexpr int kStep = CrossPointSettings::SCREEN_MARGIN_STEP;
      for (int value = kMin; value <= kMax; value += kStep) labels.push_back(std::to_string(value));
      const int current = (std::clamp<int>(SETTINGS.screenMargin, kMin, kMax) - kMin) / kStep;
      overlayPopup.show(StrId::STR_SCREEN_MARGIN, labels, current, [this](int idx) {
        SETTINGS.screenMargin =
            static_cast<uint8_t>(CrossPointSettings::SCREEN_MARGIN_MIN + idx * CrossPointSettings::SCREEN_MARGIN_STEP);
        applyTextSettingLive();
      });
      break;
    }
    default:
      return;
  }
  paintOverlayPopup();
}

void EpubReaderActivity::discardOverlayPage() {
  if (!overlayPageStored) return;
  renderer.discardStoredBwBuffer();
  overlayPageStored = false;
}

// Push freshly painted overlay chrome. Where the panel supports it the refresh
// is fired deferred: the loop keeps polling input while the waveform runs, so
// the chrome answers taps and buttons the moment it is visible instead of only
// after a blocking displayBuffer() returns. Caller must hold the RenderLock.
void EpubReaderActivity::pushOverlayRefresh() {
  if (renderer.supportsAsyncRefresh()) {
    renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
    overlayRefreshPending = true;
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// Wait out a pending deferred overlay refresh and reseed the panel's
// differential baseline from the framebuffer: the shadow-free async path skips
// the post-refresh resync, so without this the next FAST diff would run
// against the frame from before the chrome and leave stale pixels on the
// glass. Caller must hold the RenderLock.
void EpubReaderActivity::settleOverlayRefresh() {
  if (!overlayRefreshPending) return;
  overlayRefreshPending = false;
  renderer.cleanupGrayscaleWithFrameBuffer();  // waits, then reseeds the baseline
}

void EpubReaderActivity::debugOpenPanel(const int tool) {
  focusedTool = tool == 1 ? 0 : tool == 2 ? 1 : 3;
  openOverlay(tool == 1 ? Overlay::Contents : tool == 2 ? Overlay::Text : Overlay::More);
  requestUpdate();
}

void EpubReaderActivity::debugDismissHint() {
  if (!gestureHintVisible) return;
  gestureHintVisible = false;
  SETTINGS.readerGestureHintSeen = 1;
  SETTINGS.saveToFile();
  requestUpdate();
}

void EpubReaderActivity::debugSetContentsFilter(const int filter) {
  contentsFilter_ = std::clamp(filter, 0, 2);
  if (overlay != Overlay::Contents) {
    debugOpenPanel(1);
    return;
  }
  rebuildContentsRows();
  panelIndex = std::max(0, contentsRowForToc(currentTocIndex()));
  if (toolbarUi) {
    toolbarUi->nav().reset(panelIndex);
    toolbarUi->nav().top = panelIndex;
  }
  requestUpdate();
}

void EpubReaderActivity::debugActivateContentsRow(const int row) {
  if (overlay != Overlay::Contents || !epub) return;
  const int tocIndex = contentsTocForRow(row);
  if (tocIndex < 0) return;
  const auto item = epub->getTocItem(tocIndex);
  if (item.spineIndex != -1) {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = item.spineIndex;
    pendingAnchor = item.anchor;
    nextPageNumber = 0;
    section.reset();
  }
  overlay = Overlay::None;
  discardOverlayPage();
  requestUpdate();
}

void EpubReaderActivity::openOverlay(Overlay target) {
  mappedInput.resetHomeButtonInput();
  const Overlay previous = overlay;
  overlay = target;
  if (!toolbarUi) toolbarUi = makeUniqueNoThrow<ReaderToolbarUi>(renderer);
  if (!toolbarUi) {
    LOG_ERR("ERS", "OOM allocating reader toolbar");
    overlay = Overlay::None;
    return;
  }
  if (previous == Overlay::None) toolbarUi->begin();
  // Buttons show a cursor from the start; touch boards only once a button moves it.
  panelCursorShown = !mappedInput.hasTouch();
  switch (target) {
    case Overlay::Toolbar:
      focusedTool = 0;
      break;
    case Overlay::Contents: {
      focusedTool = 0;
      int focus = currentTocIndex();
      rebuildContentsRows();
#ifdef BOARD_HAS_PSRAM
      resolveJjwxcBook();
      if (jjwxcBook_ && jjwxcTocMeta_.empty()) {
        // renderOverlay() reads this vector on the render task; loading it here
        // must not overlap a frame that is still drawing the panel.
        RenderLock lock;
        loadJjwxcTocMeta();
      }
      if (pendingContentsFocus_ >= 0) focus = pendingContentsFocus_;
      pendingContentsFocus_ = -1;
#endif
      const int filteredRow = contentsRowForToc(focus);
      focus = filteredRow >= 0 ? filteredRow : 0;
      panelIndex = std::max(0, focus);
      // Fresh viewport opening on the current chapter, cursor shown or not.
      toolbarUi->nav().reset(panelIndex);
      toolbarUi->nav().top = panelIndex;
      break;
    }
    case Overlay::Text:
      static_assert(sizeof(SETTINGS.sdFontFamilyName) == 32);
      fontPreview.begin(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize, SETTINGS.sdFontFlashPreload != 0);
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::Bookmarks:
      loadCachedBookmarks();
      if (!annotationsLoaded) loadAnnotations();
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::More:
      panelIndex = 0;
      buildMoreActions();
      toolbarUi->nav().reset();
      break;
    default:
      break;
  }
  panelHoldJumped = false;

  // The page is already on screen and still in the framebuffer, so paint the
  // chrome straight onto it and push one refresh. requestUpdate() would
  // re-render the whole page first: slow, and visibly wrong, since that repaint
  // lands before the overlay does.
  if (section) {
    // Serialize against the render task: renderBook may be mid-page (status
    // bar included) in the shared framebuffer, and painting the chrome from
    // the loop task at the same time interleaves the two frames.
    RenderLock lock;
    settleOverlayRefresh();
    if (previous == Overlay::None) {
      // Snapshot the clean page so stepping back from a panel to the toolbar
      // (and closing, where supported) can restore it without a re-render.
      overlayPageStored = renderer.storeBwBuffer();
#if FREEINK_DEVICE_EEGO_A4
      // The page under the chrome is a grayscale AA frame: its gray MSB plane
      // (DTM1) survives in the controller RAM and ghosts through the overlay
      // even after a full waveform (only DTM2 is rewritten). Write the BW page
      // into both planes so the chrome opens over a clean B/W state — the
      // frontlight panel's "refresh and become B/W" handoff. The close path
      // re-renders the AA page to restore the gray look.
      renderer.cleanupGrayscaleWithFrameBuffer();
#endif
    } else if (overlayPageStored) {
      // Overlay -> overlay: wipe the previous chrome (toolbar header, sheet,
      // progress row) back to the clean page so none of it shows around or
      // through the new sheet; re-store for the next transition. No baseline
      // resync: the glass still shows the old chrome, and the differential
      // must keep diffing against it to erase it.
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    pushOverlayRefresh();
  } else {
    requestUpdate();  // no page yet: renderBook() draws the overlay once it is
  }
}

// Close the overlay back to the reading page. Boards without the Xteink
// grayscale-AA pass restore the page snapshot and push one FAST refresh -- no
// re-render, no flash; Xteink boards re-render to restore the AA planes.
void EpubReaderActivity::closeOverlayToPage() {
  mappedInput.resetHomeButtonInput();
  overlay = Overlay::None;
  overlayPopup.dismiss();  // an option picker cannot outlive its panel
  toolbarUi.reset();       // ~1 KB of interaction table + props, only needed while open
  // A panel row toggled in place (image scaling) applies here and only here:
  // the snapshot below holds pixels decoded with the old filter, so drop it to
  // fall through to the re-render path instead of restoring them.
  if (imageScalingDirty) {
    imageScalingDirty = false;
    discardOverlayPage();
  }
#if FREEINK_DEVICE_EEGO_A4
  // The AA page return sits on top of the B/W chrome frame (openOverlay's
  // cleanup wrote both planes); force the reader's next render onto the full
  // waveform so the AA pass comes back clean — the frontlight panel's onExit
  // handoff.
  renderer.requestNextFullRefresh();
#endif
  if (!xteinkClassPanel() && overlayPageStored) {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    // No baseline resync: the glass shows the chrome, and erasing it needs
    // the differential to keep diffing against the last pushed frame.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  discardOverlayPage();
  requestUpdate();  // redraw the clean page
}

void EpubReaderActivity::renderOverlay() {
  if (!epub || !section || !toolbarUi) return;

  ReaderToolbarUi::Model model;
  model.screenWidth = static_cast<int16_t>(renderer.getScreenWidth());
  // The toolbar's tool pill is the button-navigation cursor: tap-first (same
  // convention as the panel lists), it only shows once a button has moved it.
  // Panels override below: there the pill marks the open panel on every board.
  model.activeTool = (overlay == Overlay::Toolbar && !panelCursorShown) ? -1 : focusedTool;
  // Strings the model points at live here until render() returns.
  std::string chapterTitle, pageInfo;

  if (overlay == Overlay::Toolbar) {
    chapterTitle = currentChapterTitle();
    const int pageCount = section->estimatedTotalPages();
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(section->currentPage + 1) / static_cast<float>(pageCount) : 0.0f;
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress);
    pageInfo = std::to_string(section->currentPage + 1) + "/" + std::to_string(pageCount) + "   " +
               std::to_string(clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f))) + "%";
    model.chapterTitle = chapterTitle.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = static_cast<int>(bookProgress * 1000.0f + 0.5f);
    toolbarUi->setModel(model);
    toolbarUi->render();
    toolbarUi->drawContentsFocusFrame();
    return;
  }

  // Panels (Contents / Text / More): a bottom sheet over the page.
  model.panel = true;
  if (!mappedInput.hasTouch()) {
    model.denseRows = true;
  }
  // Tap-first: the cursor is only drawn once a button has moved it, so a
  // tapped row does not stay inverted after its action.
  model.selectedIndex = panelCursorShown ? panelIndex : -1;
  if (overlay == Overlay::Contents) {
    model.panelTitle = tr(STR_TOOL_CONTENTS);
    const bool emptyFiltered = contentsFilter_ != 0 && contentsRows_.empty();
    const int contentsCount = emptyFiltered ? 1 : static_cast<int>(contentsRows_.size());
    model.itemCount = contentsCount;
    model.contentsFilters = contentsFilterable();
    model.contentsFilterIndex = contentsFilter_;
#ifdef BOARD_HAS_PSRAM
    // JJWXC catalogs carry bare titles ("午夜便利店（二）"); the reader's own
    // nav is the only place every chapter is listed, so number the rows to
    // make a 300–1500-entry catalog scannable. Display-only: the EPUB's nav
    // titles stay untouched, so bookmarks and search do not inherit numbers.
    const bool numbered = jjwxcBook_;
#else
    const bool numbered = false;
#endif
    model.rowText = [this, numbered, emptyFiltered](int i) {
      if (emptyFiltered) return std::string(tr(STR_JJWXC_FILTER_EMPTY));
      const int toc = contentsTocForRow(i);
      if (toc < 0) return std::string();
      const auto item = epub->getTocItem(toc);
      const int depth = item.level > 1 ? (item.level - 1) * 2 : 0;
      std::string label = std::string(depth, ' ') + item.title;
      if (numbered) {
        char prefix[12];
        snprintf(prefix, sizeof(prefix), "%d. ", toc + 1);
        label.insert(0, prefix);
      }
      return label;
    };
    // The current chapter is the leading bar now; the old tail "current
    // chapter" text would collide with the state/action chip.
    model.rowValue = [](int) { return std::string(); };
    model.currentChapterIndex = contentsRowForToc(currentTocIndex());
    model.fullRowSelection = true;
#ifdef BOARD_HAS_PSRAM
    if (jjwxcBook_) {
      model.contentsUpdateAction = true;
      model.rowTail = [this](int row) -> ReaderToolbarUi::RowTail {
        const int tocIndex = contentsTocForRow(row);
        if (tocIndex < 0 || static_cast<size_t>(tocIndex) >= jjwxcTocMeta_.size()) return {};
        const auto& meta = jjwxcTocMeta_[static_cast<size_t>(tocIndex)];
        using State = JjwxcStore::ChapterState;
        switch (static_cast<State>(meta.state)) {
          case State::Cached:
            return {meta.isVip ? ReaderToolbarUi::TailBadge::Vip : ReaderToolbarUi::TailBadge::Free,
                    ReaderToolbarUi::TailAction::Refresh};
          case State::Missing:
            // VIP + download means "not cached; acquire". Ownership is unknown
            // here: the app's price page decides cache-vs-purchase. Known
            // unpurchased rows become VIP¥ (the app records that state).
            return {meta.isVip ? ReaderToolbarUi::TailBadge::Vip : ReaderToolbarUi::TailBadge::Free,
                    ReaderToolbarUi::TailAction::Download};
          case State::Unpurchased:
            return {ReaderToolbarUi::TailBadge::VipYen, ReaderToolbarUi::TailAction::None};
          case State::Locked:
            return {ReaderToolbarUi::TailBadge::Lock, ReaderToolbarUi::TailAction::None};
        }
        return {};
      };
    }
#endif
  } else if (overlay == Overlay::Text) {
    model.panelTitle = tr(STR_TOOL_TEXT);
    model.itemCount = kTextRowCount;
    model.rowText = [this](int i) { return textRowName(i); };
    model.rowValue = [this](int i) { return textRowValue(i); };
  } else if (overlay == Overlay::Bookmarks) {
    // One panel, two chips. Bookmarks and annotations share the row layout, the
    // fixed action strip and the tail delete button; only the content differs,
    // which is what keeps the two lists visually identical.
    const int bookmarkCount = static_cast<int>(cachedBookmarks.size());
    const int annotationCount = static_cast<int>(annotations_.size());
    model.bookmarkChips = true;
    model.annotationsChip = bookmarksPanelAnnotations;
    model.bookmarkCount = bookmarkCount;
    model.annotationCount = annotationCount;
    if (bookmarksPanelAnnotations) {
      model.panelTitle = tr(STR_ANNOTATIONS);
      model.annotationExportAction = true;
      model.itemCount = annotationCount > 0 ? annotationCount : 1;
      model.rowText = [this, annotationCount](int i) {
        if (annotationCount == 0) return std::string(tr(STR_NO_ANNOTATIONS));
        const auto& entry = annotations_[static_cast<size_t>(i)];
        return entry.note.empty() ? entry.quote : entry.note;
      };
      model.rowValue = [this, annotationCount](int i) {
        if (annotationCount == 0) return std::string();
        // An annotation is keyed to a spine position, not to a page in the
        // current section, so the value names the chapter instead of a percent.
        return std::string("#") + std::to_string(annotations_[static_cast<size_t>(i)].spineIndex + 1);
      };
      model.rowTail = [annotationCount](int i) -> ReaderToolbarUi::RowTail {
        return {ReaderToolbarUi::TailBadge::None, i < annotationCount ? ReaderToolbarUi::TailAction::Delete
                                                                     : ReaderToolbarUi::TailAction::None};
      };
      model.currentChapterIndex = -1;
    } else {
      model.panelTitle = tr(STR_BOOKMARKS);
      // The page action is the fixed strip above the list (the same chrome the
      // JJWXC Contents panel uses for "update chapter list"), not a fake first
      // row. Every row below it is a bookmark and only jumps; its tail carries
      // the delete button. An empty list still gets one "No bookmarks" row so
      // the panel is never blank.
      model.bookmarkAction = true;
      model.bookmarkActionRemove = currentPageBookmarked;
      model.itemCount = bookmarkCount > 0 ? bookmarkCount : 1;
      model.rowText = [this, bookmarkCount](int i) {
        if (bookmarkCount == 0) return std::string(tr(STR_NO_BOOKMARKS));
        const auto& bookmark = cachedBookmarks[static_cast<size_t>(i)];
        return bookmark.name.empty() ? bookmark.summary : bookmark.name;
      };
      model.rowValue = [this, bookmarkCount](int i) {
        if (bookmarkCount == 0) return std::string();
        char percent[8];
        snprintf(percent, sizeof(percent), "%d%%",
                 static_cast<int>(cachedBookmarks[static_cast<size_t>(i)].percentage * 100.0f + 0.5f));
        return std::string(percent);
      };
      model.rowTail = [bookmarkCount](int i) -> ReaderToolbarUi::RowTail {
        return {ReaderToolbarUi::TailBadge::None, i < bookmarkCount ? ReaderToolbarUi::TailAction::Delete
                                                                   : ReaderToolbarUi::TailAction::None};
      };
      model.currentChapterIndex = currentBookmarkIndex();
    }
  } else {
    model.panelTitle = tr(STR_TOOL_MORE);
    model.itemCount = static_cast<int>(moreItems.size());
    model.rowText = [this](int i) { return moreRowName(i); };
    model.rowValue = [this](int i) { return moreRowValue(i); };
    model.rowCheckboxContext = this;
    model.rowCheckbox = [](void* ctx, int i, freeink::ui::ListItem& item) {
      const auto* self = static_cast<EpubReaderActivity*>(ctx);
      using MA = EpubReaderMenuActivity::MenuAction;
      const auto action = self->moreItems[i].action;
      if (action == MA::NIGHT_MODE) GUI.setCheckboxRow(item, SETTINGS.screenInverted);
      if (action == MA::FRONTLIGHT) GUI.setCheckboxRow(item, Frontlight.isOn());
    };
  }
  toolbarUi->setModel(model);
  toolbarUi->render();
  toolbarUi->drawContentsFocusFrame();
}

void EpubReaderActivity::handleOverlayInput() {
  if (!toolbarUi) return;

  // A modal option picker over the panel owns all input while open.
  if (overlayPopup.isActive()) {
    overlayPopup.handleInput(mappedInput, [this] {
      if (overlayPopup.isActive()) {
        paintOverlayPopup();  // highlight moved
        return;
      }
      // Dismissed or selected: erase the dialog -- clean page back, then the
      // panel over it (the dialog can overhang the sheet onto the page).
      RenderLock lock;
      settleOverlayRefresh();
      if (overlayPageStored) {
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
        renderOverlay();
        pushOverlayRefresh();
      } else {
        requestUpdate();
      }
    });
    return;
  }
  const auto fastRedraw = [this] {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    renderOverlay();
    pushOverlayRefresh();
  };

  const bool textBook = Txt::isTxtOrMd(epub->getSourcePath());
  const int chapterCount = textBook ? epub->getTocItemsCount() : epub->getSpineItemsCount();
  const int chapterIndex = textBook ? std::max(0, currentTocIndex()) : currentSpineIndex;
  const auto gotoChapter = [this, textBook, chapterCount, chapterIndex](int target) {
    target = std::clamp(target, 0, chapterCount - 1);
    if (target != chapterIndex) {
      RenderLock lock;
      clearDeferredReposition();
      nextPageNumber = 0;
      if (textBook) {
        const auto item = epub->getTocItem(target);
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
      } else {
        currentSpineIndex = target;
      }
      section.reset();
    }
    requestUpdate();
  };
  const auto toolOverlay = [](int tool) {
    return tool == 0   ? Overlay::Contents
           : tool == 1 ? Overlay::Text
           : tool == 2 ? Overlay::Bookmarks
                       : Overlay::More;
  };

  // Touch first: FreeInkUI routes the frame against the tap targets the last
  // render registered and hands back the action it mapped to.
  const auto routed = toolbarUi->route(mappedInput);

  // Grabber gestures: up on the toolbar's grabber opens the chapter list, down
  // on a panel's grabber collapses to the toolbar. Other swipes remain list
  // scrolling. (Device feedback: Back now leaves the reading surface entirely.)
  int swipeSx = 0;
  int swipeSy = 0;
  int swipeEx = 0;
  int swipeEy = 0;
  const bool hasSwipeGeometry = mappedInput.wasSwipeGeometry(swipeSx, swipeSy, swipeEx, swipeEy);
  const auto swipeDir = mappedInput.wasSwipe();
  const bool grabberSwipe =
      hasSwipeGeometry && swipeSy >= toolbarUi->sheetTop() && swipeSy < toolbarUi->grabberZoneBottom();

  // --- Toolbar ---
  if (overlay == Overlay::Toolbar) {
    if (grabberSwipe && swipeDir == MappedInputManager::SwipeDir::Up) {
      focusedTool = 0;
      openOverlay(Overlay::Contents);
      return;
    }
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeOverlayToPage();
        return;
      case ReaderToolbarUi::Event::Tool:
        focusedTool = routed.value;
        openOverlay(toolOverlay(focusedTool));
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        gotoChapter(chapterIndex - 1);
        return;
      case ReaderToolbarUi::Event::NextChapter:
        gotoChapter(chapterIndex + 1);
        return;
      case ReaderToolbarUi::Event::Scrub:
        gotoChapter(static_cast<int>(
            (static_cast<float>(routed.permille) / 1000.0f) * static_cast<float>(chapterCount - 1) + 0.5f));
        return;
      default:
        break;
    }
    if (routed.routed) return;  // a touch frame the chrome consumed (or dead space)

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeOverlayToPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + 3) % 4;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % 4;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openOverlay(toolOverlay(focusedTool));
      return;
    }
    const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (prev || next) {
      gotoChapter(chapterIndex + (next ? 1 : -1));
    }
    return;
  }

  // --- Panels (Contents / Text / More) ---
  const int count = overlay == Overlay::Contents
                        ? (contentsFilter_ != 0 && contentsRows_.empty()
                               ? 1
                               : static_cast<int>(contentsRows_.size()))
                    : overlay == Overlay::Text    ? kTextRowCount
                    : overlay == Overlay::Bookmarks ? static_cast<int>(cachedBookmarks.size())
                                                  : static_cast<int>(moreItems.size());
  const int pageRows = std::max(1, toolbarUi->visibleRows());

  // Activate the highlighted row: change a value / jump to a chapter / run an
  // action. Shared by the Confirm button and a row tap.
  const auto activateRow = [this, count, &fastRedraw] {
    if (panelIndex < 0 || panelIndex >= count) return;
    if (overlay == Overlay::Text) {
      if (panelIndex == 0) {
        // Full font picker (built-in + SD fonts, live preview) -- the same
        // screen Settings uses; a popup cannot scroll a long font list.
        overlay = Overlay::None;
        overlayPopup.dismiss();
        discardOverlayPage();
        {
          RenderLock lock;
          settleOverlayRefresh();
        }
        auto textSettings = makeUniqueNoThrow<TextSettingsActivity>(
            renderer, mappedInput, &sdFontSystem.registry(), TextSettingsActivity::Tab::Family,
            TextSettingsActivity::InitialFontState::Unchanged, TextSettingsActivity::StartMode::PreviewOnly);
        if (!textSettings) {
          LOG_ERR("ERS", "OOM allocating text settings");
          overlay = Overlay::Text;
          return;
        }
        startActivityForResult(std::move(textSettings), [this](const ActivityResult&) {
          applyReaderTextSettings();
          overlay = Overlay::Text;  // back to the Text panel
          panelIndex = 0;
          if (toolbarUi) toolbarUi->begin();  // the picker drew its own FUI screen
          requestUpdate();                    // re-render page + Text panel
        });
      } else {
        // Value cells open the Settings-style picker; the grid's own 56x56
        // step buttons (ACTION_STEP_MINUS/PLUS) are the fast path. Switch
        // cells toggle in place, and the last cell restores the defaults.
        if (panelIndex >= 1 && panelIndex <= 4) {
          showTextRowPopup(panelIndex);
        } else if (panelIndex == 5 || panelIndex == 6) {
          toggleReaderSwitch(panelIndex);
          fastRedraw();
        } else if (panelIndex == 7) {
          restoreReaderDefaults();
        }
      }
    } else if (overlay == Overlay::Contents) {
      const int tocIndex = contentsTocForRow(panelIndex);
      if (tocIndex < 0) return;
      const auto item = epub->getTocItem(tocIndex);
      if (item.spineIndex != -1) {
        RenderLock lock;
        clearDeferredReposition();
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
        nextPageNumber = 0;
        section.reset();
      }
      overlay = Overlay::None;
      discardOverlayPage();
      requestUpdate();
    } else if (overlay == Overlay::Bookmarks) {
      if (bookmarksPanelAnnotations) {
        if (panelIndex < 0 || static_cast<size_t>(panelIndex) >= annotations_.size()) return;
        const auto& entry = annotations_[static_cast<size_t>(panelIndex)];
        ProgressChangeResult sync{};
        sync.hasSavedProgress = true;
        sync.hasVisibleTextOffset = true;
        sync.visibleTextOffset = entry.startOffset;
        sync.spineIndex = entry.spineIndex;
        overlay = Overlay::None;
        discardOverlayPage();
        applyProgressChange(sync);
        return;
      }
      // The page action is the fixed strip above the list, so a row index is
      // the bookmark index again (the old +1 offset is what made taps land on
      // the wrong entry).
      const size_t bookmarkIndex = static_cast<size_t>(panelIndex);
      if (panelIndex < 0 || bookmarkIndex >= cachedBookmarks.size()) return;
      const auto& bookmark = cachedBookmarks[bookmarkIndex];
      ProgressChangeResult sync{};
      sync.xpath = bookmark.xpath;
      sync.percentage = bookmark.percentage;
      sync.hasSavedProgress = true;
      sync.hasVisibleTextOffset = bookmark.hasVisibleTextOffset;
      sync.visibleTextOffset = bookmark.visibleTextOffset;
      // The offset is spine-relative, so carry its spine even when the legacy
      // page hints are stale. The jump validates the index.
      sync.spineIndex = bookmark.computedSpineIndex;
      if (bookmark.computedChapterPageCount > 0 && bookmark.computedChapterProgress < bookmark.computedChapterPageCount &&
          bookmark.computedSpineIndex < epub->getSpineItemsCount()) {
        sync.page = bookmark.computedChapterProgress;
        sync.totalPages = bookmark.computedChapterPageCount;
      }
      overlay = Overlay::None;
      discardOverlayPage();
      applyProgressChange(sync);
    } else if (overlay == Overlay::More) {
      activateMoreRow(panelIndex);
    }
  };

  // Steps up to the toolbar -- the Back button and a tap on the page above
  // the sheet.
  const auto dismissPanel = [this, &fastRedraw] {
    overlay = Overlay::Toolbar;
    // Restore the snapshotted page under the toolbar instead of re-rendering
    // it (2+ refreshes -> one FAST). Re-store right away so another panel
    // round-trip can restore again.
    if (overlayPageStored) {
      {
        RenderLock lock;  // the render task shares the framebuffer
        settleOverlayRefresh();
        // No baseline resync: the glass shows the panel, and erasing it needs
        // the differential to keep diffing against the last pushed frame.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      fastRedraw();  // takes its own RenderLock
      return;
    }
    requestUpdate();
  };

  // Pages the list by one screen of rows through the nav (measured page size,
  // no-op at the ends). A shown cursor rides along so the buttons continue
  // from what is visible; on touch boards only the viewport moves.
  const auto pageList = [this, count, pageRows, &fastRedraw](int direction) {
    if (count <= 0) return;
    const bool moved = toolbarUi->nav().scrollBy(direction * pageRows, count);
    if (panelCursorShown) {
      panelIndex = std::clamp(panelIndex + direction * pageRows, 0, count - 1);
      fastRedraw();
      return;
    }
    if (moved) fastRedraw();
  };

  switch (routed.event) {
    case ReaderToolbarUi::Event::Dismiss:
      dismissPanel();
      return;
    case ReaderToolbarUi::Event::ContentsFilter:
      if (overlay == Overlay::Contents) {
        contentsFilter_ = std::clamp(routed.value, 0, 2);
        rebuildContentsRows();
        const int currentRow = contentsRowForToc(currentTocIndex());
        panelIndex = currentRow >= 0 ? currentRow : 0;
        if (toolbarUi) {
          toolbarUi->nav().reset(panelIndex);
          toolbarUi->nav().top = panelIndex;
        }
        requestUpdate();
      }
      return;
    case ReaderToolbarUi::Event::TextStepMinus:
    case ReaderToolbarUi::Event::TextStepPlus:
      if (overlay == Overlay::Text) {
        stepTextRow(routed.value, routed.event == ReaderToolbarUi::Event::TextStepMinus ? -1 : 1);
      }
      return;
    case ReaderToolbarUi::Event::TextDone:
      if (overlay == Overlay::Text) closeOverlayToPage();
      return;
    case ReaderToolbarUi::Event::UpdateList:
      // Fixed Contents strip: fetch the catalogue, then always repackage so
      // new chapters appear as placeholders and old nav suffixes disappear.
#ifdef BOARD_HAS_PSRAM
      // PSRAM-only entry: the JJWXC app is compiled out without PSRAM (C3).
      openJjwxcUpdate(/*currentChapter=*/false, /*returnToContents=*/true);
#endif
      return;
    case ReaderToolbarUi::Event::Tool: {
      // Sheet-bottom tool switcher: hop straight to another panel.
      const Overlay target = toolOverlay(routed.value);
      if (target != overlay) {
        focusedTool = routed.value;
        openOverlay(target);
      }
      return;
    }
    case ReaderToolbarUi::Event::BookmarkAction:
      if (overlay == Overlay::Bookmarks) {
        addBookmark();
        loadCachedBookmarks();  // refresh rows, the page flag and the strip label
        panelIndex = 0;
        panelCursorShown = false;
      }
      return;
    case ReaderToolbarUi::Event::BookmarkChip:
      if (overlay == Overlay::Bookmarks && bookmarksPanelAnnotations) {
        bookmarksPanelAnnotations = false;
        panelIndex = 0;
        panelCursorShown = false;
        requestUpdate();
      }
      return;
    case ReaderToolbarUi::Event::AnnotationChip:
      if (overlay == Overlay::Bookmarks && !bookmarksPanelAnnotations) {
        bookmarksPanelAnnotations = true;
        panelIndex = 0;
        panelCursorShown = false;
        requestUpdate();
      }
      return;
    case ReaderToolbarUi::Event::ExportAnnotations:
      if (overlay == Overlay::Bookmarks && bookmarksPanelAnnotations) exportAnnotations();
      return;
    case ReaderToolbarUi::Event::ChapterAction:
      if (overlay == Overlay::Bookmarks && bookmarksPanelAnnotations) {
        // Row-tail delete in the annotation list: same one-tap contract as a
        // bookmark's, and the file is rewritten atomically.
        if (routed.value >= 0 && static_cast<size_t>(routed.value) < annotations_.size()) {
          annotations_.erase(annotations_.begin() + routed.value);
          saveAnnotations();
          panelIndex = std::min(panelIndex, std::max(0, static_cast<int>(annotations_.size()) - 1));
          requestUpdate();
        }
        return;
      }
      if (overlay == Overlay::Bookmarks) {
        // Row-tail delete: no confirmation, the bookmark is one tap from being
        // re-added and the popup reports it.
        if (routed.value >= 0 && static_cast<size_t>(routed.value) < cachedBookmarks.size()) {
          cachedBookmarks.erase(cachedBookmarks.begin() + routed.value);
          if (!BookmarkFile::save(epub->getSourcePath(), cachedBookmarks)) LOG_ERR("ERS", "Failed to save bookmarks");
          bookmarkRemoved = true;
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          loadCachedBookmarks();
          panelIndex = std::min(panelIndex, std::max(0, static_cast<int>(cachedBookmarks.size()) - 1));
          requestUpdate();
        }
        return;
      }
#ifdef BOARD_HAS_PSRAM
      if (overlay == Overlay::Contents) activateJjwxcChapterChip(contentsTocForRow(routed.value));
#endif
      return;
    case ReaderToolbarUi::Event::Row:
      // A tap on the right-edge strip pages the sheet instead (upper half =
      // previous page, lower half = next): swipes are unreliable on etched
      // glass, and a long contents list needs a fast way through.
      if (routed.x >= renderer.getScreenWidth() - 44) {
        pageList(routed.y >= renderer.getScreenHeight() - (renderer.getScreenHeight() * 62) / 200 ? 1 : -1);
        return;
      }
      panelIndex = routed.value;
      panelCursorShown = false;
      // The type grid's steppers are their own 56x56 hits (ACTION_STEP_*);
      // a tap on the cell body opens the picker / toggles, not a step.
      activateRow();
      return;
    default:
      break;
  }
  // Swipe up/down pages the list, unless the gesture started on the grabber:
  // dragging that down collapses this panel back to the toolbar.
  if (grabberSwipe && swipeDir == MappedInputManager::SwipeDir::Down) {
    dismissPanel();
    return;
  }
  // Checked before the routed-frame return: FUI routes every touch frame over
  // the sheet, so a swipe's frames count as routed (without dispatching -- too
  // much travel for a tap) and the gesture would otherwise never be seen.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageList(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  if (routed.routed) return;  // consumed by the chrome (title band, dead space)

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeOverlayToPage();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }

  // Up/Down (side) and Left/Right (front) move the cursor: a tap steps one
  // row, holding past PANEL_HOLD_MS jumps PANEL_HOLD_STEP rows in one go, which
  // is how you cross a hundreds-of-chapters contents list without a press per
  // row. The jump fires once on the hold and swallows the release that ends it,
  // so it never doubles up with the tap step.
  // D5: in the Text panel, Left/Right are the value steppers (press fires,
  // holding repeats); Up/Down still move the cursor. The releases are
  // consumed here so the cursor block below never double-fires.
  if (overlay == Overlay::Text && count > 0 && panelIndex >= 1 && panelIndex <= 4) {
    const auto heldStep = [this](const MappedInputManager::Button button, const int direction) {
      if (mappedInput.wasPressed(button)) {
        lastTextStepMs = millis();
        stepTextRow(panelIndex, direction);
        return true;
      }
      if (mappedInput.isPressed(button) && millis() - lastTextStepMs >= kTextStepRepeatMs) {
        lastTextStepMs = millis();
        stepTextRow(panelIndex, direction);
        return true;
      }
      return false;
    };
    if (heldStep(MappedInputManager::Button::Right, 1)) return;
    if (heldStep(MappedInputManager::Button::Left, -1)) return;
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      return;
    }
  }

  if (count > 0) {
    const bool up = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                    mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool down = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                      mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!panelHoldJumped && (up || down) && mappedInput.getHeldTime() >= PANEL_HOLD_MS) {
      const int step = down ? PANEL_HOLD_STEP : -PANEL_HOLD_STEP;
      panelIndex = std::clamp(panelIndex + step, 0, count - 1);
      panelHoldJumped = true;
      panelCursorShown = true;
      fastRedraw();
      return;
    }

    const bool releasedUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool releasedDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (releasedUp || releasedDown) {
      if (!panelHoldJumped) {
        panelIndex = releasedUp ? ButtonNavigator::previousIndex(panelIndex, count)
                                : ButtonNavigator::nextIndex(panelIndex, count);
        panelCursorShown = true;
        fastRedraw();
      }
      panelHoldJumped = false;
    }
  }
}

// First paint of the option picker over the panel (and highlight repaints).
// The dialog draws over the current framebuffer without clearing; erasing it
// on dismissal is the popup gate's restore in handleOverlayInput().
void EpubReaderActivity::paintOverlayPopup() {
  RenderLock lock;
  settleOverlayRefresh();
  overlayPopup.render(renderer);
  pushOverlayRefresh();
}

void EpubReaderActivity::applyReaderTextSettings() {
  SETTINGS.saveToFile();
  RenderLock lock;
  // (Re)load or unload the selected SD-card font for the current family/size.
  // The reader otherwise only loads SD fonts on book open, so without this an
  // in-reader font change wouldn't take effect until re-opening the book.
  sdFontSystem.ensureLoaded(renderer);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  section.reset();  // force re-pagination with the new settings
}

void EpubReaderActivity::finishFontPreview() {
  const auto* family = sdFontSystem.registry().findFamily(SETTINGS.sdFontFamilyName);
  const auto* file = family ? family->findNearestSize(SETTINGS.fontPointSize) : nullptr;
  const bool supportsPreload = !family || !family->vector;
  const auto check =
      file && supportsPreload ? SdCardFontCache::preflight(file->path.c_str()) : SdCardFontCache::Result::InvalidFont;
  const auto decision = fontPreview.finish(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize,
                                           check == SdCardFontCache::Result::AlreadyCached, supportsPreload);
  switch (decision) {
    case ReaderFontPreview::Decision::Keep:
      return;
    case ReaderFontPreview::Decision::Enable: {
      RenderLock lock;
      const bool reload = SETTINGS.sdFontFlashPreload == 0;
      SETTINGS.sdFontFlashPreload = 1;
      // ensureLoaded's same-family/size fast path does not change the source.
      if (reload) sdFontSystem.releaseLoadedFont(renderer);
      sdFontSystem.ensureLoaded(renderer);
      break;
    }
    case ReaderFontPreview::Decision::Disable:
    case ReaderFontPreview::Decision::Ask:
      SETTINGS.sdFontFlashPreload = 0;
      break;
  }
  SETTINGS.saveToFile();
  if (decision != ReaderFontPreview::Decision::Ask) return;

  if (check == SdCardFontCache::Result::TooLarge) {
    {
      RenderLock lock;
      fontPromptState = FontPromptState::TooLarge;
    }
    requestUpdateAndWait();
    fontNoticeStartedAt = millis();
    return;
  }
  {
    RenderLock lock;
    fontPromptState = FontPromptState::Asking;
    fontPromptWaitForBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
    if (check == SdCardFontCache::Result::Ok) {
      constexpr StrId options[] = {StrId::STR_FONT_PRELOAD_START, StrId::STR_FONT_PRELOAD_SKIP};
      overlayPopup.show(StrId::STR_FONT_PRELOAD_CONFIRM, options, static_cast<int>(std::size(options)), 0,
                        [this](int index) {
                          RenderLock lock;
                          if (index == 0) fontPromptState = FontPromptState::Accepted;
                        });
    } else {
      constexpr StrId options[] = {StrId::STR_OK_BUTTON};
      overlayPopup.show(fontpreload::failureMessage(check), options, static_cast<int>(std::size(options)), 0, {});
    }
  }
  requestUpdate();
}

void EpubReaderActivity::handleFontPreloadPrompt() {
  if (fontPromptState == FontPromptState::TooLarge) {
    if (millis() - fontNoticeStartedAt >= fontpreload::NOTICE_DURATION_MS) {
      for (uint8_t button = 0; button < MappedInputManager::kButtonCount; ++button) {
        if (mappedInput.isPressed(static_cast<MappedInputManager::Button>(button))) return;
      }
      int x = 0, y = 0;
      if (mappedInput.isScreenTouchHeld(x, y)) return;
      {
        RenderLock lock;
        fontPromptState = FontPromptState::Idle;
      }
      requestUpdate();
    }
    return;
  }
  if (fontPromptWaitForBackRelease) {
    fontPromptWaitForBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
    return;  // Consume the inherited release as well as the hold.
  }
  overlayPopup.handleInput(mappedInput, [this] { requestUpdate(); });
  if (overlayPopup.isActive()) return;

  const bool accepted = fontPromptState == FontPromptState::Accepted;
  {
    RenderLock lock;
    fontPromptState = FontPromptState::Idle;
    overlayPopup.dismiss();
  }
  if (!accepted) {
    requestUpdate();
    return;
  }

  // ActivityManager owns this existing progress activity across loop frames;
  // stack storage cannot outlive this call. No second font/cache buffer is added.
  auto preload = makeUniqueNoThrow<TextSettingsActivity>(
      renderer, mappedInput, &sdFontSystem.registry(), TextSettingsActivity::Tab::Family,
      TextSettingsActivity::InitialFontState::Changed, TextSettingsActivity::StartMode::PreloadThenExit);
  if (!preload) {
    LOG_ERR("ERS", "OOM allocating font preload activity (%zu bytes)", sizeof(TextSettingsActivity));
    SETTINGS.sdFontFlashPreload = 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(preload), [this](const ActivityResult&) {
    {
      RenderLock lock;
      // Also covers a cache that became valid between the prompt and preload.
      sdFontSystem.releaseLoadedFont(renderer);
      sdFontSystem.ensureLoaded(renderer);
    }
    lastPageTurnTime = millis();
    requestUpdate();
  });
}

// The More panel carries everything the classic list menu offers except the
// two entries that have their own tool (chapters -> Contents, text -> Text).
void EpubReaderActivity::buildMoreActions() {
  using MA = EpubReaderMenuActivity::MenuAction;
#ifdef BOARD_HAS_PSRAM
  resolveJjwxcBook();
  const bool isJjwxc = jjwxcBook_;
#else
  const bool isJjwxc = false;
#endif
  EpubReaderMenuActivity::buildMenuItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty(), isJjwxc);
  // "Toggle Bookmark" never said whether the tap would add or remove one. Name
  // the action the row will actually perform on this page.
  for (auto& item : moreItems) {
    if (item.action == MA::TOGGLE_BOOKMARK) {
      item.labelId = currentPageBookmarked ? StrId::STR_REMOVE_BOOKMARK : StrId::STR_ADD_BOOKMARK;
    }
  }
  moreItems.erase(std::remove_if(moreItems.begin(), moreItems.end(),
                                 [](const auto& item) {
                                   return item.action == MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS ||
                                          item.action == MA::JJWXC_UPDATE_CURRENT ||
                                          item.action == MA::JJWXC_UPDATE_LIST;
                                 }),
                  moreItems.end());
}

std::string EpubReaderActivity::moreRowName(int row) const {
  return row >= 0 && row < static_cast<int>(moreItems.size()) ? I18N.get(moreItems[row].labelId) : "";
}

std::string EpubReaderActivity::moreRowValue(int row) const {
  using MA = EpubReaderMenuActivity::MenuAction;
  static constexpr StrId kOrient[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(std::size(kOrient) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return "";
  switch (moreItems[row].action) {
    case MA::ROTATE_SCREEN:
      return I18N.get(kOrient[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]);
    case MA::AUTO_PAGE_TURN:
      return (autoTurnOption == 0 || autoTurnOption >= static_cast<int>(std::size(PAGE_TURN_RATES)))
                 ? std::string(tr(STR_STATE_OFF))
                 : std::to_string(PAGE_TURN_RATES[autoTurnOption]);
    case MA::NIGHT_MODE:
      return SETTINGS.screenInverted ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MA::FRONTLIGHT:
      return Frontlight.isOn() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MA::IMAGE_SCALING: {
      // Same labels the reader menu's option popup offers, so the row's value
      // and the popup's highlighted entry always agree.
      static constexpr StrId kScaling[] = {StrId::STR_IMAGE_SCALING_NEAREST, StrId::STR_IMAGE_SCALING_BILINEAR};
      static_assert(std::size(kScaling) == CrossPointSettings::IMAGE_SCALING_COUNT, "image scaling labels");
      const size_t mode = SETTINGS.imageScaling < CrossPointSettings::IMAGE_SCALING_COUNT ? SETTINGS.imageScaling : 0;
      return I18N.get(kScaling[mode]);
    }
    default:
      return "";
  }
}

void EpubReaderActivity::activateMoreRow(int row) {
  using MA = EpubReaderMenuActivity::MenuAction;
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return;
  {
    RenderLock lock;  // several actions launch screens that paint the framebuffer
    settleOverlayRefresh();
  }
  const auto action = moreItems[row].action;
  // In-place toggles keep the panel open and re-render the page beneath it.
  switch (action) {
    case MA::ROTATE_SCREEN: {
      static constexpr StrId kOrientIds[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
      static_assert(std::size(kOrientIds) == CrossPointSettings::ORIENTATION_COUNT, "orientation options");
      overlayPopup.show(StrId::STR_ORIENTATION, kOrientIds, static_cast<int>(std::size(kOrientIds)),
                        SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT, [this](int idx) {
                          if (idx == SETTINGS.orientation) return;
                          applyOrientation(static_cast<uint8_t>(idx));
                          // The stored page is laid out for the old orientation.
                          discardOverlayPage();
                          requestUpdate();
                        });
      paintOverlayPopup();
      return;
    }
    case MA::AUTO_PAGE_TURN: {
      std::vector<std::string> labels;
      labels.reserve(std::size(PAGE_TURN_RATES));
      labels.emplace_back(tr(STR_STATE_OFF));
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) labels.push_back(std::to_string(PAGE_TURN_RATES[i]));
      overlayPopup.show(StrId::STR_AUTO_TURN_PAGES_PER_MIN, labels, autoTurnOption, [this](int idx) {
        autoTurnOption = idx;
        toggleAutoPageTurn(static_cast<uint8_t>(PAGE_TURN_RATES[idx]));
      });
      paintOverlayPopup();
      return;
    }
    case MA::IMAGE_SCALING: {
      // A plain on/off-style row like the frontlight one: one tap cycles, no
      // picker. The value only reaches the page when the overlay closes, so the
      // row repaints the panel over the page snapshot instead of re-rendering
      // the page (which would decode the image twice in a row).
      const uint8_t current =
          SETTINGS.imageScaling < CrossPointSettings::IMAGE_SCALING_COUNT ? SETTINGS.imageScaling : 0;
      const uint8_t next = static_cast<uint8_t>((current + 1) % CrossPointSettings::IMAGE_SCALING_COUNT);
      SETTINGS.imageScaling = next;
      SETTINGS.saveToFile();
      imageScalingDirty = true;
      LOG_INF("ERS", "Image scaling -> %s",
              next == CrossPointSettings::IMAGE_SCALING_BILINEAR ? "bilinear" : "nearest");
      RenderLock lock;  // the render task shares the framebuffer
      if (overlayPageStored) {
        // Clean page back first (the sheet's old value text is not
        // background-filled), then re-snapshot it and lay the panel on top.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      renderOverlay();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    case MA::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      discardOverlayPage();
      requestUpdate();
      return;
    case MA::FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      {
        RenderLock lock;  // the render task shares the framebuffer
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      return;
    }
    default:
      break;
  }
  // Leaf actions open their own screen / perform the action; close the overlay first.
  overlay = Overlay::None;
  if (action == MA::GO_TO_PERCENT && overlayPageStored) {
    // The percent dialog is a popup over the current frame: wipe the toolbar
    // chrome back to the clean page first so the dialog draws over the page,
    // not the sheet. No refresh push — the dialog's first frame carries it.
    RenderLock lock;
    settleOverlayRefresh();
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
  } else {
    discardOverlayPage();
  }
  if (action == MA::TOGGLE_BOOKMARK) {
    // No child activity here to trigger the re-render the list menu relies on:
    // show the same confirmation popup the long-press path does.
    addBookmark();
    showBookmarkMessage = true;
    bookmarkMessageTime = millis();
    requestUpdate();
    return;
  }
  onReaderMenuConfirm(action);
  // Actions that neither open a screen nor leave the reader (a sync with no
  // credentials, say) would otherwise leave the closed panel on screen.
  if (action != MA::GO_HOME && action != MA::DELETE_CACHE) requestUpdate();
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage, currentPageVisibleOffset};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    return;
  }

  if (savePosition) {
    // Full: drop the oldest entry so Back always returns from the newest jump.
    if (footnoteDepth == MAX_FOOTNOTE_DEPTH) {
      std::copy(savedPositions + 1, savedPositions + MAX_FOOTNOTE_DEPTH, savedPositions);
      footnoteDepth--;
    }
    // A child screen (menu, footnote list) may have released the section;
    // nextPageNumber then holds the page it was on.
    const int page = section ? section->currentPage : nextPageNumber;
    savedPositions[footnoteDepth] = {currentSpineIndex, page};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, page);
  }

  {
    RenderLock lock;
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::saveLinkStack() const {
  // [depth] then depth x (spine u16 LE, page u16 LE)
  uint8_t data[1 + MAX_FOOTNOTE_DEPTH * 4];
  size_t size = 0;
  data[size++] = static_cast<uint8_t>(footnoteDepth);
  for (int i = 0; i < footnoteDepth; i++) {
    const SavedPosition& pos = savedPositions[i];
    data[size++] = pos.spineIndex & 0xFF;
    data[size++] = (pos.spineIndex >> 8) & 0xFF;
    data[size++] = pos.pageNumber & 0xFF;
    data[size++] = (pos.pageNumber >> 8) & 0xFF;
  }
  HalFile f;
  if (!Storage.openFileForWrite("ERS", epub->getCachePath() + "/links.bin", f)) return;
  if (f.write(data, size) != size) LOG_ERR("ERS", "Failed to write link stack");
}

void EpubReaderActivity::loadLinkStack() {
  const std::string path = epub->getCachePath() + "/links.bin";
  if (!Storage.exists(path.c_str())) return;
  {
    HalFile f;
    uint8_t data[1 + MAX_FOOTNOTE_DEPTH * 4];
    if (Storage.openFileForRead("ERS", path, f)) {
      const int size = f.read(data, sizeof(data));
      const int depth = size > 0 ? data[0] : 0;
      if (depth >= 1 && depth <= MAX_FOOTNOTE_DEPTH && size == 1 + depth * 4) {
        const int spineCount = epub->getSpineItemsCount();
        bool valid = true;
        for (int i = 0; i < depth; i++) {
          const uint8_t* p = data + 1 + i * 4;
          savedPositions[i] = {p[0] | (p[1] << 8), p[2] | (p[3] << 8)};
          valid = valid && savedPositions[i].spineIndex < spineCount;
        }
        if (valid) {
          footnoteDepth = depth;
          LOG_DBG("ERS", "Loaded link stack, depth %d", depth);
        }
      }
    }
  }
  // Consumed once: a later exit rewrites it, and an unclean shutdown must not
  // resurrect a stale stack.
  Storage.remove(path.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    pendingAnchor.clear();
    pendingPercentJump = false;
    pendingPageJump.reset();
    pendingOffsetJump = pos.visibleTextOffset;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getSourcePath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock;
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getSourcePath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

int EpubReaderActivity::currentBookmarkIndex() const {
  if (!section || !epub || cachedBookmarks.empty()) return -1;
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  for (size_t i = 0; i < cachedBookmarks.size(); ++i) {
    if (bookmarkMatchesProgress(cachedBookmarks[i], currentSpineIndex, section->currentPage, pageCount, pageRange)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void EpubReaderActivity::drawBookmarkCorner(const int top, const int rightMargin) {
  (void)top;
  (void)rightMargin;
  // v5 ribbon: 28x72, 16 px in from the right edge, hanging from the top edge.
  // Unmarked = 2 px outline; marked = solid black with the page showing through
  // the V notch at the bottom. The hit target stays 88x104 (the design's
  // comfort target) anchored to the right edge, so it covers the ribbon without
  // spilling off screen.
  constexpr int kW = 28;
  constexpr int kH = 72;
  constexpr int kNotch = 12;
  constexpr int kHitW = 88;
  constexpr int kHitH = 104;
  const int screenW = renderer.getScreenWidth();
  const int x = screenW - 16 - kW;
  bookmarkCornerRect_ = Rect{std::max(0, screenW - kHitW), 0, kHitW, kHitH};

  if (currentPageBookmarked) {
    renderer.fillRect(x, 0, kW, kH - kNotch, true);
    for (int row = kH - kNotch; row < kH; ++row) {
      const int d = (row - (kH - kNotch)) * (kW / 2) / kNotch;
      renderer.fillRect(x + d, row, kW - d * 2, 1, true);
    }
  } else {
    renderer.drawLine(x, 0, x, kH - kNotch, 2, true);
    renderer.drawLine(x + kW, 0, x + kW, kH - kNotch, 2, true);
    renderer.drawLine(x, 0, x + kW, 0, 2, true);
    renderer.drawLine(x, kH - kNotch, x + kW / 2, kH, 2, true);
    renderer.drawLine(x + kW / 2, kH, x + kW, kH - kNotch, 2, true);
  }
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

int EpubReaderActivity::getProgressBasisPoints() const {
  if (isAtEndOfBook()) return 10000;
  if (!epub || !section || epub->getBookSize() == 0) return getProgressPercent() * 100;
  const int totalPages = section->estimatedTotalPages();
  if (totalPages <= 0) return getProgressPercent() * 100;
  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(totalPages);
  const int basisPoints =
      static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 10000.0f + 0.5f);
  return std::clamp(basisPoints, 0, 10000);
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  localPos.hasResolvedSpineIndex = true;
  localPos.hasMappedPage = true;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
