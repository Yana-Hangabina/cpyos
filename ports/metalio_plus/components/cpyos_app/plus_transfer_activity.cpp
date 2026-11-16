#include "plus_transfer_activity.h"

#include <memory>

#include "activities/Activity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>

#include "I18n.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "NetworkStartup.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "plus_transfer_hooks.h"
#include "util/QrUtils.h"

namespace {
constexpr unsigned long kPollMs = 500;
constexpr int kGap = 6;
constexpr unsigned long kCompleteHoldMs = 6000;
constexpr int kCapsuleH = 56;
constexpr int kCapsuleGap = 12;
}  // namespace

void PlusTransferActivity::onEnter() {
  Activity::onEnter();
  state = PlusTransferState::WIFI_SELECTION;
  ssid.clear();
  ip.clear();
  lastReceived = lastTotal = 0;
  lastCompleted = 0;
  lastCompleteAt = 0;
  lastPollMs = 0;
  exitRequested = false;
  usbConfirm = false;
  usbPromptAt = 0;
  usbFailed = false;
  hotspotOn = false;
  usbRect = Rect{};
  hotspotRect = Rect{};
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResultWith<WifiSelectionActivity>([this](const ActivityResult& result) {
      onWifiResult(!result.isCancelled);
    });
    return;
  }
  ssid = WiFi.SSID().c_str();
  ip = WiFi.localIP().toString().c_str();
  startServer();
}

void PlusTransferActivity::onWifiResult(bool connected) {
  if (!connected) {
    // No network is not a dead end: this page can raise its own hotspot, so
    // stay here instead of bouncing the user back to the previous screen.
    NetworkStartup::releaseRadio();
    ssid.clear();
    ip.clear();
    state = PlusTransferState::NO_NETWORK;
    requestUpdate();
    return;
  }
  ssid = WiFi.SSID().c_str();
  ip = WiFi.localIP().toString().c_str();
  startServer();
}

void PlusTransferActivity::startServer() {
  state = PlusTransferState::SERVER_STARTING;
  requestUpdate();
  if (plus_transfer::start()) {
    state = PlusTransferState::SERVER_RUNNING;
  } else {
    state = PlusTransferState::ERROR;
  }
  requestUpdate();
}

void PlusTransferActivity::startHotspot() {
  if (hotspotOn) return;
  // S31 SoftAP is not usable in the full shell image: the precompiled Wi-Fi
  // library allocates a 752-byte beacon event buffer that does not fit next to
  // the panel queues (alloc eb len=752 type=4 fail -> Load access fault), and it
  // faults instead of returning an error. Refuse before touching the driver;
  // STA transfers and the USB-drive path still work.
  LOG_ERR("XFER", "hotspot unavailable on this image (S31 SoftAP beacon pool); use a Wi-Fi network or USB drive");
  usbFailed = false;
  state = PlusTransferState::ERROR;
  requestUpdate();
  return;
}

void PlusTransferActivity::stopHotspot() {
  if (!hotspotOn) return;
  hotspotOn = false;
  // Keep a joined station alive: only the AP interface goes away.
  WiFi.softAPdisconnect(false);
  if (WiFi.status() == WL_CONNECTED) {
    ssid = WiFi.SSID().c_str();
    ip = WiFi.localIP().toString().c_str();
    LOG_INF("XFER", "hotspot down; serving on the joined network %s", ip.c_str());
    return;
  }
  LOG_INF("XFER", "hotspot down; no network left, stopping the server");
  plus_transfer::stop();
  ssid.clear();
  ip.clear();
  state = PlusTransferState::NO_NETWORK;
}

void PlusTransferActivity::onExit() {
  Activity::onExit();
  plus_transfer::stop();
  // Drops the AP as well as any station (esp_wifi_deinit), so the hotspot never
  // outlives the page.
  NetworkStartup::releaseRadio();
}

void PlusTransferActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitRequested = true;
  }

  const bool actionsVisible =
      state == PlusTransferState::SERVER_RUNNING || state == PlusTransferState::NO_NETWORK;

  if (state == PlusTransferState::SERVER_RUNNING) {
    const unsigned long now = millis();
    if (now - lastPollMs >= kPollMs) {
      lastPollMs = now;
      const auto status = plus_transfer::status();
      bool changed = false;
      if (status.received != lastReceived || status.total != lastTotal) {
        lastReceived = status.received;
        lastTotal = status.total;
        changed = true;
      }
      if (status.completed != lastCompleted) {
        lastCompleted = status.completed;
        lastCompleteAt = now;
        changed = true;
      }
      if (lastCompleteAt > 0 && now - lastCompleteAt >= kCompleteHoldMs) {
        lastCompleteAt = 0;
        changed = true;
      }
      if (changed) requestUpdate();
    }
  }

  if (actionsVisible && hotspotRect.width > 0 &&
      mappedInput.wasTapInRect(hotspotRect.x, hotspotRect.y, hotspotRect.width, hotspotRect.height)) {
    if (hotspotOn) {
      stopHotspot();
    } else {
      startHotspot();
    }
    requestUpdate();
  }

  if (actionsVisible && usbRect.width > 0 &&
      mappedInput.wasTapInRect(usbRect.x, usbRect.y, usbRect.width, usbRect.height)) {
    if (!usbConfirm) {
      usbConfirm = true;
      usbPromptAt = millis();
      requestUpdate();
    } else if (millis() - usbPromptAt < 8000) {
      if (!plus_transfer::usbStart()) {
        usbFailed = true;
        state = PlusTransferState::ERROR;
      }
      requestUpdate();
    }
  }
  if (usbConfirm && millis() - usbPromptAt >= 8000) {
    usbConfirm = false;
    requestUpdate();
  }

  if (exitRequested) {
    finish();
  }
}

// The hotspot and USB actions share one row of two capsules at the bottom of
// the page. Both rects are (re)assigned here so the tap targets always match
// what was drawn last.
void PlusTransferActivity::drawActionCapsules() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - kGap;
  const int y = contentBottom - kCapsuleH;
  const int side = metrics.contentSidePadding;
  const int total = pageWidth - side * 2;
  const int capsuleW = (total - kCapsuleGap) / 2;
  const int textH = renderer.getLineHeight(UI_10_FONT_ID);

  hotspotRect = Rect{side, y, capsuleW, kCapsuleH};
  renderer.drawRoundedRect(hotspotRect.x, hotspotRect.y, hotspotRect.width, hotspotRect.height, 2,
                           kCapsuleH / 2, true);
  const char* hotspotLabel = hotspotOn ? tr(STR_WIFI_OFF) : tr(STR_CREATE_HOTSPOT);
  const int hotspotTextW = renderer.getTextWidth(UI_10_FONT_ID, hotspotLabel, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, hotspotRect.x + (hotspotRect.width - hotspotTextW) / 2,
                    y + (kCapsuleH - textH) / 2, hotspotLabel, true, EpdFontFamily::BOLD);

  usbRect = Rect{side + capsuleW + kCapsuleGap, y, total - capsuleW - kCapsuleGap, kCapsuleH};
  renderer.drawRoundedRect(usbRect.x, usbRect.y, usbRect.width, usbRect.height, 2, kCapsuleH / 2, true);
  const char* usbLabel = usbConfirm ? tr(STR_CONFIRM) : tr(STR_USB_DRIVE);
  const int usbTextW = renderer.getTextWidth(UI_10_FONT_ID, usbLabel, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, usbRect.x + (usbRect.width - usbTextW) / 2, y + (kCapsuleH - textH) / 2,
                    usbLabel, true, EpdFontFamily::BOLD);
}

