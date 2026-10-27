#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "NetworkStartup.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "activities/settings/BluetoothSettingsActivity.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/icons/listIcons48.h"
#include "fontIds.h"
#include "util/PluginEvents.h"
#include "util/QrUtils.h"
#include "util/TaskWatchdog.h"

#include "CrossPointSettings.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

void stopDnsServer() {
  if (!dnsServer) return;

  dnsServer->stop();
  delete dnsServer;
  dnsServer = nullptr;
}

void restartMdns(const char* hostname, const char* tag) {
  MDNS.end();
  if (MDNS.begin(hostname)) {
    LOG_DBG(tag, "mDNS started: http://%s.local/", hostname);
  } else {
    LOG_DBG(tag, "WARNING: mDNS failed to start");
  }
}

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}

// Draws a Mask1 freeink icon at an arbitrary pixel size (nearest-neighbour, so
// the 48 px Lucide strokes stay crisp at the sizes the page asks for).
void drawMaskIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const int size) {
  if (size <= 0 || icon.w <= 0 || icon.h <= 0) return;
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

// Globe: the design's transfer-card mark. Neither the 48 px icon set nor
// UIIcon has one, so it is stroked here (meridian + equator + outline).
void drawGlobeIcon(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int radius = std::max(4, size / 2 - 2);
  const int cx = x + size / 2;
  const int cy = y + size / 2;
  constexpr float kTwoPi = 6.2831853f;
  constexpr int kSteps = 48;
  for (int i = 0; i <= kSteps; ++i) {
    const float angle = static_cast<float>(i) * kTwoPi / kSteps;
    const int px = cx + static_cast<int>(std::lround(static_cast<float>(radius) * std::cos(angle)));
    const int py = cy + static_cast<int>(std::lround(static_cast<float>(radius) * std::sin(angle)));
    renderer.drawPixel(px, py, true);
  }
  // Meridian: a vertical ellipse through the poles.
  const int meridianW = std::max(2, radius * 42 / 100);
  for (int i = 0; i <= kSteps; ++i) {
    const float angle = static_cast<float>(i) * kTwoPi / kSteps;
    const int px = cx + static_cast<int>(std::lround(static_cast<float>(meridianW) * std::sin(angle)));
    const int py = cy - static_cast<int>(std::lround(static_cast<float>(radius) * std::cos(angle)));
    renderer.drawPixel(px, py, true);
  }
  renderer.drawLine(cx - radius, cy, cx + radius, cy, 1, true);
}

// 52x38 signal bars for the current-network card (design frame 14).
void drawHubSignalBars(const GfxRenderer& renderer, const int x, const int y, const int bars) {
  constexpr int kWidth = 52;
  constexpr int kHeight = 38;
  constexpr int kGap = 4;
  constexpr int kHeights[4] = {12, 18, 25, 32};
  const int barW = std::max(4, (kWidth - kGap * 3) / 4);
  for (int i = 0; i < 4; ++i) {
    const int barH = kHeights[i];
    const int bx = x + i * (barW + kGap);
    const int by = y + kHeight - barH;
    renderer.drawRect(bx, by, barW, barH, true);
    if (i < bars) renderer.fillRect(bx + 2, by + 2, std::max(1, barW - 4), std::max(1, barH - 3), true);
  }
}
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();
  NetworkStartup::logMemory("CrossPointWebServerActivity enter");

  LOG_DBG("WEBACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Heap-critical transition: WiFi (~45KB) plus the web server have to fit in
  // what's left of the ~380KB parts. SD-font caches retained for the CJK UI
  // fallback (mini glyph/kern arenas, kern class tables) are rebuildable on
  // demand — release them up front instead of aborting in startWebServer()
  // when the heap comes up short (observed on X3 with a Korean SD font).
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
    LOG_DBG("WEBACT", "Free heap after SD font cache release: %d bytes", ESP.getFreeHeap());
  }

  // Reset state
  state = WebServerActivityState::TRANSFER_PAGE;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  lastWifiBars = 0;
  {
    // The "saved networks" row shows a real count; SD access needs the render
    // lock because drawing also uses SPI.
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }
  savedNetworkCount = WIFI_STORE.getCredentialCount();
  requestUpdate();

  if (startInJoinNetwork) {
    // Came back from the heap-defrag reboot on a pristine heap: skip the mode
    // picker and go straight into Join Network (onNetworkModeSelected won't
    // reboot again while this flag is set).
    onNetworkModeSelected(NetworkMode::JOIN_NETWORK);
    return;
  }
}

