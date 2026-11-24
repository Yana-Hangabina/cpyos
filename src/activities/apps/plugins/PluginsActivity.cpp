#include "PluginsActivity.h"

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <Arduino.h>
#include <HalPowerManager.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "BackgroundWork.h"
#include "I18n.h"
#include "I18nKeys.h"
#include "Logging.h"
#include "Memory.h"
#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "AppCatalog.h"
#include "activities/apps/plugins/PluginActivity.h"
#include "components/UiAppHelpers.h"
#include "components/UITheme.h"
#include "components/icons/listIcons.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

void PluginsActivity::onEnter() {
  UiListActivity::onEnter();
  std::string error;
  if (native_plugins::loadLocalCatalog(catalog_, error) ||
      native_plugins::loadCachedCatalog(catalog_, error)) {
    catalogLoaded_ = true;
    LOG_INF("PLUG", "catalog: %u entries", static_cast<unsigned>(catalog_.size()));
  } else {
    catalog_.clear();
    catalogLoaded_ = false;
  }
  rescan();
}

void PluginsActivity::onExit() {
  if (worker_ != nullptr) {
    cancelRequested_ = true;
    const uint32_t deadline = millis() + 30000;
    while (worker_ != nullptr && !workerDone_.load(std::memory_order_acquire) &&
           static_cast<int32_t>(deadline - millis()) > 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (worker_ != nullptr && !workerDone_.load(std::memory_order_acquire)) {
      // The worker may be inside TLS or an SD write; deleting it would corrupt
      // the heap. A reset is the safe recovery (same rule as Font Extras).
      LOG_ERR("PLUG", "catalog worker did not stop; restarting instead of freeing its owner");
      esp_restart();
    }
    if (worker_ != nullptr) reapWorker();
  }
  releaseNetwork();
  confirm_.dismiss();
  UiListActivity::onExit();
}

void PluginsActivity::rescan() {
  plugins_ = native_plugins::scan();
  // The tool drawer mirrors the installed packages. Mark it stale after any
  // rescan: this also covers the external file-transfer copy path, not only
  // installs and removals performed by this screen.
  app_catalog::invalidatePlugins();
  rebuildRows();
}

void PluginsActivity::releaseNetwork() {
  if (wifiOwned_) {
    NetworkStartup::releaseRadio();
    wifiOwned_ = false;
  }
  if (timeSyncSuspended_) {
    NetworkStartup::resumeTimeSync();
    timeSyncSuspended_ = false;
  }
}

int PluginsActivity::catalogIndexFor(const std::string& id) const {
  for (size_t i = 0; i < catalog_.size(); ++i) {
    if (catalog_[i].id == id && catalog_[i].arch == native_plugins::expectedArch()) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void PluginsActivity::rebuildRows() {
  labels_.clear();
  subtitles_.clear();
  values_.clear();
  rows_.clear();
  rowKinds_.clear();
  rowIndexes_.clear();

  const size_t estimate = 2 + plugins_.size() + catalog_.size();
  labels_.reserve(estimate);
  subtitles_.reserve(estimate);
  values_.reserve(estimate);
  rows_.reserve(estimate);
  rowKinds_.reserve(estimate);
  rowIndexes_.reserve(estimate);

  const auto addRow = [this](const RowKind kind, const int index, const char* label, const char* subtitle,
                             const char* value, const freeink::ui::BitmapRef& icon, const bool enabled) {
    labels_.emplace_back(label != nullptr ? label : "");
    subtitles_.emplace_back(subtitle != nullptr ? subtitle : "");
    values_.emplace_back(value != nullptr ? value : "");
    rows_.push_back(fui::ListItem{});
    auto& row = rows_.back();
    row.label = labels_.back().c_str();
    row.subtitle = subtitles_.back().c_str();
    row.value = values_.back().c_str();
    row.actionValue = static_cast<int16_t>(rowKinds_.size());
    row.icon = icon;
    row.enabled = enabled;
    rowKinds_.push_back(kind);
    rowIndexes_.push_back(index);
  };

  addRow(RowKind::Rescan, 0, tr(STR_RETRY),
         plugins_.empty() ? tr(STR_NO_PLUGINS_INSTALLED) : native_plugins::kPluginRoot,
         std::to_string(plugins_.size()).c_str(), fui::bitmapFromIcon(icon_refresh_cw_32), true);
  addRow(RowKind::Check, 0, tr(STR_CHECK_UPDATES), native_plugins::catalogUrl(),
         catalogLoaded_ ? std::to_string(catalog_.size()).c_str() : "",
         fui::bitmapFromIcon(icon_download_32), true);

  for (size_t i = 0; i < plugins_.size(); ++i) {
    const auto& plugin = plugins_[i];
    char meta[96];
    std::snprintf(meta, sizeof(meta), "%s  ·  %u KB  ·  %s", plugin.version.c_str(),
                  static_cast<unsigned>((plugin.elfSize + 1023) / 1024), native_plugins::expectedArch());
    std::string subtitle(meta);
    std::string value;
    bool enabled = true;
    if (!plugin.error.empty()) {
      subtitle = plugin.error;
      value = tr(STR_ERROR_MSG);
      enabled = false;
    } else {
      const int cat = catalogIndexFor(plugin.id);
      if (cat >= 0 && catalog_[cat].version != plugin.version) {
        value = tr(STR_UPDATE_AVAILABLE);
      } else {
        value = tr(STR_INSTALLED);
      }
      if (plugin.crashed) subtitle += "  ·  [crashed last run]";
    }
    const char* localized = app_catalog::pluginTitle(plugin.id.c_str());
    labels_.push_back(localized != nullptr ? localized : (plugin.name.empty() ? plugin.dirName : plugin.name));
    subtitles_.push_back(std::move(subtitle));
    values_.push_back(std::move(value));
    rows_.push_back(fui::ListItem{});
    auto& row = rows_.back();
    row.label = labels_.back().c_str();
    row.subtitle = subtitles_.back().c_str();
    row.value = values_.back().c_str();
    row.actionValue = static_cast<int16_t>(rowKinds_.size());
    row.icon = listIconFor(UIIcon::Plugins, 32);
    row.enabled = enabled;
    rowKinds_.push_back(RowKind::Installed);
    rowIndexes_.push_back(static_cast<int>(i));
  }

  for (size_t i = 0; i < catalog_.size(); ++i) {
    const auto& entry = catalog_[i];
    if (entry.arch != native_plugins::expectedArch()) continue;
    bool installed = false;
    for (const auto& plugin : plugins_) {
      if (plugin.id == entry.id) {
        installed = true;
        break;
      }
    }
    if (installed) continue;
    char meta[96];
    std::snprintf(meta, sizeof(meta), "%s  ·  %u KB  ·  %s", entry.version.c_str(),
                  static_cast<unsigned>((entry.size + 1023) / 1024), entry.arch.c_str());
    const char* localized = app_catalog::pluginTitle(entry.id.c_str());
    addRow(RowKind::Catalog, static_cast<int>(i), localized != nullptr ? localized : entry.name.c_str(), meta,
           tr(STR_FONT_EXTRAS_NOT_INSTALLED), listIconFor(UIIcon::Plugins, 32), true);
  }
}

int PluginsActivity::listCount() const { return static_cast<int>(rows_.size()); }

const char* PluginsActivity::headerTitle() const { return tr(STR_PLUGINS); }

void PluginsActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  nav.selected = index;
  app.clearTapFlash();
  const RowKind kind = rowKinds_[index];
  const int rowIndex = rowIndexes_[index];

  if (kind == RowKind::Rescan) {
    rescan();
    nav.follow(listCount());
    requestUpdate(true);
    return;
  }
  if (kind == RowKind::Check) {
    startCheck();
    return;
  }
  if (kind == RowKind::Catalog) {
    startInstall(rowIndex);
    return;
  }

  if (rowIndex < 0 || rowIndex >= static_cast<int>(plugins_.size())) return;
  const auto& plugin = plugins_[rowIndex];
  if (!plugin.error.empty()) return;
  const int cat = catalogIndexFor(plugin.id);
  if (cat >= 0 && catalog_[cat].version != plugin.version) {
    startInstall(cat);
    return;
  }
  auto activity = makeUniqueNoThrow<PluginActivity>(renderer, mappedInput, plugin);
  if (!activity) {
    LOG_ERR("PLUG", "OOM: plugin activity");
    return;
  }
  activityManager.pushActivity(std::move(activity));
}

bool PluginsActivity::canSwipeDelete(const int index) const {
  return index >= 0 && index < listCount() && rowKinds_[index] == RowKind::Installed;
}

void PluginsActivity::swipeDelete(const int index) { promptRemove(index); }
void PluginsActivity::onRowLongPress(const int index) { promptRemove(index); }

void PluginsActivity::promptRemove(const int row) {
  if (row < 0 || row >= listCount() || rowKinds_[row] != RowKind::Installed) return;
  const int pluginIndex = rowIndexes_[row];
  if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) return;
  const auto& plugin = plugins_[pluginIndex];
  const std::string name = plugin.name.empty() ? plugin.dirName : plugin.name;
  const std::string dir = std::string(native_plugins::kPluginRoot) + "/" + plugin.dirName;
  const std::string dataDir = std::string("/.crosspoint/plugin_data/") + plugin.dirName;
  app.clearTapFlash();
  confirm_.ask(tr(STR_DELETE_ITEM_CONFIRM), name.c_str(), tr(STR_DELETE), [this, dir, dataDir] {
    closeSwipeDelete();
    const bool ok = Storage.removeDir(dir.c_str());
    Storage.removeDir(dataDir.c_str());
    LOG_INF("PLUG", "remove %s: %s (data included)", dir.c_str(), ok ? "ok" : "failed");
    rescan();
    nav.follow(listCount());
    requestUpdate(true);
  });
  requestUpdate();
}

bool PluginsActivity::handleCustomInput() {
  return confirm_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void PluginsActivity::setError(const char* title, const char* body) {
  errorTitle_ = title != nullptr ? title : tr(STR_ERROR_MSG);
  errorBody_ = body != nullptr ? body : "";
  state_ = State::Error;
  requestUpdate(true);
}

bool PluginsActivity::ensureWifi() {
  if (!timeSyncSuspended_) {
    NetworkStartup::suspendTimeSync();
    timeSyncSuspended_ = true;
  }
  if (WiFi.status() == WL_CONNECTED) return true;
  HalPowerManager::Lock powerLock(/*quiet=*/true);
  WIFI_STORE.loadFromFile();
  const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential.has_value()) {
    LOG_INF("PLUG", "catalog: no saved network");
    return false;
  }
  if (!NetworkStartup::setMode(renderer, WIFI_STA)) return false;
  LOG_INF("PLUG", "catalog: connecting saved network %.32s", credential->ssid.c_str());
  WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(deadline - millis()) > 0) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    NetworkStartup::releaseRadio();
    return false;
  }
  wifiOwned_ = true;
  return true;
}