void PlusTransferActivity::renderRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int subHeaderY = metrics.topPadding + metrics.headerHeight;
  const int subHeaderBottom = subHeaderY + metrics.tabBarHeight;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - kGap;
  const int clipBottom = contentBottom - kCapsuleH - 10;

  // When the hotspot is up the sub-header carries the AP's SSID and address:
  // that is the pair the user has to type on the phone.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FILE_TRANSFER));
  GUI.drawSubHeader(renderer, Rect{0, subHeaderY, pageWidth, metrics.tabBarHeight}, ssid.c_str(),
                    (std::string(tr(STR_IP_ADDRESS_PREFIX)) + ip).c_str());

  {
    const GfxRenderer::ClipScope clip(renderer, 0, subHeaderBottom, pageWidth,
                                      std::max(0, clipBottom - subHeaderBottom));
    const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
    const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
    /* STA mode: the same scan-to-open QR the shared web-server page draws. The
     * hotspot path cannot start on this image (S31 SoftAP beacon pool), so an
     * AP-join code would never be shown. */
    const bool showUrlQr =
        !hotspotOn && !ip.empty() && state == PlusTransferState::SERVER_RUNNING;
    const int qrSize = 180;
    const int qrGap = 16;
    const int qrX = pageWidth - metrics.contentSidePadding - qrSize;
    const int qrY = subHeaderBottom + kGap;
    const int textWidth =
        pageWidth - metrics.contentSidePadding * 2 - (showUrlQr ? qrSize + qrGap : 0);
    int y = subHeaderBottom + kGap;

    if (hotspotOn) {
      renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_HOTSPOT_MODE), true,
                        EpdFontFamily::BOLD);
      y += line12 + kGap;
    }

    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_SETUP), true,
                      EpdFontFamily::BOLD);
    y += line12 + kGap;

    const char* instructions[] = {tr(STR_CALIBRE_INSTRUCTION_1), tr(STR_CALIBRE_INSTRUCTION_2),
                                  tr(STR_CALIBRE_INSTRUCTION_3), tr(STR_CALIBRE_INSTRUCTION_4)};
    for (const char* instruction : instructions) {
      const std::string line =
          showUrlQr ? renderer.truncatedText(UI_10_FONT_ID, instruction, textWidth, EpdFontFamily::REGULAR)
                    : std::string(instruction);
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.c_str());
      y += line10 + kGap;
    }

    y += kGap;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_STATUS), true,
                      EpdFontFamily::BOLD);
    y += line12 + kGap;

    if (lastTotal > 0 && lastReceived <= lastTotal) {
      std::string label = tr(STR_CALIBRE_RECEIVING);
      label = renderer.truncatedText(UI_10_FONT_ID, label.c_str(), textWidth, EpdFontFamily::REGULAR);
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, label.c_str());
      GUI.drawProgressBar(
          renderer,
          Rect{metrics.contentSidePadding, y + line10 + kGap, textWidth, metrics.progressBarHeight},
          static_cast<size_t>(lastReceived), static_cast<size_t>(lastTotal));
    } else if (lastCompleteAt > 0 && millis() - lastCompleteAt < kCompleteHoldMs) {
      std::string msg = tr(STR_CALIBRE_RECEIVED);
      msg = renderer.truncatedText(UI_10_FONT_ID, msg.c_str(), textWidth, EpdFontFamily::REGULAR);
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, msg.c_str());
    }

    if (showUrlQr) {
      const std::string webInfo = std::string("http://") + ip + "/";
      QrUtils::drawQrCode(renderer, Rect{qrX, qrY, qrSize, qrSize}, webInfo);
      const std::string hint =
          renderer.truncatedText(UI_10_FONT_ID, tr(STR_SCAN_QR_HINT), qrSize + 24);
      const int hintW = renderer.getTextWidth(UI_10_FONT_ID, hint.c_str());
      renderer.drawText(UI_10_FONT_ID, qrX + (qrSize - hintW) / 2, qrY + qrSize + 6, hint.c_str());
    }
  }

  drawActionCapsules();

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Reached when the user cancels the Wi-Fi picker: the page stays so the hotspot
// can be raised from here.
void PlusTransferActivity::renderNoNetwork() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int clipBottom = pageHeight - metrics.buttonHintsHeight - kGap - kCapsuleH - 10;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FILE_TRANSFER));

  {
    const int top = metrics.topPadding + metrics.headerHeight;
    const GfxRenderer::ClipScope clip(renderer, 0, top, pageWidth, std::max(0, clipBottom - top));
    const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
    const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
    const int centre = (top + clipBottom) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, centre - line12, tr(STR_CONNECTION_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, centre + line10 / 2, tr(STR_HOTSPOT_DESC));
  }

  drawActionCapsules();

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PlusTransferActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();

  if (state == PlusTransferState::SERVER_RUNNING) {
    renderRunning();
  } else if (state == PlusTransferState::NO_NETWORK) {
    renderNoNetwork();
  } else {
    // No action row in these states: clear the tap targets so a stale rect from
    // the previous frame cannot be hit.
    usbRect = Rect{};
    hotspotRect = Rect{};
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   tr(STR_FILE_TRANSFER));
    const int centre = renderer.getScreenHeight() / 2;
    if (state == PlusTransferState::ERROR) {
      renderer.drawCenteredText(UI_12_FONT_ID, centre,
                                usbFailed ? tr(STR_USB_DRIVE_START_ERROR) : tr(STR_CONNECTION_FAILED), true,
                                EpdFontFamily::BOLD);
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, centre, tr(STR_CALIBRE_STARTING));
    }
  }
  renderer.displayBuffer();
}
