#include "CbzReaderActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "AchievementsStore.h"
#include "CrossPointSettings.h"
#include "ProgressFile.h"
#include "ReadingStatsStore.h"
#include "ReaderActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/AchievementPopupUtils.h"

namespace {
constexpr int kThumbHeight = 200;
}

bool CbzReaderActivity::loadBook() {
  auto loaded = makeUniqueNoThrow<Cbz>(bookPath, "/.crosspoint");
  if (!loaded) {
    LOG_ERR("CBR", "OOM: CBZ object");
    return false;
  }
  if (!loaded->load()) {
    LOG_ERR("CBR", "Failed to load CBZ");
    return false;
  }
  cbz = std::move(loaded);
  cbz->setupCacheDir();
  loadProgress();

  const int total = cbz->getPageCount();
  const uint8_t startPercent = total > 0 ? static_cast<uint8_t>(std::min(100, (currentPage + 1) * 100 / total)) : 0;
  READING_STATS.beginSession(cbz->getPath(), cbz->getTitle(), cbz->getAuthor(), cbz->getThumbBmpPath(kThumbHeight),
                             startPercent, "", 0);
  return true;
}

void CbzReaderActivity::onExit() {
  READING_STATS.endSession();
  ACHIEVEMENTS.recordSessionEnded(READING_STATS.getLastSessionSnapshot());
  showPendingAchievementPopups(renderer);
  ReaderActivity::onExit();
}

bool CbzReaderActivity::handleFormatInput() {
  READING_STATS.tickActiveSession();
  return false;
}

void CbzReaderActivity::applyInitialOrientation() {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
}

void CbzReaderActivity::renderBook() {
  if (!cbz || currentPage < 0 || currentPage >= cbz->getPageCount()) return;
  renderPage();
  saveProgress();
}

void CbzReaderActivity::renderPage() {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const std::string bmpPath = cbz->getCachePath() + "/page.bmp";

  bool converted = false;
  {
    // The JPEG/PNG converters use the framebuffer as scratch when it is lent;
    // this is the same protocol the image viewer uses for full-page previews.
    GfxRenderer::FrameBufferLoan loan(renderer);
    converted = cbz->renderPageToBmp(currentPage, bmpPath, screenWidth, screenHeight);
  }
  if (!converted) {
    LOG_ERR("CBR", "Failed to render page %d/%d", currentPage + 1, cbz->getPageCount());
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("CBR", bmpPath, file)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, tr(STR_INVALID_IMAGE_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  const int x = (screenWidth - bitmap.getWidth()) / 2;
  const int y = (screenHeight - bitmap.getHeight()) / 2;
  renderer.clearScreen();
  if (!renderer.drawBitmap(bitmap, x, y, screenWidth, screenHeight, 0, 0)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // Keep a small, readable page indicator. The fixed-layout XTC status-bar
  // preference is reused so the two image readers behave alike.
  const auto sb = SETTINGS.statusBarSpec();
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight > 0 && sb.xtcMode != CrossPointSettings::XTC_STATUS_BAR_HIDE) {
    const bool bottom = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_BOTTOM;
    const int clearY = bottom ? std::max(0, screenHeight - statusBarHeight - 4) : 0;
    const int clearHeight = bottom ? screenHeight - clearY : statusBarHeight + 4;
    renderer.fillRect(0, clearY, screenWidth, clearHeight, false);
    const float progress = cbz->getPageCount() > 0 ? (currentPage + 1) * 100.0f / cbz->getPageCount() : 0.0f;
    GUI.drawStatusBar(renderer, progress, currentPage + 1, cbz->getPageCount(), cbz->getTitle(),
                      bottom ? 0 : screenHeight - statusBarHeight - 4);
  }

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh--;
  }
}

bool CbzReaderActivity::pageTurn(const bool isForward) {
  if (!cbz) return false;
  READING_STATS.noteActivity();
  const int pageCount = cbz->getPageCount();
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

bool CbzReaderActivity::skipPages(const int amount) {
  if (!cbz || amount == 0) return false;
  READING_STATS.noteActivity();
  const int pageCount = cbz->getPageCount();
  const int next = std::clamp(currentPage + amount, 0, pageCount);
  if (next == currentPage) return false;
  currentPage = next;
  return true;
}

bool CbzReaderActivity::isAtEndOfBook() const { return cbz && currentPage >= cbz->getPageCount(); }

void CbzReaderActivity::onReturnFromEndOfBook() {
  currentPage = cbz && cbz->getPageCount() > 0 ? cbz->getPageCount() - 1 : 0;
}

void CbzReaderActivity::saveProgress() const {
  if (!cbz) return;
  const int total = cbz->getPageCount();
  const uint8_t percent = total > 0 ? static_cast<uint8_t>(std::min(100, (currentPage + 1) * 100 / total)) : 0;
  READING_STATS.updateProgress(percent, total > 0 && currentPage + 1 >= total, "", percent);

  uint8_t data[4] = {static_cast<uint8_t>(currentPage & 0xFF), static_cast<uint8_t>((currentPage >> 8) & 0xFF),
                     static_cast<uint8_t>((currentPage >> 16) & 0xFF),
                     static_cast<uint8_t>((currentPage >> 24) & 0xFF)};
  if (!ProgressFile::writeAtomic(cbz->getCachePath(), data, sizeof(data))) {
    LOG_ERR("CBR", "Failed to save progress: page %d", currentPage);
  }
}

void CbzReaderActivity::loadProgress() {
  if (!cbz) return;
  HalFile file;
  if (!Storage.openFileForRead("CBR", cbz->getCachePath() + "/progress.bin", file)) return;
  uint8_t data[4];
  if (file.read(data, sizeof(data)) != 4) return;
  currentPage = static_cast<int>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
  if (cbz->getPageCount() > 0 && currentPage >= cbz->getPageCount()) currentPage = cbz->getPageCount() - 1;
  if (currentPage < 0) currentPage = 0;
}

ScreenshotInfo CbzReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Cbz;
  if (cbz) {
    const std::string title = cbz->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", title.c_str());
    const int pageCount = cbz->getPageCount();
    info.totalPages = pageCount;
    const int page = pageCount > 0 ? std::clamp(currentPage, 0, pageCount - 1) : 0;
    info.progressPercent = pageCount > 0 ? (page + 1) * 100 / pageCount : 0;
    info.currentPage = page + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