void CrossPointWebServerActivity::onExit() {
  Activity::onExit();
  NetworkStartup::logMemory("CrossPointWebServerActivity exit begin");

  LOG_DBG("WEBACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  state = WebServerActivityState::SHUTTING_DOWN;
  stopDnsServer();
  MDNS.end();

  // Web uploads may have installed or removed plugins. Non-touch devices
  // reboot below (setup() re-reads everything), but touch devices end the
  // session in place, so re-read event subscriptions here.
  pluginevents::refreshSubscriptions();

  // Skip reboot if WiFi was never activated (e.g. user backed out of mode selection).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (isApMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(30);
    NetworkStartup::logMemory("web exit before restart");
    silentRestart();
  }

  LOG_DBG("WEBACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  // Join Network brings up WiFi + the web server + TLS relays, whose working set
  // needs a large *contiguous* block. After reading/browsing the heap is
  // fragmented enough that tight boards (X3) abort mid-activation. Reboot once
  // into a pristine heap and land straight back here; the flag prevents a second
  // reboot on that return. No-op on touch boards, which fall through.
  if (mode == NetworkMode::JOIN_NETWORK && !startInJoinNetwork) {
    silentRestartToJoinNetwork();  // does not return on non-touch boards
  }

  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
#if FREEINK_CAP_USB_MSC
  } else if (mode == NetworkMode::USB_DRIVE) {
    modeName = "USB Drive";
#endif
  }
  LOG_DBG("WEBACT", "Network mode selected: %s", modeName);

#if FREEINK_CAP_USB_MSC
  if (mode == NetworkMode::USB_DRIVE) {
    activityManager.goToUsbDrive();
    return;
  }
#endif

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    startActivityForResultWith<CalibreConnectActivity>([this](const ActivityResult& result) {
      (void)result;
      state = WebServerActivityState::MODE_SELECTION;
      requestUpdate();

      startActivityForResultWith<NetworkModeSelectionActivity>([this](const ActivityResult& nested) {
        if (nested.isCancelled) {
          state = WebServerActivityState::TRANSFER_PAGE;
          savedNetworkCount = WIFI_STORE.getCredentialCount();
          requestUpdate();
        } else {
          onNetworkModeSelected(std::get<NetworkModeResult>(nested.data).mode);
        }
      });
    });
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    state = WebServerActivityState::WIFI_SELECTION;
    LOG_DBG("WEBACT", "Launching WifiSelectionActivity...");
    if (!startActivityForResultWith<WifiSelectionActivity>([this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& wifi = std::get<WifiResult>(result.data);
            connectedIP = wifi.ip;
            connectedSSID = wifi.ssid;
          }
          onWifiSelectionComplete(!result.isCancelled);
        })) {
      onGoHome();
    }
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  LOG_DBG("WEBACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    // Get connection info before exiting subactivity
    isApMode = false;

    // Start mDNS for hostname resolution
    restartMdns(AP_HOSTNAME, "WEBACT");

    // Start the web server
    startWebServer();
  } else {
    // Back on the transfer page: the user can pick another mode or leave.
    state = WebServerActivityState::TRANSFER_PAGE;
    savedNetworkCount = WIFI_STORE.getCredentialCount();
    requestUpdate();
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Starting Access Point mode...");
  LOG_DBG("WEBACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  // Configure and start the AP
  if (!NetworkStartup::setMode(renderer, WIFI_AP)) {
    LOG_ERR("WEBACT", "Failed to initialize WiFi for Access Point");
    onGoHome();
    return;
  }
  delay(100);

  // Start soft AP
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    onGoHome();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  LOG_DBG("WEBACT", "Access Point started!");
  LOG_DBG("WEBACT", "SSID: %s", AP_SSID);
  LOG_DBG("WEBACT", "IP: %s", connectedIP.c_str());

  // Start mDNS for hostname resolution
  restartMdns(AP_HOSTNAME, "WEBACT");

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  stopDnsServer();
  dnsServer = new (std::nothrow) DNSServer();
  if (!dnsServer) {
    LOG_ERR("WEBACT", "OOM: DNSServer (%u bytes)", static_cast<unsigned>(sizeof(DNSServer)));
    onGoHome();
    return;
  }
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);
  LOG_DBG("WEBACT", "DNS server started for captive portal");

  LOG_DBG("WEBACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  LOG_DBG("WEBACT", "Starting web server...");

  // Repeat the release right before the allocation: the WiFi selection screen
  // rendered since onEnter(), and a CJK SSID repopulates the SD-font caches.
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("WEBACT", "Free heap before SD font cache release: %d bytes", ESP.getFreeHeap());
    fcm->releaseSdFontCaches();
    LOG_DBG("WEBACT", "Free heap before server alloc: %d bytes", ESP.getFreeHeap());
  }

  // Create the web server instance
  webServer = makeUniqueNoThrow<CrossPointWebServer>();
  if (!webServer) {
    LOG_ERR("WEBACT", "OOM: CrossPointWebServer (%u bytes)", static_cast<unsigned>(sizeof(CrossPointWebServer)));
    onGoHome();
    return;
  }
  // An upload holds loop() inside handleClient(), so input is sampled on each received chunk instead. On a slow
  // link chunks come ~0.5 s apart, so a button found down gets a second sample past the SDK's 5 ms debounce.
  webServer->setUploadCancelCheck([this] {
    mappedInput.update(true);
    if (gpio.rawInputActive()) {
      delay(6);
      mappedInput.update(true);
    }
    leaveRequested = leaveRequested || mappedInput.isPressed(MappedInputManager::Button::Back) ||
                     mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture();
    return leaveRequested;
  });
  const bool started = webServer->begin();
  NetworkStartup::logMemory(started ? "web services ready" : "web services failed");
  if (started) {
    state = WebServerActivityState::SERVER_RUNNING;
    LOG_DBG("WEBACT", "Web server started successfully");
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);

    // The device is online: deliver queued plugin events through their
    // manifest handlers. STA only (AP mode has no internet route); bounded by
    // the drain's per-call event budget so serving is not noticeably delayed.
    if (!isApMode) {
      pluginevents::drain(&renderer);
    }

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    onGoHome();
  }
}

