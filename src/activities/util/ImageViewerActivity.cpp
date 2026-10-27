#include "ImageViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* IMAGE_PREVIEW_PATH = "/.crosspoint/image_preview.bmp";
constexpr const char* TRANSPARENT_PREVIEW_PATH = "/.crosspoint/image_preview.transparent.bmp";
constexpr const char* SLEEP_IMAGE_PATH = "/sleep.bmp";
constexpr const char* SLEEP_IMAGE_PART_PATH = "/sleep.bmp.part";
constexpr const char* SLEEP_IMAGE_BACKUP_PATH = "/sleep.bmp.bak";

Rect sleepCoverActionRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect(metrics.contentSidePadding,
              renderer.getScreenHeight() - metrics.contentSidePadding - metrics.menuRowHeight,
              renderer.getScreenWidth() - metrics.contentSidePadding * 2, metrics.menuRowHeight);
}

// CPY viewer chrome (design: 圆底返回钮 / 角落页码 / 翻页按钮常显). Shared by
// the render passes and the touch routing so the two cannot drift.
constexpr int kCpyChromeInset = 32;
constexpr int kCpyBackSize = 48;
constexpr int kCpyPagerSize = 56;

Rect cpyBackRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect(kCpyChromeInset, metrics.topPadding + 8, kCpyBackSize, kCpyBackSize);
}

Rect cpyPagerPrevRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect(kCpyChromeInset, renderer.getScreenHeight() - metrics.contentSidePadding - metrics.menuRowHeight - 16 -
                                   kCpyPagerSize,
              kCpyPagerSize, kCpyPagerSize);
}

Rect cpyPagerNextRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect(renderer.getScreenWidth() - kCpyChromeInset - kCpyPagerSize,
              renderer.getScreenHeight() - metrics.contentSidePadding - metrics.menuRowHeight - 16 - kCpyPagerSize,
              kCpyPagerSize, kCpyPagerSize);
}
}  // namespace

ImageViewerActivity::ImageViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("ImageViewer", renderer, mappedInput), filePath(std::move(path)) {}

void ImageViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) return;

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string fname(name);
        if (FsHelpers::hasImageExtension(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
  }

  FsHelpers::sortFileList(siblingImages);

  const auto image = std::find(siblingImages.begin(), siblingImages.end(), fileName);
  if (image != siblingImages.end()) {
    currentImageIndex = static_cast<int>(image - siblingImages.begin());
  }
}

bool ImageViewerActivity::isPng() const { return FsHelpers::hasPngExtension(filePath); }

bool ImageViewerActivity::preparePreview() {
  if (!Storage.ensureDirectoryExists("/.crosspoint")) return false;
  if (Storage.exists(IMAGE_PREVIEW_PATH) && !Storage.remove(IMAGE_PREVIEW_PATH)) return false;

  bool prepared = false;
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    if (isPng()) {
      prepared = PngToBmpConverter::pngFileToBmpFile(filePath.c_str(), IMAGE_PREVIEW_PATH, true);
    } else {
      HalFile input, output;
      if (Storage.openFileForRead("IMAGE", filePath.c_str(), input) &&
          Storage.openFileForWrite("IMAGE", IMAGE_PREVIEW_PATH, output)) {
        prepared = JpegToBmpConverter::jpegFileToBmpStreamWithSize(input, output, renderer.getScreenWidth(),
                                                                   renderer.getScreenHeight(), /*crop=*/false);
        output.flush();
      }
    }
  }
  if (!prepared) Storage.remove(IMAGE_PREVIEW_PATH);
  return prepared;
}

