#include "plus_camera_activity.h"

#include <esp_err.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <ctime>

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>

#include "Epub/converters/JpegToFramebufferConverter.h"
#include "components/StatusCards.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "plus_camera.h"

namespace {

// Card root, next to the other CPYOS content folders. No i18n key is added for
// the location: the page prints the concrete path instead.
constexpr const char* kPhotoDir = "/photos";
constexpr int kCaptureWidth = 640;
constexpr int kCaptureHeight = 480;

}  // namespace

void PlusCameraActivity::onEnter() {
  Activity::onEnter();
  state_ = State::Probing;
  detail_.clear();
  savedBytes_ = 0;
  forceFullNext_ = true;
  /* One probe per entry. It already does the LCD_CAM hand-off, which is why it
   * runs under the render lock and why the first frame below is a full sweep. */
  probeSensor();
  requestUpdate();
}

void PlusCameraActivity::probeSensor() {
  RenderLock lock;
  // Documents the contract: no panel push may be in flight while the camera
  // owns LCD_CAM. On this port pushes are synchronous, so this is a no-op guard
  // for the async boards (and for a future async path here).
  display.waitForScanIdle(3000);
  const esp_err_t err = plus_camera_probe();
  pid_ = plus_camera_last_pid();
  if (err == ESP_OK) {
    state_ = State::Ready;
    LOG_INF("CAM", "GC2145 ready (PID 0x%04x)", static_cast<unsigned>(pid_));
    return;
  }
  state_ = State::Unavailable;
  detail_ = esp_err_to_name(err);
  LOG_ERR("CAM", "probe failed: %s", esp_err_to_name(err));
}

void PlusCameraActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }
  if (state_ == State::Capturing) return;

  bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (!confirm && canCapture()) {
    // The page has no other control, so a tap anywhere is the shutter.
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasScreenTapped(tapX, tapY)) {
      confirm = true;
    }
  }
  if (confirm && canCapture()) capturePhoto();
}

void PlusCameraActivity::capturePhoto() {
  state_ = State::Capturing;

  uint8_t* jpeg = nullptr;
  size_t length = 0;
  esp_err_t err = ESP_FAIL;
  {
    /* Take the panel first: the wait card draws and pushes, and the render task
     * must not be half-way through a frame while that happens. Holding the lock
     * also guarantees no push can start once the camera owns LCD_CAM. */
    RenderLock lock;
    display.waitForScanIdle(3000);
    status_card::showWait(renderer, tr(STR_LOADING_POPUP), nullptr, nullptr, -1, false, millis());
    // The wait card is itself a push: let it finish before LCD_CAM is dismantled.
    display.waitForScanIdle(3000);
    err = plus_camera_capture_jpeg(&jpeg, &length);
  }
  // The panel was dismantled and rebuilt around the capture: the frame drawn
  // below has to be a full sweep, whatever happened in between.
  forceFullNext_ = true;

  bool saved = false;
  if (err == ESP_OK && jpeg != nullptr && length > 0) {
    saved = saveJpeg(jpeg, length);
  } else if (err == ESP_OK) {
    detail_ = "empty frame";
  } else {
    detail_ = esp_err_to_name(err);
  }
  if (jpeg != nullptr) heap_caps_free(jpeg);

  if (saved) {
    state_ = State::Shot;
  } else {
    // Back to a state that still owns a working panel and can retry.
    state_ = State::Failed;
    LOG_ERR("CAM", "capture failed: %s", detail_.c_str());
  }
  requestUpdate();
}

bool PlusCameraActivity::saveJpeg(const uint8_t* data, const size_t len) {
  if (!Storage.ensureDirectoryExists(kPhotoDir)) {
    detail_ = "mkdir " + std::string(kPhotoDir);
    return false;
  }
  /* IMG_<epoch>.jpg. The clock is the name, so a photo sorts with the others;
   * +1 bumps keep names unique when the RTC has not been set yet (the epoch
   * then starts at 0 for every shot). */
  const long now = static_cast<long>(time(nullptr));
  for (int bump = 0; bump < 1000; ++bump) {
    char path[64];
    std::snprintf(path, sizeof(path), "%s/IMG_%ld.jpg", kPhotoDir, now + bump);
    if (Storage.exists(path)) continue;
    HalFile file;
    if (!Storage.openFileForWrite("CAM", path, file)) continue;
    const size_t written = file.write(data, len);
    file.flush();
    file.close();
    if (written != len) {
      Storage.remove(path);
      LOG_ERR("CAM", "short write to %s (%u/%u)", path, static_cast<unsigned>(written),
              static_cast<unsigned>(len));
      continue;
    }
    detail_ = path;
    savedBytes_ = len;
    LOG_INF("CAM", "saved %s (%u bytes)", path, static_cast<unsigned>(len));
    return true;
  }
  detail_ = "no free name under " + std::string(kPhotoDir);
  return false;
}