void PluginsActivity::startCheck() {
  if (state_ != State::List) return;
  confirm_.dismiss();
  if (!ensureWifi()) {
    setError(tr(STR_FONT_EXTRAS_NEED_WIFI), tr(STR_CHECK_UPDATES));
    return;
  }
  NetworkStartup::prepare(renderer);
  cancelRequested_ = false;
  workerDone_.store(false);
  progressBytes_.store(0);
  totalBytes_.store(0);
  job_ = Job::Catalog;
  state_ = State::Fetching;
  waitStartedMs_ = millis();
  lastWaitPaintMs_ = waitStartedMs_;
  requestUpdateAndWait();
  if (!startWorker(Job::Catalog)) setError(tr(STR_MEMORY_ERROR), tr(STR_CHECK_UPDATES));
}

void PluginsActivity::startInstall(const int catalogIndex) {
  if (state_ != State::List || catalogIndex < 0 || catalogIndex >= static_cast<int>(catalog_.size())) return;
  confirm_.dismiss();
  const std::string& source = catalog_[catalogIndex].url;
  const bool localSource = source.rfind("file://", 0) == 0 || (!source.empty() && source[0] == '/');
  if (!localSource) {
    if (!ensureWifi()) {
      setError(tr(STR_FONT_EXTRAS_NEED_WIFI), catalog_[catalogIndex].name.c_str());
      return;
    }
    NetworkStartup::prepare(renderer);
  }
  cancelRequested_ = false;
  workerDone_.store(false);
  progressBytes_.store(0);
  totalBytes_.store(catalog_[catalogIndex].size);
  jobEntry_ = catalog_[catalogIndex];
  job_ = Job::Install;
  state_ = State::Downloading;
  waitStartedMs_ = millis();
  lastWaitPaintMs_ = waitStartedMs_;
  requestUpdateAndWait();
  if (!startWorker(Job::Install)) setError(tr(STR_MEMORY_ERROR), jobEntry_.name.c_str());
}

