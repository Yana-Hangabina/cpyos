#include "UsbDownloadModeActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalSystem.h>
#include <Logging.h>

#include "I18n.h"
#include "fontIds.h"

void UsbDownloadModeActivity::onEnter() {
  Activity::onEnter();
  startedAt_ = millis();
  paint();
}

void UsbDownloadModeActivity::paint() {
  const int height = renderer.getScreenHeight();
  renderer.clearScreen();

  const int titleY = height * 28 / 100;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_USB_DOWNLOAD_MODE), true, EpdFontFamily::BOLD);
  const int line = renderer.getLineHeight(SMALL_FONT_ID) + 10;
  int y = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 40;
  renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_USB_DOWNLOAD_SCREEN_HINT), true, EpdFontFamily::REGULAR);
  y += line;
  renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_USB_DOWNLOAD_RESTART_HINT), true, EpdFontFamily::REGULAR);

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  painted_ = true;
}

void UsbDownloadModeActivity::loop() {
  if (switching_) return;
  // Give the user a chance to back out before the USB console disappears.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (millis() - startedAt_ >= 2500) {
    switching_ = true;
    LOG_INF("SETTINGS", "rebooting into USB download mode");
    if (!HalSystem::rebootToUsbDownload()) {
      switching_ = false;
      LOG_ERR("SETTINGS", "USB download mode is unsupported on this target");
    }
  }
}

void UsbDownloadModeActivity::render(RenderLock&& lock) {
  (void)lock;
  if (!painted_) paint();
}
