#include "FontExtrasActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <HalClock.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>

#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "EmojiFont.h"
#include "BackgroundWork.h"
#include "activities/network/WifiSelectionActivity.h"
#include "Logging.h"
#include "MappedInputManager.h"
#include "NetworkStartup.h"
#include "SdCardFontSystem.h"
#include "WifiCredentialStore.h"
#include "HalPowerManager.h"
#include "components/StatusCards.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {
std::atomic<bool> g_promptShownThisBoot{false};
}

FontExtrasActivity::FontExtrasActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Purpose purpose)
    : UiListActivity("FontExtras", renderer, mappedInput), purpose_(purpose) {}

bool FontExtrasActivity::wasPromptShownThisBoot() { return g_promptShownThisBoot.load(std::memory_order_relaxed); }
void FontExtrasActivity::suppressPromptThisBoot() {
  g_promptShownThisBoot.store(true, std::memory_order_relaxed);
}

void FontExtrasActivity::onEnter() {
  UiListActivity::onEnter();
  app.on(ACTION_CANCEL_DOWNLOAD, &FontExtrasActivity::onCancel, this);
  font_extras::loadInstalled(installed_);
  if (purpose_ == Purpose::ReaderPrompt) {
    g_promptShownThisBoot.store(true, std::memory_order_relaxed);
    confirm_.ask(tr(STR_FONT_EXTRAS_MISSING_TITLE), tr(STR_FONT_EXTRAS_MISSING_BODY), tr(STR_DOWNLOAD),
                 [this] { startCheck(); }, [this] { finish(); });
    requestUpdate();
    return;
  }
  std::string error;
  if (!font_extras::loadCachedManifest(packs_, error)) packs_.clear();
  rowsDirty_ = true;  // buildScreen() rebuilds on the render task
  requestUpdate();
}

void FontExtrasActivity::debugConfirm() {
  if (state_ == State::ERROR) {
    startCheck();
    return;
  }
  if (state_ == State::LIST) activateIndex(nav.selected);
}

void FontExtrasActivity::debugCancel() {
  if (state_ == State::FETCHING || state_ == State::DOWNLOADING) {
    cancelRequested_ = true;
    LOG_INF("FEXT", "cancel requested (serial)");
  }
}

void FontExtrasActivity::drawFooter() {
  if (state_ == State::FETCHING || state_ == State::DOWNLOADING) {
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  UiListActivity::drawFooter();
}

void FontExtrasActivity::pollCancelInput() {
  // The worker owns the request; this runs on the UI task so Cancel, Back and
  // the Home gesture stay live. The worker checks the flag at each chunk and
  // between route attempts (a TLS connect itself cannot be interrupted).
  mappedInput.update();
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY) && waitCancelRect_.width > 0 && tapX >= waitCancelRect_.x &&
      tapX < waitCancelRect_.x + waitCancelRect_.width && tapY >= waitCancelRect_.y &&
      tapY < waitCancelRect_.y + waitCancelRect_.height) {
    if (!cancelRequested_) {
      cancelRequested_ = true;
      requestUpdate(true);
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
    if (!cancelRequested_) {
      cancelRequested_ = true;
      requestUpdate(true);
    }
  }
  UiAppHost::routeTouch(mappedInput);
  if (app.invalidated()) requestUpdate();
}

void FontExtrasActivity::onCancel(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<FontExtrasActivity*>(user);
  if (self->state_ != State::FETCHING && self->state_ != State::DOWNLOADING) return;
  if (self->cancelRequested_) return;
  self->cancelRequested_ = true;
  self->requestUpdate(true);
}