void ImageViewerActivity::onEnter() {
  Activity::onEnter();
  imageReady = false;

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  const bool needsPreview = isPng() || FsHelpers::hasJpgExtension(filePath);
  const bool prepared = !needsPreview || preparePreview();
  const char* bitmapPath = needsPreview ? IMAGE_PREVIEW_PATH : filePath.c_str();
  HalFile file;
  // 1. Open the file
  if (!prepared) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_IMAGE_FILE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else if (Storage.openFileForRead("IMAGE", bitmapPath, file)) {
    Bitmap bitmap(file, true,
                  renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                      display.getController() == HalDisplay::Controller::SSD1677);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
        renderer.clearScreen();
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        return;
      }

      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      if (UITheme::getInstance().usesCpyLayout()) drawCpyChrome(hasPrevious, hasNext);
      if (mappedInput.hasTouch()) {
        GUI.drawActionButton(renderer, sleepCoverActionRect(renderer), tr(STR_SET_SLEEP_COVER));
      }
      if (bitmap.hasGreyscale()) {
        const bool absolute = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
        if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return;
        if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
        bool planesReady = true;
        for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
          if (bitmap.rewindToData() != BmpReaderError::Ok) {
            LOG_ERR("BMP", "Failed to rewind bitmap for grayscale rendering");
            planesReady = false;
            break;
          }
          renderer.clearScreen(absolute ? 0xFF : 0x00);
          renderer.setRenderMode(mode);
          if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
            planesReady = false;
            break;
          }
          GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
          if (UITheme::getInstance().usesCpyLayout()) drawCpyChrome(hasPrevious, hasNext);
          if (mappedInput.hasTouch())
            GUI.drawActionButton(renderer, sleepCoverActionRect(renderer), tr(STR_SET_SLEEP_COVER));
          if (mode == GfxRenderer::GRAYSCALE_LSB) {
            renderer.copyGrayscaleLsbBuffers();
          } else {
            renderer.copyGrayscaleMsbBuffers();
          }
        }
        if (planesReady) renderer.displayGrayBuffer();

        // Rebuild the BW framebuffer for popups and subsequent differential updates.
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.clearScreen();
        if (bitmap.rewindToData() != BmpReaderError::Ok ||
            !renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
          LOG_ERR("BMP", "Failed to rewind bitmap to restore the BW framebuffer");
          renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
          planesReady = false;
        }
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
        if (UITheme::getInstance().usesCpyLayout()) drawCpyChrome(hasPrevious, hasNext);
        renderer.cleanupGrayscaleWithFrameBuffer();
        imageReady = planesReady;
        if (!planesReady) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        imageReady = true;
      }

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_IMAGE_FILE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void ImageViewerActivity::drawCpyChrome(const bool hasPrevious, const bool hasNext) const {
  // Touch-only chrome: on keyed boards the button hints already carry these.
  if (!mappedInput.hasTouch()) return;

  // Round back button top-left (the round back language every CPY subpage
  // header uses).
  const Rect back = cpyBackRect(renderer);
  renderer.drawRoundedRect(back.x, back.y, back.width, back.height, 2, back.height / 2, true);
  {
    const int cx = back.x + back.width / 2;
    const int cy = back.y + back.height / 2;
    renderer.drawLine(cx + 5, cy - 9, cx - 4, cy, 3, true);
    renderer.drawLine(cx - 4, cy, cx + 5, cy + 9, 3, true);
  }

  // Corner page number when there are siblings to page through.
  if (siblingImages.size() > 1 && currentImageIndex >= 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", currentImageIndex + 1, static_cast<int>(siblingImages.size()));
    const int width = renderer.getTextWidth(UI_10_FONT_ID, buf);
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, renderer.getScreenWidth() - kCpyChromeInset - width,
                      back.y + (back.height - lineHeight) / 2, buf);
  }

  // Always-on pager buttons at the bottom corners; a side with nowhere to go
  // draws nothing (the design's persistent buttons exist so touch never has
  // to swipe, not to fake a disabled state).
  const auto drawChevron = [this](const Rect& box, const bool pointsLeft) {
    const int cx = box.x + box.width / 2;
    const int cy = box.y + box.height / 2;
    const int dx = pointsLeft ? -5 : 5;
    renderer.drawLine(cx - dx, cy - 11, cx + dx, cy, 3, true);
    renderer.drawLine(cx + dx, cy, cx - dx, cy + 11, 3, true);
  };
  if (hasPrevious) {
    const Rect p = cpyPagerPrevRect(renderer);
    renderer.drawRoundedRect(p.x, p.y, p.width, p.height, 2, 14, true);
    drawChevron(p, true);
  }
  if (hasNext) {
    const Rect p = cpyPagerNextRect(renderer);
    renderer.drawRoundedRect(p.x, p.y, p.width, p.height, 2, 14, true);
    drawChevron(p, false);
  }
}