void CrossPointWebServerActivity::loop() {
  if (state == WebServerActivityState::TRANSFER_PAGE) {
    handleTransferPageInput();
    return;
  }

  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // The rows stay live while serving: network mode / saved networks tear the
    // server down before leaving, Bluetooth does not touch the link.
    handleTransferPageInput();
    if (state != WebServerActivityState::SERVER_RUNNING) return;
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        // Driver auto-reconnect handles retries; abandon (via onGoHome) only
        // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
        bool repaint = false;
        if (wifiStatus != WL_CONNECTED) {
          if (consecutiveDisconnects == 0) {
            firstDisconnectAt = millis();
            repaint = true;
          }
          consecutiveDisconnects++;
          LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
                  consecutiveDisconnects, millis() - firstDisconnectAt);
          if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
            LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
            state = WebServerActivityState::SHUTTING_DOWN;
            onGoHome();
            return;
          }
        } else {
          if (consecutiveDisconnects > 0) {
            LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
                    millis() - firstDisconnectAt);
            repaint = true;
          }
          consecutiveDisconnects = 0;
          firstDisconnectAt = 0;
          const int rssi = WiFi.RSSI();
          if (rssi < -75) {
            LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
          }
          const int bars = barsForRssi(rssi, lastWifiBars);
          if (bars != lastWifiBars) {
            lastWifiBars = bars;
            repaint = true;
          }
        }
        if (repaint) requestUpdate();
      }
    }

    // Handle web server requests - maximize throughput with watchdog safety
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        LOG_DBG("WEBACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
      }

      // Reset watchdog BEFORE processing - HTTP header parsing can be slow
      resetTaskWatchdogIfSubscribed();

      // Process HTTP requests in tight loop for maximum throughput
      // More iterations = more data processed per main loop cycle
      constexpr int MAX_ITERATIONS = 500;
      for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
        webServer->handleClient();
        if (leaveRequested) {
          onGoHome();
          return;
        }
        // Reset watchdog every 32 iterations
        if ((i & 0x1F) == 0x1F) {
          resetTaskWatchdogIfSubscribed();
        }
        // Yield and check for exit button every 64 iterations
        if ((i & 0x3F) == 0x3F) {
          yield();
          // Pump input inside this blocking loop so exit events remain responsive.
          mappedInput.update(true);
          // Home remains available now; other configured actions are deferred
          // to the next main-loop pass.
          if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
            onGoHome();
            return;
          }
        }
      }
      lastHandleClientTime = millis();
    }

    // Also check outside the request-processing loop.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
      onGoHome();
      return;
    }
  }
}