bool PluginsActivity::startWorker(const Job job) {
  if (worker_ != nullptr) return false;
  constexpr uint32_t kPsramStackBytes = 16 * 1024;
  constexpr uint32_t kInternalStackBytes = 8 * 1024;
  BaseType_t ok = pdFAIL;
#if defined(BOARD_HAS_PSRAM)
  ok = xTaskCreatePinnedToCoreWithCaps(workerTrampoline, "pluginNet", kPsramStackBytes / sizeof(StackType_t), this,
                                       tskIDLE_PRIORITY, &worker_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  workerStackInPsram_ = ok == pdPASS;
#endif
  if (ok != pdPASS) {
    workerStackInPsram_ = false;
    ok = xTaskCreatePinnedToCore(workerTrampoline, "pluginNet", kInternalStackBytes / sizeof(StackType_t), this,
                                 tskIDLE_PRIORITY, &worker_, 0);
  }
  if (ok != pdPASS) {
    worker_ = nullptr;
    job_ = Job::None;
    return false;
  }
  LOG_INF("PLUG", "catalog worker up (%s), job=%d", workerStackInPsram_ ? "PSRAM" : "internal", static_cast<int>(job));
  return true;
}

void PluginsActivity::workerTrampoline(void* parameter) {
  auto* self = static_cast<PluginsActivity*>(parameter);
  {
    background_work::Guard guard;
    self->runWorker();
  }
  self->workerDone_.store(true, std::memory_order_release);
  vTaskSuspend(nullptr);
}

void PluginsActivity::runWorker() {
  const Job job = job_;
  vTaskDelay(pdMS_TO_TICKS(1000));
  HalPowerManager::Lock powerLock(/*quiet=*/true);
  if (job == Job::Catalog) {
    std::string text;
    text.reserve(2048);
    const bool ok = HttpDownloader::fetchUrl(
        native_plugins::catalogUrl(),
        [this, &text](const uint8_t* data, const size_t len) {
          if (cancelRequested_) return false;
          if (text.size() + len > native_plugins::kCatalogCap) return false;
          text.append(reinterpret_cast<const char*>(data), len);
          progressBytes_.store(text.size(), std::memory_order_relaxed);
          return true;
        },
        "", "", {}, 15000, &cancelRequested_);
    if (!ok) {
      jobError_ = cancelRequested_ ? "cancelled" : "catalog download failed";
      jobOk_ = false;
      return;
    }
    std::vector<native_plugins::CatalogEntry> parsed;
    std::string error;
    if (!native_plugins::parseCatalog(text.data(), text.size(), parsed, error)) {
      jobError_ = error;
      jobOk_ = false;
      return;
    }
    native_plugins::saveCatalogCache(text.data(), text.size());
    jobCatalog_ = std::move(parsed);
    jobOk_ = true;
    return;
  }

  jobOk_ = native_plugins::installCatalogEntry(
      jobEntry_,
      [this](const size_t downloaded, const size_t total) {
        progressBytes_.store(downloaded, std::memory_order_relaxed);
        totalBytes_.store(total, std::memory_order_relaxed);
      },
      &cancelRequested_, jobError_);
}

void PluginsActivity::reapWorker() {
  if (worker_ == nullptr) return;
  if (workerStackInPsram_) {
    vTaskDeleteWithCaps(worker_);
  } else {
    vTaskDelete(worker_);
  }
  worker_ = nullptr;
  workerStackInPsram_ = false;
}

void PluginsActivity::pollWorker() {
  if (worker_ == nullptr || !workerDone_.load(std::memory_order_acquire)) return;
  reapWorker();
  releaseNetwork();
  const Job job = job_;
  job_ = Job::None;
  if (!jobOk_) {
    const bool cancelled = cancelRequested_;
    if (cancelled) {
      state_ = State::List;
      requestUpdate(true);
      return;
    }
    setError(tr(STR_DOWNLOAD_FAILED), jobError_.c_str());
    return;
  }
  if (job == Job::Catalog) {
    catalog_ = std::move(jobCatalog_);
    catalogLoaded_ = true;
    state_ = State::List;
    rescan();
    requestUpdate(true);
    return;
  }
  state_ = State::List;
  rescan();
  requestUpdate(true);
}

void PluginsActivity::loop() {
  if (state_ != State::List) {
    if (state_ == State::Fetching || state_ == State::Downloading) {
      pollWorker();
      if (state_ != State::List) {
        int x = 0;
        int y = 0;
        if (mappedInput.wasScreenTapped(x, y) && waitCancelRect_.width > 0 && x >= waitCancelRect_.x &&
            x < waitCancelRect_.x + waitCancelRect_.width && y >= waitCancelRect_.y &&
            y < waitCancelRect_.y + waitCancelRect_.height) {
          cancelRequested_ = true;
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
          cancelRequested_ = true;
        }
        if (static_cast<uint32_t>(millis() - lastWaitPaintMs_) >= 250) {
          lastWaitPaintMs_ = millis();
          requestUpdate();
        }
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state_ = State::List;
      errorTitle_.clear();
      errorBody_.clear();
      requestUpdate(true);
    }
    return;
  }
  UiListActivity::loop();
}

void PluginsActivity::drawFooter() {
  if (state_ == State::Fetching || state_ == State::Downloading) {
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  if (state_ == State::Error) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  UiListActivity::drawFooter();
}

bool PluginsActivity::paintOverlayBeforePresent() {
  if (state_ == State::Fetching || state_ == State::Downloading) {
    const size_t total = totalBytes_.load(std::memory_order_relaxed);
    const size_t done = progressBytes_.load(std::memory_order_relaxed);
    const int permille = total > 0 ? static_cast<int>(std::min<size_t>(1000, done * 1000 / total)) : -1;
    const char* detail = state_ == State::Fetching ? native_plugins::catalogUrl() : jobEntry_.name.c_str();
    const status_card::Layout layout =
        status_card::paintWait(renderer, state_ == State::Fetching ? tr(STR_CHECKING_UPDATE) : tr(STR_DOWNLOADING),
                               detail, nullptr, permille, true, millis() - waitStartedMs_);
    waitCancelRect_ = layout.cancel;
    return true;
  }
  if (state_ == State::Error) {
    status_card::paintMessage(renderer, errorTitle_.c_str(), errorBody_.c_str());
    return true;
  }
  if (confirm_.active()) {
    confirm_.paint(renderer);
    return true;
  }
  return false;
}

void PluginsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (state_ != State::List) return;

  fui::ListProps props;
  props.items = rows_.data();
  props.count = static_cast<uint16_t>(rows_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;
  configureSwipeDelete(screen, props);
  syncListViewport(screen, props, 0);
  screen.list(props);
}

#endif  // CROSSPOINT_PLUGIN_RUNTIME