void PlusCameraActivity::onExit() {
  Activity::onExit();
  // Never leave LCD_CAM half-owned: if a capture was torn down mid-way this
  // re-attaches the panel so the next screen can push.
  plus_camera_release();
  // The photo page was a 16-level frame; the parent redraws differentially on
  // top of it, so start from a clean sheet.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

Rect PlusCameraActivity::photoBounds() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int left = metrics.contentSidePadding;
  const int maxW = screenW - left * 2;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  // Caption line plus one row of button hints under the picture.
  const int bottom = screenH - metrics.contentSidePadding - metrics.menuRowHeight -
                     renderer.getLineHeight(SMALL_FONT_ID) - 16;
  const int maxH = bottom - top;
  if (maxW <= 0 || maxH <= 0) return Rect{0, 0, 0, 0};
  int width = maxW;
  int height = width * kCaptureHeight / kCaptureWidth;
  if (height > maxH) {
    height = maxH;
    width = height * kCaptureWidth / kCaptureHeight;
  }
  return Rect{left + (maxW - width) / 2, top + (maxH - height) / 2, width, height};
}

bool PlusCameraActivity::drawPhotoPage() {
  if (renderer.getGrayscaleLevels() != 16) return false;
  if (detail_.empty()) return false;
  const Rect bounds = photoBounds();
  if (bounds.width <= 0 || bounds.height <= 0) return false;
  if (!renderer.beginGrayscale16()) return false;

  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_IMAGES));

  JpegToFramebufferConverter converter;
  RenderConfig config;
  config.x = bounds.x;
  config.y = bounds.y;
  config.maxWidth = bounds.width;
  config.maxHeight = bounds.height;
  config.useExactDimensions = true;  // the photo is already 4:3 inside the box
  config.useDithering = false;
  config.output = DecodeOutput::NativeGrayscale16;
  ImageRenderError decodeError = ImageRenderError::None;
  config.error = &decodeError;
  const bool decoded = converter.decodeToFramebuffer(detail_, renderer, config);

  if (decoded) {
    const std::string caption =
        renderer.truncatedText(SMALL_FONT_ID, detail_.c_str(), bounds.width);
    renderer.drawText(SMALL_FONT_ID, bounds.x, bounds.y + bounds.height + 8, caption.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    // Every capture rebuilt LCD_CAM, so this frame is a GC16 sweep: the panel
    // was detached while the camera ran and must be driven from scratch.
    renderer.commitGrayscale16(forceFullNext_);
    return true;
  }

  LOG_ERR("CAM", "cannot decode %s (err %d)", detail_.c_str(), static_cast<int>(decodeError));
  renderer.cancelGrayscale16();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  return false;
}

void PlusCameraActivity::drawStatusPage() {
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_IMAGES));

  const int left = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - left * 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  const char* headline = nullptr;
  std::string body;
  switch (state_) {
    case State::Probing:
      headline = tr(STR_LOADING_POPUP);
      break;
    case State::Capturing:
      headline = tr(STR_LOADING_POPUP);
      break;
    case State::Ready: {
      headline = "GC2145";
      char identity[64];
      std::snprintf(identity, sizeof(identity), "PID 0x%04x   %dx%d   JPEG q80", static_cast<unsigned>(pid_),
                    kCaptureWidth, kCaptureHeight);
      body = identity;
      break;
    }
    case State::Unavailable:
      // Probe failed: no sensor, or something that is not a GC2145 answered.
      headline = tr(STR_NOT_AVAILABLE);
      body = detail_;
      break;
    case State::Failed:
      headline = tr(STR_FAILED_LOWER);
      body = detail_;
      break;
    case State::Shot:
      // Only reachable when the photo could not be decoded for preview.
      headline = tr(STR_DONE);
      body = detail_;
      break;
  }

  if (headline != nullptr) {
    renderer.drawText(UI_10_FONT_ID, left, y, headline, true, EpdFontFamily::BOLD);
    y += lineH + 12;
  }
  if (!body.empty()) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, body.c_str(), width, 4, EpdFontFamily::REGULAR);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
      y += lineH + 4;
    }
  }
  if (state_ == State::Shot && savedBytes_ > 0) {
    char size[32];
    std::snprintf(size, sizeof(size), "%u bytes", static_cast<unsigned>(savedBytes_));
    renderer.drawText(SMALL_FONT_ID, left, y + 4, size);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canCapture() ? tr(STR_CONFIRM) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PlusCameraActivity::render(RenderLock&&) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  if (state_ == State::Shot && drawPhotoPage()) {
    forceFullNext_ = false;
    return;
  }
  drawStatusPage();
  renderer.displayBuffer(forceFullNext_ ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
  forceFullNext_ = false;
}