const char* CrossPointWebServerActivity::headerTitle() const {
  if (isApMode) return tr(STR_HOTSPOT_MODE);
  return tr(STR_NETWORK_TRANSFER);
}


void CrossPointWebServerActivity::stopNetworkingForNavigation() {
  // The render task checks webServer/isRunning(); stop it under the render
  // lock so it never observes a half-destroyed server.
  RenderLock lock(*this);
  if (webServer) {
    webServer->stop();
    webServer.reset();
  }
  stopDnsServer();
  MDNS.end();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (isApMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(20);
  }
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  consecutiveDisconnects = 0;
  firstDisconnectAt = 0;
  modeRow_ = Rect{};
  savedRow_ = Rect{};
  bluetoothRow_ = Rect{};
}

void CrossPointWebServerActivity::openModeSelection() {
  stopNetworkingForNavigation();
  state = WebServerActivityState::MODE_SELECTION;
  savedNetworkCount = WIFI_STORE.getCredentialCount();
  requestUpdate();
  if (!startActivityForResultWith<NetworkModeSelectionActivity>([this](const ActivityResult& result) {
        if (result.isCancelled) {
          state = WebServerActivityState::TRANSFER_PAGE;
          requestUpdate();
          return;
        }
        onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
      })) {
    state = WebServerActivityState::TRANSFER_PAGE;
    requestUpdate();
  }
}

void CrossPointWebServerActivity::openSavedNetworks() {
  stopNetworkingForNavigation();
  state = WebServerActivityState::WIFI_SELECTION;
  requestUpdate();
  // autoConnect=false: a saved-network pick is a deliberate choice, not an
  // automatic join. Saved entries sort first and long-press forgets.
  if (!startActivityForResultWith<WifiSelectionActivity>(
          [this](const ActivityResult& result) {
            savedNetworkCount = WIFI_STORE.getCredentialCount();
            if (!result.isCancelled) {
              const auto& wifi = std::get<WifiResult>(result.data);
              connectedIP = wifi.ip;
              connectedSSID = wifi.ssid;
              isApMode = false;
              restartMdns(AP_HOSTNAME, "WEBACT");
              startWebServer();
            } else {
              state = WebServerActivityState::TRANSFER_PAGE;
              requestUpdate();
            }
          },
          false)) {
    state = WebServerActivityState::TRANSFER_PAGE;
    requestUpdate();
  }
}

void CrossPointWebServerActivity::openBluetooth() {
  // The web server cannot serve while a sub-activity owns loop(); stopping it
  // here is the same contract as leaving the page (transfer ends).
  stopNetworkingForNavigation();
  state = WebServerActivityState::TRANSFER_PAGE;
  requestUpdate();
  startActivityForResultWith<BluetoothSettingsActivity>([this](const ActivityResult&) { requestUpdate(); });
}

