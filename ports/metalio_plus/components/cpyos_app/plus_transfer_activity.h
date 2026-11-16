#pragma once

#include <string>

#include "activities/Activity.h"

enum class PlusTransferState { WIFI_SELECTION, NO_NETWORK, SERVER_STARTING, SERVER_RUNNING, ERROR };

/**
 * Native stand-in for CPYOS's web-server / Calibre pages on Metalio Plus.
 *
 * The port has no Arduino WebServer stack; instead this page starts the native
 * HTTP/WebDAV server (`web_transfer`) and the port-81 Calibre upload socket
 * (`ws_upload`), shows the address and live progress, and releases the radio on
 * exit. It is reached from the File Transfer tile and the network entry.
 *
 * The page also owns a SoftAP ("CrossPoint-Reader", open, channel 1, 4 clients)
 * so a device with no network in range can still be reached from a phone. The
 * hotspot is started and stopped from the action row and never outlives the
 * page: onExit() always drops the radio.
 */
class PlusTransferActivity final : public Activity {
 public:
  explicit PlusTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PlusTransfer", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == PlusTransferState::SERVER_RUNNING; }
  bool skipLoopDelay() override { return state == PlusTransferState::SERVER_RUNNING; }

 private:
  void onWifiResult(bool connected);
  void startServer();
  void startHotspot();
  void stopHotspot();
  void drawActionCapsules() const;
  void renderRunning() const;
  void renderNoNetwork() const;

  PlusTransferState state = PlusTransferState::WIFI_SELECTION;
  std::string ssid;
  std::string ip;
  unsigned long long lastReceived = 0;
  unsigned long long lastTotal = 0;
  unsigned int lastCompleted = 0;
  unsigned long lastCompleteAt = 0;
  unsigned long lastPollMs = 0;
  bool exitRequested = false;
  bool usbConfirm = false;
  unsigned long usbPromptAt = 0;
  bool usbFailed = false;
  bool hotspotOn = false;
  mutable Rect usbRect{};
  mutable Rect hotspotRect{};
};