void FontExtrasActivity::onExit() {
  if (worker_ != nullptr) {
    // Ask the worker to stop at its next boundary. A single TLS connect cannot
    // be interrupted, so wait a bounded time before the last-resort delete.
    cancelRequested_ = true;
    const uint32_t deadline = millis() + 30000;
    while (worker_ != nullptr && !workerDone_.load(std::memory_order_acquire) &&
           static_cast<int32_t>(deadline - millis()) > 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (worker_ != nullptr && !workerDone_.load(std::memory_order_acquire)) {
      LOG_ERR("FEXT", "worker did not stop in time; deleting it");
    }
    if (worker_ != nullptr) reapWorker();
  }
  Activity::onExit();
  if (wifiOwned_) {
    NetworkStartup::releaseRadio();
    wifiOwned_ = false;
  }
  // Release the SNTP suspension only after the radio is down: re-arming while
  // Wi-Fi is up starts a DNS query we deliberately kept away from teardown.
  if (timeSyncSuspended_) {
    NetworkStartup::resumeTimeSync();
    timeSyncSuspended_ = false;
  }
}

int FontExtrasActivity::listCount() const { return 1 + static_cast<int>(packs_.size()); }

const char* FontExtrasActivity::headerTitle() const { return tr(STR_MANAGE_FONT_EXTRAS); }

FontExtrasActivity::PackState FontExtrasActivity::packState(const font_extras::Pack& pack) const {
  if (font_extras::isInstalled(pack)) return PackState::Installed;
  for (const auto& entry : installed_) {
    if (std::strcmp(entry.id, pack.id) == 0) return PackState::Update;
  }
  return PackState::NotInstalled;
}

void FontExtrasActivity::rebuildRows() {
  rowLabels_.assign(1 + packs_.size(), std::string());
  rowSubtitles_.assign(1 + packs_.size(), std::string());
  rowValues_.assign(1 + packs_.size(), std::string());
  rowItems_.clear();
  rowItems_.reserve(1 + packs_.size());

  rowLabels_[0] = tr(STR_FONT_EXTRAS_CHECK);
  rowValues_[0] = packs_.empty() ? std::string() : std::to_string(packs_.size());
  {
    fui::ListItem item;
    item.label = rowLabels_[0].c_str();
    if (!rowValues_[0].empty()) item.value = rowValues_[0].c_str();
    item.actionValue = 0;
    rowItems_.push_back(item);
  }

  for (size_t i = 0; i < packs_.size(); ++i) {
    const auto& pack = packs_[i];
    const PackState state = packState(pack);
    rowLabels_[i + 1] = pack.name;
    rowSubtitles_[i + 1] = std::string(pack.kind == font_extras::Kind::Emoji ? tr(STR_FONT_EXTRAS_EMOJI)
                                                                            : tr(STR_FONT_EXTRAS_FALLBACK)) +
                           " · " + font_extras::formatSize(pack.size);
    switch (state) {
      case PackState::Installed:
        rowValues_[i + 1] = tr(STR_INSTALLED);
        break;
      case PackState::Update:
        rowValues_[i + 1] = tr(STR_UPDATE_AVAILABLE);
        break;
      case PackState::NotInstalled:
        rowValues_[i + 1] = tr(STR_FONT_EXTRAS_NOT_INSTALLED);
        break;
    }
    fui::ListItem item;
    item.label = rowLabels_[i + 1].c_str();
    item.subtitle = rowSubtitles_[i + 1].c_str();
    item.value = rowValues_[i + 1].c_str();
    item.actionValue = static_cast<int16_t>(i + 1);
    if (state == PackState::Installed) item.state = fui::StateDisabled;  // visual only; still tappable for delete
    rowItems_.push_back(item);
  }
  rowsDirty_ = false;
}

void FontExtrasActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (state_ != State::LIST) {
    // Touch devices get a real Cancel action in the footer. The network work
    // blocks the UI task, so its progress callback routes touch into this action.
    if (mappedInput.hasTouch()) {
      fui::FooterAction actions[1] = {{tr(STR_CANCEL), ACTION_CANCEL_DOWNLOAD}};
      screen.footer(actions, 1);
    }
    return;
  }
  if (rowsDirty_) rebuildRows();
  if (rowItems_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }
  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void FontExtrasActivity::activateIndex(const int index) {
  if (state_ != State::LIST) return;
  app.clearTapFlash();
  if (index == 0) {
    startCheck();
    return;
  }
  const int packIndex = index - 1;
  if (packIndex < 0 || packIndex >= static_cast<int>(packs_.size())) return;
  if (packState(packs_[packIndex]) == PackState::Installed) {
    promptRemove(packIndex);
    return;
  }
  startDownload(packIndex);
}

void FontExtrasActivity::startCheck() {
  if (state_ == State::FETCHING || state_ == State::DOWNLOADING) return;
  // The busy loop skips the list host, so an open confirm could neither be
  // dismissed nor painted over the wait card; close it before going busy.
  confirm_.dismiss();
  pendingAction_ = PendingAction::Check;
  pendingDownloadIndex_ = -1;
  ensureWifi();
}

void FontExtrasActivity::ensureWifi() {
  // OTA disables auto-sync before Wi-Fi for the same reason: the connection
  // callback starts NTP, whose DNS work competes with the TLS handshake for the
  // last few KB of internal RAM (and can trip lwIP's udp_new_ip_type guard).
  // The suspension stays for the whole session; request helpers nest on it.
  if (!timeSyncSuspended_) {
    NetworkStartup::suspendTimeSync();
    timeSyncSuspended_ = true;
  }
  if (WiFi.getMode() != WIFI_MODE_NULL && WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  // Join the last saved network without the selection sheet when possible.
  // The sheet's list rendering fragments the internal heap badly enough that
  // the handshake loses its contiguous block (measured 17.9 KB/7.1 KB via the
  // sheet vs 25.0 KB/13.3 KB via this silent path, which the browser uses).
  if (trySilentSavedNetwork()) {
    wifiOwned_ = true;
    onWifiSelectionComplete(true);
    return;
  }
  wifiOwned_ = true;
  if (!startActivityForResultWith<WifiSelectionActivity>(
          [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); })) {
    pendingAction_ = PendingAction::None;
    pendingDownloadIndex_ = -1;
    setError(tr(STR_FONT_EXTRAS_NEED_WIFI));
  }
}

bool FontExtrasActivity::trySilentSavedNetwork() {
  HalPowerManager::Lock powerLock(/*quiet=*/true);
  WIFI_STORE.loadFromFile();
  const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential.has_value()) {
    LOG_INF("FEXT", "no saved network to connect silently");
    return false;
  }
  LOG_INF("FEXT", "connecting saved network %.32s", credential->ssid.c_str());
  if (!NetworkStartup::setMode(renderer, WIFI_STA)) return false;
  WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(deadline - millis()) > 0) {
    delay(100);
  }
  const bool connected = WiFi.status() == WL_CONNECTED;
  LOG_INF("FEXT", "saved network connect %s", connected ? "ok" : "failed");
  if (!connected) NetworkStartup::releaseRadio();
  return connected;
}

void FontExtrasActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    pendingAction_ = PendingAction::None;
    pendingDownloadIndex_ = -1;
    setError(tr(STR_FONT_EXTRAS_NEED_WIFI));
    return;
  }
  if (!HttpDownloader::hasMemoryForTls()) {
    pendingAction_ = PendingAction::None;
    pendingDownloadIndex_ = -1;
    setError(tr(STR_MEMORY_ERROR));
    return;
  }
  if (pendingAction_ == PendingAction::Download) {
    const int packIndex = pendingDownloadIndex_;
    pendingAction_ = PendingAction::None;
    pendingDownloadIndex_ = -1;
    startDownloadNow(packIndex);
    return;
  }
  if (pendingAction_ != PendingAction::Check) return;
  pendingAction_ = PendingAction::None;
  NetworkStartup::prepare(renderer);
  {
    RenderLock lock(*this);
    downloadStartedAt_ = millis();
    lastPaintMs_ = downloadStartedAt_;
    state_ = State::FETCHING;
  }
  requestUpdateAndWait();
  if (!startWorker(Job::Manifest)) {
    setError(tr(STR_MEMORY_ERROR));
  }
}

void FontExtrasActivity::startDownload(const int packIndex) {
  if (packIndex < 0 || packIndex >= static_cast<int>(packs_.size())) return;
  confirm_.dismiss();
  pendingAction_ = PendingAction::Download;
  pendingDownloadIndex_ = packIndex;
  ensureWifi();
}