void CrossPointWebServerActivity::handleTransferPageInput() {
  if (state == WebServerActivityState::TRANSFER_PAGE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
      onGoHome();
      return;
    }
  }

  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return;
  const auto hit = [&](const Rect& rect) {
    return rect.width > 0 && rect.height > 0 && x >= rect.x && x < rect.x + rect.width && y >= rect.y &&
           y < rect.y + rect.height;
  };
  if (hit(modeRow_)) {
    openModeSelection();
    return;
  }
  if (hit(savedRow_)) {
    openSavedNetworks();
    return;
  }
  if (bluetoothRowVisible_ && hit(bluetoothRow_)) {
    openBluetooth();
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // The sub-activities paint while they are on the stack; if a frame slips
  // through, paint the page they were opened from so the screen is never
  // blank.
  if (state == WebServerActivityState::SHUTTING_DOWN) return;

  renderer.clearScreen();
  if (state == WebServerActivityState::AP_STARTING) {
    renderStartingHotspot();
  } else {
    renderTransferPage();
  }
  renderer.displayBuffer();
}

void CrossPointWebServerActivity::renderStartingHotspot() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safeArea.x, safeArea.y + metrics.topPadding, safeArea.width, metrics.headerHeight},
                 headerTitle(), nullptr);
  const Rect content = SubpageLayout::contentRect(safeArea, metrics);
  if (UITheme::getInstance().usesCpyLayout()) {
    // Wave bar with the duck at the mockup's 55% mark (starting a hotspot has
    // no true progress), bold title.
    constexpr int kBarHeight = 40;
    const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
    const int barWidth = std::min(content.width - 2 * metrics.contentSidePadding, 420);
    const int blockHeight = kBarHeight + 20 + titleLine;
    int y = content.y + (content.height - blockHeight) / 2;
    GUI.drawProgressBar(renderer, Rect{content.x + (content.width - barWidth) / 2, y, barWidth, kBarHeight}, 55, 100,
                        false);
    y += kBarHeight + 20;
    UITheme::drawCenteredText(renderer, content, UI_12_FONT_ID, y, tr(STR_STARTING_HOTSPOT), true,
                              EpdFontFamily::BOLD);
  } else {
    const Rect text = SubpageLayout::insetHorizontal(content, metrics.contentSidePadding);
    const int height = renderer.getLineHeight(UI_12_FONT_ID);
    UITheme::drawCenteredText(renderer, text, UI_12_FONT_ID, SubpageLayout::centeredTop(content, height),
                              tr(STR_STARTING_HOTSPOT));
  }
}