void ImageViewerActivity::onExit() {
  Activity::onExit();
  imageReady = false;
  if (Storage.exists(IMAGE_PREVIEW_PATH)) Storage.remove(IMAGE_PREVIEW_PATH);
  if (Storage.exists(TRANSPARENT_PREVIEW_PATH)) Storage.remove(TRANSPARENT_PREVIEW_PATH);
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void ImageViewerActivity::doSetSleepCover(const char* sourcePath, const bool transparent) {
  if (!imageReady) return;
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  const char* preparedPath = sourcePath;
  bool success = true;
  if (transparent) {
    GfxRenderer::FrameBufferLoan loan(renderer);
    success = PngToBmpConverter::pngFileToTransparentBmpFile(sourcePath, TRANSPARENT_PREVIEW_PATH, true);
    if (success) preparedPath = TRANSPARENT_PREVIEW_PATH;
  }

  Storage.remove(SLEEP_IMAGE_PART_PATH);
  if (success) {
    HalFile inFile, outFile;
    if (Storage.openFileForRead("IMAGE", preparedPath, inFile) &&
        Storage.openFileForWrite("IMAGE", SLEEP_IMAGE_PART_PATH, outFile)) {
      const uint64_t expected = inFile.fileSize64();
      uint64_t copied = 0;
      char buffer[128];
      int bytesRead;
      while ((bytesRead = inFile.read(buffer, sizeof(buffer))) > 0) {
        if (outFile.write(buffer, bytesRead) != bytesRead) {
          success = false;
          break;
        }
        copied += bytesRead;
      }
      outFile.flush();
      success = success && copied == expected;
    } else {
      success = false;
    }
  }

  if (success) {
    Storage.remove(SLEEP_IMAGE_BACKUP_PATH);
    const bool hadPrevious = Storage.exists(SLEEP_IMAGE_PATH);
    if ((hadPrevious && !Storage.rename(SLEEP_IMAGE_PATH, SLEEP_IMAGE_BACKUP_PATH)) ||
        !Storage.rename(SLEEP_IMAGE_PART_PATH, SLEEP_IMAGE_PATH)) {
      if (hadPrevious && !Storage.exists(SLEEP_IMAGE_PATH)) {
        Storage.rename(SLEEP_IMAGE_BACKUP_PATH, SLEEP_IMAGE_PATH);
      }
      success = false;
    }
  }

  if (success) {
    const uint8_t previousMode = SETTINGS.sleepScreen;
    SETTINGS.sleepScreen = transparent ? CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT
                                       : CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    if (!SETTINGS.saveToFile()) {
      SETTINGS.sleepScreen = previousMode;
      (void)SETTINGS.saveToFile();
      Storage.remove(SLEEP_IMAGE_PATH);
      if (Storage.exists(SLEEP_IMAGE_BACKUP_PATH)) {
        Storage.rename(SLEEP_IMAGE_BACKUP_PATH, SLEEP_IMAGE_PATH);
      }
      success = false;
    }
  }

  Storage.remove(SLEEP_IMAGE_PART_PATH);
  if (success) Storage.remove(SLEEP_IMAGE_BACKUP_PATH);
  GUI.drawPopup(renderer, success ? tr(STR_DONE) : tr(STR_FAILED_LOWER));
  delay(1000);
}

void ImageViewerActivity::showSleepCoverOptions() {
  if (!imageReady) return;
  if (!isPng()) {
    doSetSleepCover(FsHelpers::hasJpgExtension(filePath) ? IMAGE_PREVIEW_PATH : filePath.c_str(), false);
    return;
  }

  static constexpr StrId options[] = {StrId::STR_NORMAL, StrId::STR_TRANSPARENT};
  static constexpr int optionCount = sizeof(options) / sizeof(options[0]);
  sleepCoverPopup.show(StrId::STR_SET_SLEEP_COVER, options, optionCount, 0, [this](const int index) {
    doSetSleepCover(index == 1 ? filePath.c_str() : IMAGE_PREVIEW_PATH, index == 1);
  });
  requestUpdate();
}

void ImageViewerActivity::render(RenderLock&&) { sleepCoverPopup.processRender(renderer, mappedInput); }

void ImageViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (sleepCoverPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    if (!sleepCoverPopup.isActive()) onEnter();
    return;
  }

  auto openSibling = [this](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }
    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }
    currentImageIndex = nextIndex;
    std::string dirPath = FsHelpers::extractFolderPath(filePath);
    if (dirPath.back() != '/') dirPath += "/";
    filePath = dirPath + siblingImages[currentImageIndex];
    onEnter();
    return true;
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  if (mappedInput.hasTouch()) {
    const Rect sleepCoverAction = sleepCoverActionRect(renderer);
    if (mappedInput.wasTapInRect(sleepCoverAction.x, sleepCoverAction.y, sleepCoverAction.width,
                                 sleepCoverAction.height)) {
      showSleepCoverOptions();
      return;
    }

    // CPY chrome taps: the round back button leaves to the browser exactly
    // like the Back key, and the persistent pager buttons step siblings.
    if (imageReady && UITheme::getInstance().usesCpyLayout()) {
      const Rect back = cpyBackRect(renderer);
      if (mappedInput.wasTapInRect(back.x, back.y, back.width, back.height)) {
        activityManager.goToFileBrowser(filePath);
        return;
      }
      const bool hasPrevious = siblingImages.size() > 1 && currentImageIndex > 0;
      const bool hasNext = siblingImages.size() > 1 && currentImageIndex != -1 &&
                           currentImageIndex < static_cast<int>(siblingImages.size()) - 1;
      if (hasPrevious) {
        const Rect p = cpyPagerPrevRect(renderer);
        if (mappedInput.wasTapInRect(p.x, p.y, p.width, p.height)) {
          openSibling(-1);
          return;
        }
      }
      if (hasNext) {
        const Rect p = cpyPagerNextRect(renderer);
        if (mappedInput.wasTapInRect(p.x, p.y, p.width, p.height)) {
          openSibling(1);
          return;
        }
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showSleepCoverOptions();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }
}