void FontExtrasActivity::startDownloadNow(const int packIndex) {
  if (packIndex < 0 || packIndex >= static_cast<int>(packs_.size())) return;
  NetworkStartup::prepare(renderer);
  {
    RenderLock lock(*this);
    jobPack_ = packs_[packIndex];  // stable across the worker download
    jobPackIndex_ = packIndex;
    downloadTitle_ = jobPack_.name;
    downloadProgress_.store(0);
    downloadTotal_.store(jobPack_.size);
    downloadStartedAt_ = millis();
    lastPaintMs_ = downloadStartedAt_;
    state_ = State::DOWNLOADING;
  }
  requestUpdateAndWait();
  if (!startWorker(Job::Download)) {
    setError(tr(STR_MEMORY_ERROR));
  }
}

bool FontExtrasActivity::startWorker(const Job job) {
  if (worker_ != nullptr) return false;
  // Reclaim again right before TLS. The first prepare() ran before the Wi-Fi
  // sheet; that sheet (and the wait card) re-rendered text and refilled the
  // font glyph caches, which cost ~5 KB of internal RAM and ~2 KB of the
  // largest block — enough to turn a working handshake into "TCP connect
  // failed". prepare() skips its reclaim when free heap looks acceptable, so
  // release the caches unconditionally, exactly like the browser does in
  // beginFetch().
  NetworkStartup::prepare(renderer);
  {
    const RenderLock lock;
    if (auto* fontCache = renderer.getFontCacheManager()) {
      fontCache->clearCache();
      fontCache->releaseSdFontCaches();
    }
  }
  // The wait card is already on screen, so the list model is not being drawn.
  // Releasing it now matters: measured in the same session, the browser (no
  // list state) kept a 13.3 KB internal block through association, while this
  // activity kept only 6.9 KB and the handshake timed out. The rows rebuild
  // from packs_/installed_ when the job finishes.
  {
    std::vector<freeink::ui::ListItem>().swap(rowItems_);
    std::vector<std::string>().swap(rowLabels_);
    std::vector<std::string>().swap(rowSubtitles_);
    std::vector<std::string>().swap(rowValues_);
    std::vector<font_extras::InstalledPack>().swap(installed_);
    rowsDirty_ = true;
  }
  LOG_INF("FEXT", "list state released: free=%u largest=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  job_ = job;
  jobOk_ = false;
  jobFromCache_ = false;
  jobError_.clear();
  cancelRequested_ = false;
  workerDone_.store(false, std::memory_order_relaxed);
  constexpr uint32_t kPsramStackBytes = 16 * 1024;
  constexpr uint32_t kInternalStackBytes = 8 * 1024;
  BaseType_t ok = pdFAIL;
#if defined(BOARD_HAS_PSRAM)
  ok = xTaskCreatePinnedToCoreWithCaps(workerTrampoline, "fontExtras", kPsramStackBytes / sizeof(StackType_t), this,
                                       tskIDLE_PRIORITY, &worker_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  workerStackInPsram_ = ok == pdPASS;
  if (ok != pdPASS) {
    LOG_ERR("FEXT", "PSRAM worker stack refused (%d); using internal RAM", static_cast<int>(ok));
  }
#endif
  if (ok != pdPASS) {
    workerStackInPsram_ = false;
    ok = xTaskCreatePinnedToCore(workerTrampoline, "fontExtras", kInternalStackBytes / sizeof(StackType_t), this,
                                 tskIDLE_PRIORITY, &worker_, 0);
  }
  if (ok != pdPASS) {
    worker_ = nullptr;
    job_ = Job::None;
    return false;
  }
  LOG_INF("FEXT", "worker up: stack in %s, free=%u largest=%u", workerStackInPsram_ ? "PSRAM" : "INTERNAL RAM",
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  return true;
}

void FontExtrasActivity::workerTrampoline(void* param) {
  auto* self = static_cast<FontExtrasActivity*>(param);
  {
    // A download can outlive a standby transition; the guard keeps the device
    // awake (no light/deep sleep) until the SD writes are finished.
    background_work::Guard guard;
    self->runWorker();
  }
  self->workerDone_.store(true, std::memory_order_release);
  vTaskSuspend(nullptr);
}

void FontExtrasActivity::runWorker() {
  const Job job = job_;
  // Same settling as the browser's fetch task: the radio reports connected
  // before DHCP/ARP are useful, and the CPU governor drops to 80 MHz after a
  // few idle seconds (ECC then takes far longer and the handshake deadline can
  // expire even though the server answered).
  vTaskDelay(pdMS_TO_TICKS(1000));
  HalPowerManager::Lock powerLock(/*quiet=*/true);
  font_extras::Failure failure = font_extras::Failure::None;
  if (job == Job::Manifest) {
    stage_.store(1, std::memory_order_relaxed);
    std::vector<font_extras::Pack> packs;
    bool fromCache = false;
    std::string error;
    const bool ok = font_extras::fetchManifest(
        packs, fromCache, error,
        [this](const size_t done, size_t) {
          downloadProgress_.store(done);
          if (done > 0) stage_.store(2, std::memory_order_relaxed);
        },
        &cancelRequested_, &failure);
    stage_.store(3, std::memory_order_relaxed);
    jobOk_ = ok;
    jobFromCache_ = fromCache;
    jobError_ = std::move(error);
    jobPacks_ = std::move(packs);
    jobFailure_ = failure;
    return;
  }
  if (job == Job::Download) {
    stage_.store(1, std::memory_order_relaxed);
    std::string error;
    const bool ok = font_extras::downloadAndInstall(
        jobPack_,
        [this](const size_t downloaded, const size_t total) {
          downloadProgress_.store(downloaded);
          downloadTotal_.store(total);
          if (downloaded > 0) stage_.store(2, std::memory_order_relaxed);
        },
        &cancelRequested_, error, &failure);
    stage_.store(3, std::memory_order_relaxed);
    jobOk_ = ok;
    jobError_ = std::move(error);
    jobFailure_ = failure;
  }
}

void FontExtrasActivity::reapWorker() {
  if (worker_ == nullptr) return;
  if (workerStackInPsram_) {
    vTaskDeleteWithCaps(worker_);
  } else {
    vTaskDelete(worker_);
  }
  worker_ = nullptr;
  workerStackInPsram_ = false;
}

void FontExtrasActivity::pollWorker() {
  if (worker_ == nullptr || !workerDone_.load(std::memory_order_acquire)) return;
  reapWorker();
  // The job is over, so give the radio back: it frees the driver's ~43 KB,
  // follows the power rule, and lets the next check re-associate from a clean
  // heap (observed: a fresh association sometimes leaves a larger contiguous
  // internal block than the one that has been up across several requests).
  if (wifiOwned_) {
    NetworkStartup::releaseRadio();
    wifiOwned_ = false;
  }
  const Job job = job_;
  job_ = Job::None;
  const font_extras::Failure failure = jobFailure_;
  jobFailure_ = font_extras::Failure::None;
  stage_.store(0, std::memory_order_relaxed);
  if (job == Job::Manifest) {
    if (!jobOk_) {
      if (cancelRequested_) {
        font_extras::loadInstalled(installed_);
        {
          RenderLock lock(*this);
          state_ = State::LIST;
          rowsDirty_ = true;
        }
        requestUpdate(true);
        return;
      }
      retryPackIndex_ = -1;
      setError(jobError_.c_str(), failure);
      return;
    }
    std::vector<font_extras::InstalledPack> installed;
    font_extras::loadInstalled(installed);
    {
      RenderLock lock(*this);
      packs_ = std::move(jobPacks_);
      installed_ = std::move(installed);
      rowsDirty_ = true;
      state_ = State::LIST;
      // The network failed but the cached manifest is usable: keep the list
      // and say why it may be stale, with Retry / Use cached as the actions.
      offlineNotice_ = jobFromCache_ && failure != font_extras::Failure::None;
      if (offlineNotice_) failure_ = failure;
    }
    LOG_INF("FEXT", "manifest list: %zu packs (cache=%d, failure=%d)", packs_.size(), jobFromCache_ ? 1 : 0,
            static_cast<int>(failure));
    requestUpdate(true);
    return;
  }
  if (job == Job::Download) {
    if (!jobOk_) {
      if (cancelRequested_) {
        font_extras::loadInstalled(installed_);
        {
          RenderLock lock(*this);
          state_ = State::LIST;
          rowsDirty_ = true;
        }
        requestUpdate(true);
      } else {
        retryPackIndex_ = jobPackIndex_;
        setError(jobError_.c_str(), failure);
      }
      return;
    }
    afterInstall(jobPack_);
    std::vector<font_extras::InstalledPack> installed;
    font_extras::loadInstalled(installed);
    {
      RenderLock lock(*this);
      installed_ = std::move(installed);
      rowsDirty_ = true;
      state_ = State::LIST;
      retryPackIndex_ = -1;
    }
    requestUpdate(true);
  }
}

void FontExtrasActivity::loop() {
  pollWorker();
  if (state_ == State::FETCHING || state_ == State::DOWNLOADING) {
    // The network worker owns the request; this task keeps the screen and the
    // Cancel action alive (in-card pill, Back, Home gesture and the footer).
    pollCancelInput();
    // One frame per second so the progress bar actually moves. TLS now
    // allocates from PSRAM (main.cpp), so the glyph-cache refill this causes
    // can no longer starve a handshake.
    const uint32_t now = millis();
    if (now - lastPaintMs_ >= 1000) {
      lastPaintMs_ = now;
      requestUpdate();
    }
    return;
  }
  UiListActivity::loop();
}

void FontExtrasActivity::afterInstall(const font_extras::Pack& pack) {
  if (pack.kind == font_extras::Kind::Fallback) {
    sdFontSystem.markRegistryDirty();
    sdFontSystem.refreshIfDirty();
    sdFontSystem.loadSupplementFonts(renderer, SETTINGS.fontPointSize);
  } else {
    emoji_font::reload(renderer, SETTINGS.fontPointSize);
  }
  renderer.requestNextFullRefresh();
  LOG_INF("FEXT", "activated %s", pack.id);
}

void FontExtrasActivity::promptRemove(const int packIndex) {
  if (packIndex < 0 || packIndex >= static_cast<int>(packs_.size())) return;
  confirm_.ask(tr(STR_DELETE), packs_[packIndex].name, tr(STR_DELETE),
               [this, packIndex] { removePack(packIndex); });
  requestUpdate();
}

void FontExtrasActivity::removePack(const int packIndex) {
  if (packIndex < 0 || packIndex >= static_cast<int>(packs_.size())) return;
  const font_extras::Pack pack = packs_[packIndex];
  std::string error;
  if (!font_extras::removePack(pack, error)) {
    setError(error.c_str());
    return;
  }
  if (pack.kind == font_extras::Kind::Fallback) {
    sdFontSystem.markRegistryDirty();
    sdFontSystem.refreshIfDirty();
    sdFontSystem.loadSupplementFonts(renderer, SETTINGS.fontPointSize);
  } else {
    emoji_font::reload(renderer, SETTINGS.fontPointSize);
  }
  renderer.requestNextFullRefresh();
  std::vector<font_extras::InstalledPack> installed;
  font_extras::loadInstalled(installed);
  {
    RenderLock lock(*this);
    installed_ = std::move(installed);
    rowsDirty_ = true;
  }
  requestUpdate(true);
}

void FontExtrasActivity::setError(const char* message, const font_extras::Failure failure) {
  {
    RenderLock lock(*this);
    errorMessage_ = message != nullptr ? message : "";
    failure_ = failure;
    state_ = State::ERROR;
  }
  LOG_ERR("FEXT", "error (%d): %s", static_cast<int>(failure), errorMessage_.c_str());
  requestUpdate(true);
}

const char* FontExtrasActivity::failureText(const font_extras::Failure failure) const {
  switch (failure) {
    case font_extras::Failure::NoWifi:
      return tr(STR_FONT_EXTRAS_NEED_WIFI);
    case font_extras::Failure::Timeout:
      return tr(STR_FONT_EXTRAS_TIMEOUT);
    case font_extras::Failure::Network:
      return tr(STR_FONT_EXTRAS_NETWORK);
    case font_extras::Failure::HttpStatus:
      return tr(STR_FONT_EXTRAS_SERVER);
    case font_extras::Failure::Manifest:
      return tr(STR_FONT_EXTRAS_BAD_MANIFEST);
    case font_extras::Failure::Download:
      return tr(STR_DOWNLOAD_FAILED);
    case font_extras::Failure::Verify:
      return tr(STR_FONT_EXTRAS_VERIFY_FAILED);
    case font_extras::Failure::SdCard:
      return tr(STR_FONT_EXTRAS_STORAGE);
    case font_extras::Failure::None:
    default:
      return errorMessage_.empty() ? tr(STR_ERROR_GENERAL_FAILURE) : errorMessage_.c_str();
  }
}

void FontExtrasActivity::drawWait() {
  const uint32_t elapsed = millis() - downloadStartedAt_;
  const size_t done = downloadProgress_.load();
  const size_t total = downloadTotal_.load();
  const int stage = stage_.load();
  int permille = 80 + std::min<int>(320, static_cast<int>(elapsed / 50));  // 8..40 %
  std::string detail;
  if (cancelRequested_) {
    detail = tr(STR_CANCEL);
  } else if (stage <= 1) {
    // Association/DNS/TLS have no byte counter; advancing by time is honest
    // about "still connecting" without pretending to know the remainder.
    detail = tr(STR_CONNECTING);
  } else if (state_ == State::DOWNLOADING) {
    permille = total != 0
                   ? 400 + static_cast<int>(std::min<uint64_t>(550, static_cast<uint64_t>(done) * 550 / total))
                   : std::min(900, 400 + static_cast<int>(done / 2048) * 100);
    detail = font_extras::formatSize(done);
    if (total != 0) detail += " / " + font_extras::formatSize(total);
  } else {
    // Manifest body: ~679 bytes. 45..90 % while it arrives.
    permille = 450 + static_cast<int>(std::min<uint64_t>(450, static_cast<uint64_t>(done) * 450 / 2048));
    detail = font_extras::formatSize(done);
  }
  if (stage >= 3) permille = 940;
  permille = std::max(0, std::min(1000, permille));
  const char* title = state_ == State::FETCHING ? tr(STR_LOADING_FONT_LIST) : tr(STR_DOWNLOADING);
  const char* subtitle = state_ == State::FETCHING ? nullptr : downloadTitle_.c_str();
  waitCancelRect_ = status_card::paintWait(renderer, title, subtitle, detail.c_str(), permille,
                                           /*withCancel=*/true, elapsed)
                        .cancel;
}

void FontExtrasActivity::drawErrorCard() {
  status_card::paintMessage(renderer, tr(STR_FONT_EXTRAS_ERROR), failureText(failure_));
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int kBtnH = 56;
  constexpr int kRetryW = 200;
  constexpr int kBackW = 140;
  constexpr int kGap = 20;
  const int rowY = safe.y + safe.height - metrics.buttonHintsHeight - kBtnH - 8;
  const int rowX = safe.x + (safe.width - (kRetryW + kGap + kBackW)) / 2;
  errorRetryRect_ = Rect{rowX, rowY, kRetryW, kBtnH};
  errorBackRect_ = Rect{rowX + kRetryW + kGap, rowY, kBackW, kBtnH};
  GUI.drawActionButton(renderer, errorRetryRect_, tr(STR_RETRY), true);
  GUI.drawActionButton(renderer, errorBackRect_, tr(STR_BACK), false);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), nullptr, nullptr);
  GUI.drawButtonHintsWithStyle(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, false);
}

bool FontExtrasActivity::handleErrorActions() {
  if (state_ != State::ERROR) return false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (retryPackIndex_ >= 0 && retryPackIndex_ < static_cast<int>(packs_.size())) {
      startDownload(retryPackIndex_);
    } else {
      startCheck();
    }
    return true;
  }
  return true;
}