// refreshed UI frame 14 (network & transfer). Everything on this page is live:
// the network card only exists while a link is actually up, and the web card
// only publishes a URL/QR while the server is running on this page. There is
// no U-disk row, no 5 GHz claim and no per-source sync timestamp because the
// firmware cannot know any of them (see ALIGNMENT-AUDIT.md).
void CrossPointWebServerActivity::renderTransferPage() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int width = renderer.getScreenWidth();
  const int pad = metrics.contentSidePadding;
  const int innerX = safeArea.x + pad;
  const int cardW = std::max(1, safeArea.width - 2 * pad);
  const int gap = SubpageLayout::sectionGap(metrics);

  GUI.drawHeader(renderer, Rect{safeArea.x, safeArea.y + metrics.topPadding, safeArea.width, metrics.headerHeight},
                 headerTitle(), nullptr);

  const Rect content = SubpageLayout::contentRect(safeArea, metrics, false, metrics.buttonHintsHeight);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyLine = renderer.getLineHeight(UI_10_FONT_ID);
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int bigLine = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const bool compact = content.height < 780;

  int y = content.y + SubpageLayout::relatedGap(metrics);

  const bool staUp = !isApMode && WiFi.status() == WL_CONNECTED;
  const bool apUp = isApMode && state == WebServerActivityState::SERVER_RUNNING;
  // Current-network card, always present (frame 14): when nothing is up it
  // says so instead of vanishing and leaving only a dead transfer card.
  {
    const int cardH = compact ? 112 : 148;
    const int cardPad = 20;
    GUI.drawCardFrame(renderer, Rect{static_cast<int16_t>(innerX), static_cast<int16_t>(y), static_cast<int16_t>(cardW),
                                     static_cast<int16_t>(cardH)},
                      false);
    drawMaskIcon(renderer, icon_wifi_48, innerX + cardPad, y + cardPad - 2, 28);
    renderer.drawText(SMALL_FONT_ID, innerX + cardPad + 8 + 28, y + cardPad, tr(STR_CURRENT_NETWORK), true,
                      EpdFontFamily::BOLD);

    const std::string ssid =
        staUp ? std::string(WiFi.SSID().c_str()) : (apUp ? connectedSSID : std::string(tr(STR_NOT_CONNECTED)));
    const std::string ipText = staUp ? [] {
      const IPAddress ip = WiFi.localIP();
      char buf[16];
      snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
      return std::string(buf);
    }() : connectedIP;
    const int barsW = staUp ? 52 + 16 : 0;
    const std::string shownSsid =
        renderer.truncatedText(UI_12_FONT_ID, ssid.c_str(), std::max(1, cardW - 2 * cardPad - barsW));
    renderer.drawText(UI_12_FONT_ID, innerX + cardPad, y + cardPad + smallLine + 10, shownSsid.c_str(), true,
                      EpdFontFamily::BOLD);
    if (staUp || apUp) {
      const std::string meta = std::string(I18N.get(staUp ? StrId::STR_CONNECTED : StrId::STR_HOTSPOT_MODE)) +
                               " \xc2\xb7 " + ipText;
      renderer.drawText(SMALL_FONT_ID, innerX + cardPad,
                        y + cardPad + smallLine + 10 + titleLine + 8, meta.c_str());
    } else {
      renderer.drawText(SMALL_FONT_ID, innerX + cardPad,
                        y + cardPad + smallLine + 10 + titleLine + 8, tr(STR_JOIN_NETWORK));
    }
    if (staUp) {
      drawHubSignalBars(renderer, innerX + cardW - cardPad - 52, y + cardH - cardPad - 38,
                        barsForRssi(WiFi.RSSI(), 0));
    }
    y += cardH + gap;
  }

  // Web-transfer card. The URL and QR exist only while this page owns a
  // running server; otherwise the card says so instead of showing a dead link.
  const bool running = state == WebServerActivityState::SERVER_RUNNING && webServer && webServer->isRunning();
  const int cardPad = 20;
  const int qrSize = compact ? 96 : 140;
  const int headerY = y + cardPad;
  int cardH = cardPad * 2 + smallLine + 10 + smallLine;
  if (running) {
    cardH = cardPad * 2 + smallLine + 10 + bigLine + 6 + smallLine + 10 + qrSize;
  }
  GUI.drawCardFrame(renderer, Rect{static_cast<int16_t>(innerX), static_cast<int16_t>(y), static_cast<int16_t>(cardW),
                                   static_cast<int16_t>(cardH)},
                    false);
  drawGlobeIcon(renderer, innerX + cardPad, headerY - 2, 24);
  renderer.drawText(SMALL_FONT_ID, innerX + cardPad + 34, headerY, tr(STR_WEB_TRANSFER), true, EpdFontFamily::BOLD);

  if (running) {
    const std::string webInfo = std::string("http://") + connectedIP + "/";
    const int urlY = headerY + smallLine + 10;
    const std::string shownUrl =
        renderer.truncatedText(NOTOSANS_18_FONT_ID, webInfo.c_str(), std::max(1, cardW - 2 * cardPad - qrSize - 16));
    renderer.drawText(NOTOSANS_18_FONT_ID, innerX + cardPad, urlY, shownUrl.c_str(), true, EpdFontFamily::BOLD);
    const std::string hint =
        renderer.truncatedText(SMALL_FONT_ID, tr(STR_OPEN_URL_HINT), std::max(1, cardW - 2 * cardPad - qrSize - 16));
    renderer.drawText(SMALL_FONT_ID, innerX + cardPad, urlY + bigLine + 6, hint.c_str());
    if (isApMode) {
      const std::string joinHint = renderer.truncatedText(SMALL_FONT_ID, tr(STR_CONNECT_WIFI_HINT),
                                                          std::max(1, cardW - 2 * cardPad));
      renderer.drawText(SMALL_FONT_ID, innerX + cardPad, urlY + bigLine + 6 + smallLine + 2, joinHint.c_str());
    }
    const int qrX = innerX + cardW - cardPad - qrSize;
    const int qrY = urlY;
    QrUtils::drawQrCode(renderer, Rect{static_cast<int16_t>(qrX), static_cast<int16_t>(qrY), qrSize, qrSize}, webInfo);
    renderer.drawText(SMALL_FONT_ID, qrX,
                      qrY + qrSize + 4, renderer.truncatedText(SMALL_FONT_ID, tr(STR_SCAN_QR_HINT), qrSize).c_str());
  } else {
    renderer.drawText(SMALL_FONT_ID, innerX + cardPad, headerY + smallLine + 10, tr(STR_NOT_RUNNING));
  }
  y += cardH + gap;

  // Navigation rows: 88 px, hairline separators, icon + label + value + ">".
  const int rowH = UiHighDpiProfile::enabled ? 88 : 60;
  const int rowIcon = UiHighDpiProfile::enabled ? 40 : 28;
  const int rowTextX = innerX + 20 + rowIcon + 14;
  const int rowRight = safeArea.x + safeArea.width - pad - 16;
  modeRow_ = Rect{};
  savedRow_ = Rect{};
  bluetoothRow_ = Rect{};
  bluetoothRowVisible_ = false;