bool FontExtrasActivity::handleOfflineNotice() {
  if (!offlineNotice_) return false;
  const auto useCache = [this] {
    {
      RenderLock lock(*this);
      offlineNotice_ = false;
    }
    requestUpdate(true);
  };
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    useCache();
    return true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    {
      RenderLock lock(*this);
      offlineNotice_ = false;
    }
    startCheck();
    return true;
  }
  return false;
}

bool FontExtrasActivity::handleCustomInput() {
  if (confirm_.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (state_ == State::FETCHING || state_ == State::DOWNLOADING) return true;
  int tapX = 0;
  int tapY = 0;
  const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
  const auto inside = [&](const Rect& r) {
    return tapped && r.width > 0 && tapX >= r.x && tapX < r.x + r.width && tapY >= r.y && tapY < r.y + r.height;
  };
  if (state_ == State::ERROR) {
    if (inside(errorRetryRect_)) {
      if (retryPackIndex_ >= 0 && retryPackIndex_ < static_cast<int>(packs_.size())) {
        startDownload(retryPackIndex_);
      } else {
        startCheck();
      }
      return true;
    }
    if (inside(errorBackRect_)) {
      finish();
      return true;
    }
    return handleErrorActions() || tapped;
  }
  if (offlineNotice_) {
    if (inside(errorRetryRect_)) {
      {
        RenderLock lock(*this);
        offlineNotice_ = false;
      }
      startCheck();
      return true;
    }
    if (inside(errorBackRect_)) {
      {
        RenderLock lock(*this);
        offlineNotice_ = false;
      }
      requestUpdate(true);
      return true;
    }
  }
  return false;
}

bool FontExtrasActivity::paintOverlayBeforePresent() {
  if (confirm_.active()) {
    confirm_.paint(renderer);
    return true;
  }
  if (state_ == State::FETCHING || state_ == State::DOWNLOADING) {
    drawWait();
    return true;
  }
  if (state_ == State::ERROR) {
    drawErrorCard();
    return true;
  }
  if (offlineNotice_) {
    status_card::paintMessage(renderer, tr(STR_FONT_EXTRAS_OFFLINE_TITLE), failureText(failure_));
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
    const auto& metrics = UITheme::getInstance().getMetrics();
    constexpr int kBtnH = 56;
    constexpr int kRetryW = 200;
    constexpr int kUseW = 180;
    constexpr int kGap = 20;
    const int rowY = safe.y + safe.height - metrics.buttonHintsHeight - kBtnH - 8;
    const int rowX = safe.x + (safe.width - (kRetryW + kGap + kUseW)) / 2;
    errorRetryRect_ = Rect{rowX, rowY, kRetryW, kBtnH};
    errorBackRect_ = Rect{rowX + kRetryW + kGap, rowY, kUseW, kBtnH};
    GUI.drawActionButton(renderer, errorRetryRect_, tr(STR_RETRY), true);
    GUI.drawActionButton(renderer, errorBackRect_, tr(STR_USE_CACHE), false);
    return true;
  }
  return false;
}