#if FREEINK_CAP_BLE_HID_HOST
  bluetoothRowVisible_ = true;
#endif

  const int rowCount = bluetoothRowVisible_ ? 3 : 2;
  const freeink::Icon* rowIcons[3] = {&icon_wifi_48, &icon_library_48, &icon_bluetooth_48};
  const char* rowLabels[3] = {tr(STR_NETWORK_MODE), tr(STR_WIFI_NETWORKS), tr(STR_BLUETOOTH)};
  char savedCountText[16];
  snprintf(savedCountText, sizeof(savedCountText), "%zu", savedNetworkCount);
  const char* rowValues[3] = {staUp ? tr(STR_CONNECTED) : (apUp ? tr(STR_HOTSPOT_MODE) : tr(STR_NOT_CONNECTED)),
                              savedCountText,
                              SETTINGS.bluetoothEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF)};

  for (int i = 0; i < rowCount; ++i) {
    const int rowY = y + i * rowH;
    if (rowY + rowH > (content.y + content.height)) break;
    const Rect row{static_cast<int16_t>(safeArea.x), static_cast<int16_t>(rowY),
                   static_cast<int16_t>(safeArea.width), rowH};
    if (i > 0) renderer.drawLine(0, rowY, width, rowY, 1, true);
    drawMaskIcon(renderer, *rowIcons[i], innerX, rowY + (rowH - rowIcon) / 2, rowIcon);
    renderer.drawText(UI_10_FONT_ID, rowTextX, rowY + (rowH - bodyLine) / 2, rowLabels[i], true,
                      EpdFontFamily::BOLD);
    const int chevW = renderer.getTextWidth(SMALL_FONT_ID, ">");
    renderer.drawText(SMALL_FONT_ID, rowRight - chevW, rowY + (rowH - smallLine) / 2, ">");
    const int valueMax = std::max(1, rowRight - chevW - 12 - rowTextX);
    const std::string value = renderer.truncatedText(SMALL_FONT_ID, rowValues[i], valueMax);
    renderer.drawText(SMALL_FONT_ID, rowRight - chevW - 12 - renderer.getTextWidth(SMALL_FONT_ID, value.c_str()),
                      rowY + (rowH - smallLine) / 2, value.c_str());
    if (i == 0) {
      modeRow_ = row;
    } else if (i == 1) {
      savedRow_ = row;
    } else {
      bluetoothRow_ = row;
    }
  }

  // Exiting stops the transfer; the button hint is the honest place to say it
  // (the design's longer warning has no i18n key yet).
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
