#include "plus_resources_activity.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "BackgroundWork.h"
#include "activities/network/WifiSelectionActivity.h"
#include "FontCacheManager.h"
#include "LibraryBuilder.h"
#include "Logging.h"
#include "NetworkStartup.h"
#include "SdCardFontSystem.h"
#include "WifiCredentialStore.h"
#include "components/StatusCards.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kWorkerWaitMs = 50000;

bool hit(const Rect& r, const int x, const int y) {
  return r.width > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

// activation.message is the vendor's binding entry. Some upstream profiles
// return a placeholder host (example.com, xiaozhi.server.com) that does not
// resolve for a phone on the LAN; rendering it as a QR only sends the user to a
// dead page. Show the code and instructions instead in that case.
bool usableBindUrl(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return false;
  const std::string prefix = url.substr(0, scheme);
  if (prefix != "http" && prefix != "https") return false;
  const size_t host_start = scheme + 3;
  const size_t host_end = url.find_first_of("/?#", host_start);
  std::string host = url.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
  for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (host.empty() || host == "example.com" || host == "www.example.com" || host == "xiaozhi.server.com" ||
      host == "www.xiaozhi.server.com") {
    return false;
  }
  return true;
}

const char* itemTypeText(const plus_resources::Item& item) {
  switch (item.type) {
    case plus_resources::Type::Book:
      return tr(STR_BOOK);
    case plus_resources::Type::Badge:
      return item.community_type == "wallpaper" ? tr(STR_IMAGES) : tr(STR_RESOURCE_BADGE);
    case plus_resources::Type::Font:
      return tr(STR_FONT);
    default:
      return "";
  }
}
}  // namespace

void PlusResourcesActivity::onEnter() {
  UiListActivity::onEnter();
  app.on(ACTION_USER, &PlusResourcesActivity::onCancel, this);
  rows_dirty_ = true;
  state_ = State::List;
  // The page is a status page first: if the device is already online, refresh
  // the binding from the server automatically so a successful mini-program
  // binding is reflected without an extra tap.
  auto_probe_pending_ = plus_resources::configured();
  requestUpdate();
  LOG_INF("RES", "page: key=%d source=%s device=%s version=%s", plus_resources::configured() ? 1 : 0,
          plus_resources::keySource(), plus_resources::deviceId(), plus_resources::currentVersion());
}

void PlusResourcesActivity::onExit() {
  if (worker_ != nullptr) {
    cancel_requested_ = true;
    const uint32_t deadline = millis() + kWorkerWaitMs;
    while (worker_ != nullptr && !worker_done_.load(std::memory_order_acquire) &&
           static_cast<int32_t>(deadline - millis()) > 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (worker_ != nullptr && !worker_done_.load(std::memory_order_acquire)) {
      // The worker holds `this` and can be inside TLS or an SD write. Deleting
      // it here would corrupt the heap; a reset is the safe recovery.
      LOG_ERR("RES", "worker did not stop; restarting instead of freeing its owner");
      esp_restart();
    }
    if (worker_ != nullptr) reapWorker();
  }
  releaseOwnedRadio();
  UiListActivity::onExit();
}

int PlusResourcesActivity::listCount() const { return 2 + static_cast<int>(items_.size()); }

const char* PlusResourcesActivity::headerTitle() const { return tr(STR_DEVICE_RESOURCES); }

void PlusResourcesActivity::drawFooter() {
  if (bind_guide_ || state_ == State::Connecting) return;
  if (state_ == State::Binding || state_ == State::Fetching || state_ == State::Downloading) {
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  if (state_ == State::Error) return;  // drawErrorCard() owns the error footer.
  UiListActivity::drawFooter();
}

void PlusResourcesActivity::rebuildRows() {
  constexpr int kActionRows = 2;
  row_labels_.clear();
  row_subtitles_.clear();
  row_values_.clear();
  row_items_.clear();
  row_labels_.reserve(kActionRows + items_.size());
  row_subtitles_.reserve(kActionRows + items_.size());
  row_values_.reserve(kActionRows + items_.size());
  row_items_.reserve(kActionRows + items_.size());

  row_labels_.push_back(bound_ ? tr(STR_DEVICE_BOUND) : tr(STR_BIND_DEVICE));
  if (bound_) {
    row_subtitles_.push_back(tr(STR_CONNECTED));
    row_values_.push_back(tr(STR_DONE));
  } else if (!activation_code_.empty()) {
    row_subtitles_.push_back(tr(STR_ACTIVATION_CODE));
    row_values_.push_back(activation_code_);
  } else {
    row_subtitles_.push_back(bind_message_.empty()
                                 ? (plus_resources::configured() ? tr(STR_BIND_HINT) : tr(STR_RESOURCE_KEY_MISSING))
                                 : bind_message_);
    row_values_.push_back("-");
  }

  row_labels_.push_back(tr(STR_FETCH));
  if (!fetched_) {
    row_subtitles_.push_back(tr(STR_DEVICE_RESOURCES));
  } else if (items_.empty()) {
    row_subtitles_.push_back(tr(STR_NO_RESOURCES));
  } else {
    row_subtitles_.push_back(std::to_string(items_.size()));
  }
  row_values_.push_back(tr(STR_FETCH));

  for (size_t i = 0; i < items_.size(); ++i) {
    const auto& item = items_[i];
    row_labels_.push_back(item.display_name.empty() ? item.name.c_str() : item.display_name.c_str());
    std::string sub = itemTypeText(item);
    if (item.file_size != 0) {
      sub += " · ";
      sub += plus_resources::formatBytes(item.file_size);
    }
    row_subtitles_.push_back(std::move(sub));
    if (!item.supported || item.state == plus_resources::ItemState::Unsupported) {
      row_values_.push_back(tr(STR_UNSUPPORTED_RESOURCE));
    } else if (item.state == plus_resources::ItemState::SavedNotAcknowledged) {
      row_values_.push_back(tr(STR_RESOURCE_ACK_FAILED));
    } else {
      row_values_.push_back(tr(STR_DOWNLOAD));
    }
  }

  for (size_t i = 0; i < row_labels_.size(); ++i) {
    fui::ListItem item;
    item.label = row_labels_[i].c_str();
    item.subtitle = row_subtitles_[i].c_str();
    item.value = row_values_[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    row_items_.push_back(item);
  }
  rows_dirty_ = false;
}

void PlusResourcesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (state_ != State::List) {
    if (mappedInput.hasTouch() &&
        (state_ == State::Binding || state_ == State::Fetching || state_ == State::Downloading)) {
      fui::FooterAction action[1] = {{tr(STR_CANCEL), ACTION_USER}};
      screen.footer(action, 1);
    }
    return;
  }
  if (rows_dirty_) rebuildRows();
  if (row_items_.empty()) {
    screen.centeredText(tr(STR_NO_RESOURCES), screen.theme().bodyText);
    return;
  }
  fui::ListProps props;
  props.items = row_items_.data();
  props.count = static_cast<uint16_t>(row_items_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void PlusResourcesActivity::activateIndex(const int index) {
  if (state_ != State::List) return;
  app.clearTapFlash();
  if (index == 0) {
    startBind();
    return;
  }
  if (index == 1) {
    startFetch();
    return;
  }
  const int item_index = index - 2;
  if (item_index < 0 || item_index >= static_cast<int>(items_.size())) return;
  if (!items_[item_index].supported || items_[item_index].state == plus_resources::ItemState::Unsupported) {
    setError(tr(STR_UNSUPPORTED_RESOURCE));
    return;
  }
  startDownload(item_index);
}

void PlusResourcesActivity::startBind() {
  if (busy()) return;
  if (!plus_resources::configured()) {
    setError(tr(STR_RESOURCE_KEY_MISSING));
    return;
  }
  queueJob(PendingAction::Bind, -1);
}

void PlusResourcesActivity::startFetch() {
  if (busy()) return;
  if (!plus_resources::configured()) {
    setError(tr(STR_RESOURCE_KEY_MISSING));
    return;
  }
  queueJob(PendingAction::Fetch, -1);
}

void PlusResourcesActivity::startDownload(const int item_index) {
  if (busy()) return;
  if (item_index < 0 || item_index >= static_cast<int>(items_.size())) return;
  if (!items_[item_index].supported || items_[item_index].state == plus_resources::ItemState::Unsupported) {
    setError(tr(STR_UNSUPPORTED_RESOURCE));
    return;
  }
  queueJob(PendingAction::Download, item_index);
}

void PlusResourcesActivity::queueJob(const PendingAction action, const int item_index) {
  pending_action_ = action;
  pending_index_ = item_index;
  // Connecting can take up to 15 s. Put the wait card on screen first, then
  // block on the saved AP; otherwise the page looks frozen on the first tap.
  state_ = State::Connecting;
  job_started_ms_ = millis();
  last_paint_ms_ = job_started_ms_;
  requestUpdateAndWait();
  if (!ensureWifi(/*allow_picker=*/true)) {
    pending_action_ = PendingAction::None;
    pending_index_ = -1;
    setError(tr(STR_FONT_EXTRAS_NEED_WIFI));
  }
}

bool PlusResourcesActivity::ensureWifi(const bool allow_picker) {
  // Wi-Fi is required before the worker starts. Keep the time-sync pause for
  // the whole session, then try the last saved AP silently. If that is not
  // possible, hand the user the normal Wi-Fi picker instead of failing with a
  // generic error: the resource action stays queued until the sheet returns.
  if (!time_sync_suspended_) {
    NetworkStartup::suspendTimeSync();
    time_sync_suspended_ = true;
  }
  if (static_cast<int>(WiFi.getMode()) != static_cast<int>(WIFI_MODE_NULL) && WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return true;
  }

  bool connectedSilently = false;
  {
    HalPowerManager::Lock power_lock(/*quiet=*/true);
    WIFI_STORE.loadFromFile();
    const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
    if (credential.has_value()) {
      LOG_INF("RES", "connecting saved network %.32s", credential->ssid.c_str());
      NetworkStartup::prepare(renderer);
      if (NetworkStartup::setMode(renderer, WIFI_STA)) {
        WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
        const uint32_t deadline = millis() + kConnectTimeoutMs;
        while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(deadline - millis()) > 0) {
          delay(100);
        }
        connectedSilently = WiFi.status() == WL_CONNECTED;
        LOG_INF("RES", "saved network connect %s", connectedSilently ? "ok" : "failed");
      }
      if (!connectedSilently) NetworkStartup::releaseRadio();
    } else {
      LOG_INF("RES", "no saved network%s", allow_picker ? "; opening the Wi-Fi picker" : "");
    }
  }
  if (connectedSilently) {
    wifi_owned_ = true;
    onWifiSelectionComplete(true);
    return true;
  }
  if (!allow_picker) return false;

  wifi_owned_ = true;
  if (!startActivityForResultWith<WifiSelectionActivity>(
          [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); })) {
    wifi_owned_ = false;
    pending_action_ = PendingAction::None;
    pending_index_ = -1;
    setError(tr(STR_FONT_EXTRAS_NEED_WIFI));
    return false;
  }
  return true;
}

void PlusResourcesActivity::onWifiSelectionComplete(const bool success) {
  const PendingAction action = pending_action_;
  const int item_index = pending_index_;
  if (!success) {
    pending_action_ = PendingAction::None;
    pending_index_ = -1;
    if (action == PendingAction::Probe) {
      // An automatic check must never turn into an error page: the user only
      // opened the resource page.
      state_ = State::List;
      bind_message_ = tr(STR_WIFI_CONN_FAILED);
      rows_dirty_ = true;
      requestUpdate(true);
      return;
    }
    setError(tr(STR_FONT_EXTRAS_NEED_WIFI));
    return;
  }
  pending_action_ = PendingAction::None;
  pending_index_ = -1;
  if (action == PendingAction::Bind) {
    beginBindJob();
  } else if (action == PendingAction::Fetch) {
    beginFetchJob();
  } else if (action == PendingAction::Download) {
    beginDownloadJob(item_index);
  } else if (action == PendingAction::Probe) {
    beginProbeJob();
  }
}

void PlusResourcesActivity::beginBindJob() {
  if (busy()) return;
  state_ = State::Binding;
  job_ = Job::Bind;
  job_index_ = -1;
  retry_job_ = Job::None;
  retry_index_ = -1;
  cancel_requested_ = false;
  worker_done_.store(false, std::memory_order_relaxed);
  progress_bytes_.store(0);
  progress_total_.store(0);
  stage_.store(1);
  job_started_ms_ = millis();
  last_paint_ms_ = job_started_ms_;
  requestUpdateAndWait();
  if (!startWorker(Job::Bind, -1)) setError(tr(STR_MEMORY_ERROR));
}

void PlusResourcesActivity::beginFetchJob() {
  if (busy()) return;
  state_ = State::Fetching;
  job_ = Job::Fetch;
  job_index_ = -1;
  retry_job_ = Job::None;
  retry_index_ = -1;
  cancel_requested_ = false;
  worker_done_.store(false, std::memory_order_relaxed);
  progress_bytes_.store(0);
  progress_total_.store(0);
  stage_.store(1);
  job_started_ms_ = millis();
  last_paint_ms_ = job_started_ms_;
  requestUpdateAndWait();
  if (!startWorker(Job::Fetch, -1)) setError(tr(STR_MEMORY_ERROR));
}

void PlusResourcesActivity::beginDownloadJob(const int item_index) {
  if (busy()) return;
  if (item_index < 0 || item_index >= static_cast<int>(items_.size())) return;
  job_item_ = items_[item_index];
  download_title_ = job_item_.display_name.empty() ? std::string(job_item_.name.c_str()) : std::string(job_item_.display_name.c_str());
  state_ = State::Downloading;
  job_ = Job::Download;
  job_index_ = item_index;
  retry_job_ = Job::None;
  retry_index_ = -1;
  cancel_requested_ = false;
  worker_done_.store(false, std::memory_order_relaxed);
  progress_bytes_.store(0);
  progress_total_.store(static_cast<size_t>(job_item_.file_size));
  stage_.store(1);
  job_started_ms_ = millis();
  last_paint_ms_ = job_started_ms_;
  requestUpdateAndWait();
  if (!startWorker(Job::Download, item_index)) setError(tr(STR_MEMORY_ERROR));
}

void PlusResourcesActivity::scheduleProbe() {
  if (probe_deadline_ms_ == 0 || !bind_guide_) {
    // The activation code is valid for a day; keeping the guide polling for
    // 15 minutes covers the whole "open the mini-program and type the code"
    // flow instead of stopping after two minutes and leaving the exchange
    // undone.
    probe_deadline_ms_ = millis() + 15UL * 60UL * 1000UL;
    probe_attempts_ = 0;
  }
  next_probe_ms_ = millis() + (retry_pairing_ ? 10000 : 4000);
}

void PlusResourcesActivity::beginProbeJob() {
  if (worker_ != nullptr || job_ != Job::None) return;
  ++probe_attempts_;
  job_ = Job::Probe;
  job_index_ = -1;
  cancel_requested_ = false;
  worker_done_.store(false, std::memory_order_relaxed);
  stage_.store(1);
  job_started_ms_ = millis();
  requestUpdate();
  if (!startWorker(Job::Probe, -1)) setError(tr(STR_MEMORY_ERROR));
}

void PlusResourcesActivity::startProbe(const bool user_initiated, const bool from_enter) {
  if (worker_ != nullptr || job_ != Job::None) return;
  if (!user_initiated && (probe_attempts_ >= 225 || static_cast<int32_t>(millis() - probe_deadline_ms_) >= 0)) {
    return;
  }
  probe_from_enter_ = from_enter;
  probe_polls_tokens_ = bind_guide_ && (!activation_code_.empty() || retry_pairing_);
  if (WiFi.status() != WL_CONNECTED) {
    if (!user_initiated) return;
    queueJob(PendingAction::Probe, -1);
    return;
  }
  beginProbeJob();
}

bool PlusResourcesActivity::startWorker(const Job job, const int item_index) {
  (void)item_index;
  if (worker_ != nullptr) return false;
  NetworkStartup::prepare(renderer);
  {
    const RenderLock lock;
    if (auto* font_cache = renderer.getFontCacheManager()) {
      font_cache->clearCache();
      font_cache->releaseSdFontCaches();
    }
    // A background binding probe keeps the current rows on screen; only an
    // explicit list-changing job drops them.
    if (job != Job::Probe) {
      std::vector<fui::ListItem>().swap(row_items_);
      std::vector<std::string>().swap(row_labels_);
      std::vector<std::string>().swap(row_subtitles_);
      std::vector<std::string>().swap(row_values_);
      rows_dirty_ = true;
    }
  }
  job_ = job;
  worker_done_.store(false, std::memory_order_relaxed);
  constexpr uint32_t kPsramStackBytes = 24 * 1024;
  constexpr uint32_t kInternalStackBytes = 12 * 1024;
  BaseType_t ok = pdFAIL;
#if defined(BOARD_HAS_PSRAM)
  ok = xTaskCreatePinnedToCoreWithCaps(workerTrampoline, "plusRes", kPsramStackBytes / sizeof(StackType_t), this,
                                       tskIDLE_PRIORITY, &worker_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  worker_stack_in_psram_ = ok == pdPASS;
  if (ok != pdPASS) LOG_ERR("RES", "PSRAM worker stack refused (%d); using internal RAM", static_cast<int>(ok));
#endif
  if (ok != pdPASS) {
    worker_stack_in_psram_ = false;
    ok = xTaskCreatePinnedToCore(workerTrampoline, "plusRes", kInternalStackBytes / sizeof(StackType_t), this,
                                 tskIDLE_PRIORITY, &worker_, 0);
  }
  if (ok != pdPASS) {
    worker_ = nullptr;
    job_ = Job::None;
    return false;
  }
  LOG_INF("RES", "worker up: stack in %s, free=%u largest=%u", worker_stack_in_psram_ ? "PSRAM" : "INTERNAL RAM",
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  return true;
}

void PlusResourcesActivity::workerTrampoline(void* param) {
  auto* self = static_cast<PlusResourcesActivity*>(param);
  {
    background_work::Guard guard;
    self->runWorker();
  }
  self->worker_done_.store(true, std::memory_order_release);
  vTaskSuspend(nullptr);
}

void PlusResourcesActivity::progressThunk(const size_t done, const size_t total, void* ctx) {
  auto* self = static_cast<PlusResourcesActivity*>(ctx);
  self->progress_bytes_.store(done);
  self->progress_total_.store(total);
  if (done > 0) self->stage_.store(2, std::memory_order_relaxed);
}

void PlusResourcesActivity::runWorker() {
  vTaskDelay(pdMS_TO_TICKS(1000));
  HalPowerManager::Lock power_lock(/*quiet=*/true);
  stage_.store(1);
  if (job_ == Job::Bind) {
    bind_result_ = plus_resources::bindDevice();
  } else if (job_ == Job::Probe && probe_polls_tokens_) {
    bind_result_ = plus_resources::bindDevice();
  } else if (job_ == Job::Fetch || job_ == Job::Probe) {
    fetch_result_ = plus_resources::fetchResources();
  } else if (job_ == Job::Download) {
    transfer_result_ = plus_resources::downloadAndAck(job_item_, &PlusResourcesActivity::progressThunk, this,
                                                       &cancel_requested_);
  }
  stage_.store(3);
}

void PlusResourcesActivity::reapWorker() {
  if (worker_ == nullptr) return;
  if (worker_stack_in_psram_) {
    vTaskDeleteWithCaps(worker_);
  } else {
    vTaskDelete(worker_);
  }
  worker_ = nullptr;
  worker_stack_in_psram_ = false;
}

void PlusResourcesActivity::releaseOwnedRadio() {
  if (wifi_owned_) {
    NetworkStartup::releaseRadio();
    wifi_owned_ = false;
  }
  if (time_sync_suspended_) {
    NetworkStartup::resumeTimeSync();
    time_sync_suspended_ = false;
  }
}

void PlusResourcesActivity::pollWorker() {
  if (worker_ == nullptr || !worker_done_.load(std::memory_order_acquire)) return;
  reapWorker();
  // Keep the STA link while the binding guide is open; the probe loop would
  // otherwise pay a full reconnect for every check. onExit() releases it.
  const Job finished = job_;
  job_ = Job::None;
  job_index_ = -1;
  stage_.store(0);
  const bool cancelled = cancel_requested_;
  const bool from_enter = probe_from_enter_;
  cancel_requested_ = false;
  probe_from_enter_ = false;
  if (cancelled) {
    state_ = State::List;
    rows_dirty_ = true;
    requestUpdate(true);
    return;
  }
  if (finished == Job::Bind) {
    probe_polls_tokens_ = false;
    if (!bind_result_.ok) {
      if (bind_result_.http == 409) {
        retry_pairing_ = true;
        bind_guide_ = true;
        activation_code_.clear();
        bind_message_ = bind_result_.error.empty() ? tr(STR_NOT_BOUND) : bind_result_.error;
        state_ = State::List;
        rows_dirty_ = true;
        scheduleProbe();
        requestUpdate(true);
        return;
      }
      setError(bind_result_.error.empty() ? tr(STR_ERROR_GENERAL_FAILURE) : bind_result_.error, Job::Bind);
      return;
    }
    retry_pairing_ = false;
    if (bind_result_.code.empty()) {
      // The upstream omits activation for a device it already knows. Trust the
      // resource service (not the field's absence) and check the binding.
      bind_guide_ = false;
      bound_ = false;
      state_ = State::List;
      rows_dirty_ = true;
      requestUpdate(true);
      startProbe(true);
      return;
    }
    activation_code_ = bind_result_.code;
    bind_message_.clear();
    bind_url_ = bind_result_.bind_url;
    bound_ = false;
    bind_guide_ = true;
    state_ = State::List;
    rows_dirty_ = true;
    scheduleProbe();
    requestUpdate(true);
    return;
  }
  if (finished == Job::Probe && probe_polls_tokens_) {
    probe_polls_tokens_ = false;
    if (!bind_result_.ok) {
      if (bind_result_.http == 409) {
        retry_pairing_ = true;
        bind_guide_ = true;
        activation_code_.clear();
        bind_message_ = bind_result_.error.empty() ? tr(STR_NOT_BOUND) : bind_result_.error;
        state_ = State::List;
        rows_dirty_ = true;
        scheduleProbe();
        requestUpdate(true);
        return;
      }
      setError(bind_result_.error.empty() ? tr(STR_ERROR_GENERAL_FAILURE) : bind_result_.error, Job::Probe);
      return;
    }
    retry_pairing_ = false;
    if (bind_result_.code.empty()) {
      // The deviceCode was exchanged (or the device was already bound): the
      // tokens are on disk now, so fetch the delivery list without waiting for
      // another tap.
      bind_guide_ = false;
      retry_pairing_ = false;
      activation_code_.clear();
      bind_url_.clear();
      bind_message_.clear();
      bound_ = false;
      state_ = State::List;
      rows_dirty_ = true;
      requestUpdate(true);
      startProbe(true);
      return;
    }
    activation_code_ = bind_result_.code;
    bind_message_.clear();
    bound_ = false;
    state_ = State::List;
    rows_dirty_ = true;
    scheduleProbe();
    requestUpdate(true);
    return;
  }
  if (finished == Job::Fetch || finished == Job::Probe) {
    if (!fetch_result_.ok) {
      if (fetch_result_.business_code == 500 || fetch_result_.business_code == 401) {
        // The resource service uses business code 500 for "no binding" (not a
        // transient HTTP error). Keep the server text instead of replacing it
        // with a generic label: it tells the user whether the device is
        // missing, unbound, or has conflicting bindings.
        bound_ = false;
        bind_message_ = fetch_result_.error.empty() ? tr(STR_NOT_BOUND) : fetch_result_.error;
        bind_guide_ = !from_enter;
        state_ = State::List;
        rows_dirty_ = true;
        if (bind_guide_) scheduleProbe();
        requestUpdate(true);
        return;
      }
      if (from_enter) {
        state_ = State::List;
        bind_message_ = fetch_result_.error.empty() ? tr(STR_ERROR_GENERAL_FAILURE) : fetch_result_.error;
        rows_dirty_ = true;
        requestUpdate(true);
        return;
      }
      setError(fetch_result_.error.empty() ? tr(STR_ERROR_GENERAL_FAILURE) : fetch_result_.error,
               finished == Job::Probe ? Job::Probe : Job::Fetch);
      return;
    }
    bound_ = true;
    bind_guide_ = false;
    activation_code_.clear();
    bind_url_.clear();
    bind_message_.clear();
    items_ = std::move(fetch_result_.items);
    fetched_ = true;
    state_ = State::List;
    rows_dirty_ = true;
    requestUpdate(true);
    return;
  }
  if (finished == Job::Download) {
    const int index = job_index_;
    if (!transfer_result_.ok) {
      if (transfer_result_.unsupported) {
        if (index >= 0 && index < static_cast<int>(items_.size())) {
          items_[index].state = plus_resources::ItemState::Unsupported;
        }
        state_ = State::List;
        rows_dirty_ = true;
        requestUpdate(true);
      } else {
        setError(transfer_result_.error.empty() ? tr(STR_DOWNLOAD_FAILED) : transfer_result_.error, Job::Download,
                 index);
      }
      return;
    }
    if (index >= 0 && index < static_cast<int>(items_.size())) {
      if (!transfer_result_.acked) {
        items_[index].state = plus_resources::ItemState::SavedNotAcknowledged;
      } else {
        if (items_[index].type == plus_resources::Type::Book) {
          library::markLibraryIndexDirty();
        } else if (items_[index].type == plus_resources::Type::Font) {
          sdFontSystem.markRegistryDirty();
          sdFontSystem.refreshIfDirty();
        }
        items_.erase(items_.begin() + index);
      }
    }
    state_ = State::List;
    rows_dirty_ = true;
    requestUpdate(true);
  }
}

void PlusResourcesActivity::setError(std::string message, const Job retry_job, const int retry_index) {
  {
    RenderLock lock(*this);
    error_message_ = std::move(message);
    retry_job_ = retry_job;
    retry_index_ = retry_index;
    state_ = State::Error;
    job_ = Job::None;
  }
  LOG_ERR("RES", "error: %s", error_message_.c_str());
  requestUpdate(true);
}

void PlusResourcesActivity::retryLastJob() {
  const Job retry = retry_job_;
  const int index = retry_index_;
  retry_job_ = Job::None;
  retry_index_ = -1;
  if (retry == Job::Bind) {
    startBind();
  } else if (retry == Job::Fetch) {
    startFetch();
  } else if (retry == Job::Download) {
    startDownload(index);
  }
}

void PlusResourcesActivity::onCancel(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<PlusResourcesActivity*>(user);
  if (self->state_ != State::Connecting && self->state_ != State::Binding && self->state_ != State::Fetching &&
      self->state_ != State::Downloading) {
    return;
  }
  if (self->cancel_requested_) return;
  self->cancel_requested_ = true;
  self->requestUpdate(true);
}

void PlusResourcesActivity::pollCancelInput() {
  mappedInput.update();
  int tap_x = 0;
  int tap_y = 0;
  if (mappedInput.wasScreenTapped(tap_x, tap_y) && hit(wait_cancel_rect_, tap_x, tap_y) && !cancel_requested_) {
    cancel_requested_ = true;
    requestUpdate(true);
  }
  if ((mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) &&
      !cancel_requested_) {
    cancel_requested_ = true;
    requestUpdate(true);
  }
  UiAppHost::routeTouch(mappedInput);
  if (app.invalidated()) requestUpdate();
}

void PlusResourcesActivity::drawWait() {
  const uint32_t elapsed = millis() - job_started_ms_;
  const size_t done = progress_bytes_.load();
  const size_t total = progress_total_.load();
  const int stage = stage_.load();
  int permille = 80 + std::min<int>(300, static_cast<int>(elapsed / 60));
  std::string detail = tr(STR_LOADING);
  if (cancel_requested_) {
    detail = tr(STR_CANCEL);
  } else if (state_ == State::Connecting) {
    detail = tr(STR_CONNECTING_SAVED_WIFI);
  } else if (state_ == State::Downloading) {
    if (total != 0) {
      permille = 400 + static_cast<int>(std::min<uint64_t>(550, static_cast<uint64_t>(done) * 550 / total));
      detail = plus_resources::formatBytes(done) + " / " + plus_resources::formatBytes(total);
    } else {
      permille = 400 + std::min(500, static_cast<int>(done / 2048) * 100);
      detail = plus_resources::formatBytes(done);
    }
  } else if (state_ == State::Fetching) {
    detail = tr(STR_LOADING);
  } else if (state_ == State::Binding) {
    detail = tr(STR_BIND_DEVICE);
  }
  if (stage >= 3) permille = 940;
  permille = std::max(0, std::min(1000, permille));
  const char* title = state_ == State::Connecting ? tr(STR_DEVICE_RESOURCES)
                      : state_ == State::Binding ? tr(STR_BIND_DEVICE)
                      : state_ == State::Fetching ? tr(STR_DEVICE_RESOURCES)
                                                  : tr(STR_DOWNLOADING);
  const char* subtitle = state_ == State::Downloading ? download_title_.c_str() : nullptr;
  wait_cancel_rect_ = status_card::paintWait(renderer, title, subtitle, detail.c_str(), permille,
                                             /*withCancel=*/state_ != State::Connecting, elapsed)
                          .cancel;
}

void PlusResourcesActivity::drawBindGuide() {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int cardW = std::min(640, safe.width - 24);
  const int cardH = std::min(560, safe.height - 24);
  const int cardX = safe.x + (safe.width - cardW) / 2;
  const int cardY = safe.y + (safe.height - cardH) / 2;
  GUI.drawModalScrim(renderer);
  GUI.drawModalCardFrame(renderer, Rect{cardX, cardY, cardW, cardH});

  constexpr int kPad = 24;
  constexpr int kGap = 8;
  const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int line18 = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  int x = cardX + kPad;
  int y = cardY + 18;
  renderer.drawText(UI_12_FONT_ID, x, y, tr(STR_BIND_DEVICE), true, EpdFontFamily::BOLD);

  // Status chip at the top-right; the guide is a status surface, not a form.
  const char* chip = busy() ? tr(STR_LOADING) : bound_ ? tr(STR_DEVICE_BOUND) : tr(STR_NOT_BOUND);
  const int chipW = std::min(150, renderer.getTextWidth(UI_10_FONT_ID, chip) + 24);
  const Rect chipRect{cardX + cardW - kPad - chipW, y - 4, chipW, line10 + 12};
  renderer.drawRoundedRect(chipRect.x, chipRect.y, chipRect.width, chipRect.height, 1, chipRect.height / 2, true);
  renderer.drawText(UI_10_FONT_ID, chipRect.x + (chipRect.width - renderer.getTextWidth(UI_10_FONT_ID, chip)) / 2,
                    chipRect.y + 6, chip);
  y += line12 + 14;

  const bool show_qr = usableBindUrl(bind_url_);
  const int qrSize = show_qr ? std::min(184, cardH - 230) : 0;
  const int leftW = cardW - 2 * kPad - (show_qr ? qrSize + 20 : 0);
  const int leftX = x;

  // Six large boxes read as a code from a distance; a plain string in the
  // row below the title was the main "not intuitive" complaint.
  if (activation_code_.size() == 6) {
    renderer.drawText(UI_10_FONT_ID, leftX, y, tr(STR_ACTIVATION_CODE));
    y += line10 + 8;
    const int boxW = std::min(56, (leftW - 5 * kGap) / 6);
    const int boxH = 72;
    const int totalW = 6 * boxW + 5 * kGap;
    int bx = leftX + (leftW - totalW) / 2;
    for (size_t i = 0; i < activation_code_.size(); ++i) {
      renderer.drawRoundedRect(bx, y, boxW, boxH, 2, 12, true);
      const std::string digit(1, activation_code_[i]);
      const int tw = renderer.getTextWidth(NOTOSANS_18_FONT_ID, digit.c_str());
      renderer.drawText(NOTOSANS_18_FONT_ID, bx + (boxW - tw) / 2, y + (boxH - line18) / 2, digit.c_str(), true,
                        EpdFontFamily::BOLD);
      bx += boxW + kGap;
    }
    y += boxH + 14;
  } else {
    renderer.drawRoundedRect(leftX, y, leftW, 72, 2, 12, true);
    const char* empty = tr(STR_NOT_BOUND);
    const int emptyW = renderer.getTextWidth(UI_12_FONT_ID, empty);
    renderer.drawText(UI_12_FONT_ID, leftX + (leftW - emptyW) / 2, y + (72 - line12) / 2, empty, true,
                      EpdFontFamily::BOLD);
    y += 72 + 14;
  }

  const auto steps = renderer.wrappedText(UI_10_FONT_ID, tr(STR_BIND_HINT), leftW, 3);
  for (const auto& line : steps) {
    renderer.drawText(UI_10_FONT_ID, leftX, y, line.c_str());
    y += line10 + 3;
  }
  y += 10;

  const char* status = busy() ? tr(STR_LOADING)
                       : bound_ ? tr(STR_DEVICE_BOUND)
                                : (bind_message_.empty() ? tr(STR_NOT_BOUND) : bind_message_.c_str());
  const auto statusLines = renderer.wrappedText(UI_10_FONT_ID, status, leftW, 4);
  for (const auto& line : statusLines) {
    renderer.drawText(UI_10_FONT_ID, leftX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += line10 + 3;
  }
  if (show_qr) {
    const Rect qrRect{cardX + cardW - kPad - qrSize, cardY + 18 + line12 + 14, qrSize, qrSize};
    QrUtils::drawQrCode(renderer, qrRect, bind_url_);
    int qy = qrRect.y + qrRect.height + 6;
    const auto qrHint = renderer.wrappedText(UI_10_FONT_ID, tr(STR_SCAN_QR_HINT), qrSize, 2);
    for (const auto& line : qrHint) {
      renderer.drawText(UI_10_FONT_ID, qrRect.x, qy, line.c_str());
      qy += line10 + 2;
    }
  }

  constexpr int kButtonH = 54;
  constexpr int kButtonGap = 10;
  constexpr int kDoneW = 210;
  constexpr int kRefreshW = 170;
  constexpr int kBackW = 96;
  const int rowW = kDoneW + kRefreshW + kBackW + 2 * kButtonGap;
  const int rowX = cardX + (cardW - rowW) / 2;
  const int rowY = cardY + cardH - kButtonH - 18;
  bind_check_rect_ = Rect{rowX, rowY, kDoneW, kButtonH};
  bind_retry_rect_ = Rect{rowX + kDoneW + kButtonGap, rowY, kRefreshW, kButtonH};
  bind_back_rect_ = Rect{rowX + kDoneW + kRefreshW + 2 * kButtonGap, rowY, kBackW, kButtonH};
  GUI.drawActionButton(renderer, bind_check_rect_, tr(STR_CHECK_BINDING), true);
  GUI.drawActionButton(renderer, bind_retry_rect_, tr(STR_RETRY), false);
  GUI.drawActionButton(renderer, bind_back_rect_, tr(STR_BACK), false);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CHECK_BINDING), nullptr, nullptr);
  GUI.drawButtonHintsWithStyle(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, false);
}

bool PlusResourcesActivity::handleBindGuideInput() {
  int tapX = 0;
  int tapY = 0;
  const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
  if (tapped && hit(bind_check_rect_, tapX, tapY)) {
    startProbe();
    return true;
  }
  if (tapped && hit(bind_retry_rect_, tapX, tapY)) {
    bind_guide_ = false;
    startBind();
    return true;
  }
  if (tapped && hit(bind_back_rect_, tapX, tapY)) {
    bind_guide_ = false;
    releaseOwnedRadio();
    requestUpdate();
    return true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    bind_guide_ = false;
    releaseOwnedRadio();
    requestUpdate();
    return true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    startProbe();
    return true;
  }
  return true;
}

void PlusResourcesActivity::drawErrorCard() {
  status_card::paintMessage(renderer, tr(STR_DEVICE_RESOURCES), error_message_.c_str());
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int kButtonH = 56;
  constexpr int kRetryW = 200;
  constexpr int kBackW = 140;
  constexpr int kGap = 20;
  const int row_y = safe.y + safe.height - metrics.buttonHintsHeight - kButtonH - 8;
  if (retry_job_ != Job::None) {
    const int row_x = safe.x + (safe.width - (kRetryW + kGap + kBackW)) / 2;
    error_retry_rect_ = Rect{row_x, row_y, kRetryW, kButtonH};
    error_back_rect_ = Rect{row_x + kRetryW + kGap, row_y, kBackW, kButtonH};
    GUI.drawActionButton(renderer, error_retry_rect_, tr(STR_RETRY), true);
    GUI.drawActionButton(renderer, error_back_rect_, tr(STR_BACK), false);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), nullptr, nullptr);
    GUI.drawButtonHintsWithStyle(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, false);
  } else {
    const int row_x = safe.x + (safe.width - kBackW) / 2;
    error_retry_rect_ = Rect{};
    error_back_rect_ = Rect{row_x, row_y, kBackW, kButtonH};
    GUI.drawActionButton(renderer, error_back_rect_, tr(STR_BACK), true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHintsWithStyle(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, false);
  }
}

bool PlusResourcesActivity::handleErrorActions() {
  if (state_ != State::Error) return false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && retry_job_ != Job::None) {
    retryLastJob();
    return true;
  }
  return true;
}

bool PlusResourcesActivity::handleCustomInput() {
  const bool busy_now = state_ == State::Connecting || state_ == State::Binding || state_ == State::Fetching ||
      state_ == State::Downloading;
  int tap_x = 0;
  int tap_y = 0;
  const bool tapped = mappedInput.wasScreenTapped(tap_x, tap_y);
  if (state_ == State::Error) {
    if (tapped && hit(error_retry_rect_, tap_x, tap_y)) {
      retryLastJob();
      return true;
    }
    if (tapped && hit(error_back_rect_, tap_x, tap_y)) {
      finish();
      return true;
    }
    return handleErrorActions() || tapped;
  }
  if (bind_guide_) return handleBindGuideInput();
  if (busy_now) return true;
  return false;
}

bool PlusResourcesActivity::paintOverlayBeforePresent() {
  if (state_ == State::Error) {
    drawErrorCard();
    return true;
  }
  if (bind_guide_) {
    drawBindGuide();
    return true;
  }
  if (state_ == State::Connecting || state_ == State::Binding || state_ == State::Fetching ||
      state_ == State::Downloading) {
    drawWait();
    return true;
  }
  return false;
}

void PlusResourcesActivity::loop() {
  pollWorker();
  if (auto_probe_pending_) {
    auto_probe_pending_ = false;
    if (plus_resources::configured() && !busy()) {
      if (WiFi.status() == WL_CONNECTED) {
        probe_from_enter_ = true;
        scheduleProbe();
        beginProbeJob();
      } else {
        probe_from_enter_ = true;
        state_ = State::Connecting;
        job_started_ms_ = millis();
        last_paint_ms_ = job_started_ms_;
        requestUpdateAndWait();
        pending_action_ = PendingAction::Probe;
        pending_index_ = -1;
        if (!ensureWifi(/*allow_picker=*/false)) {
          pending_action_ = PendingAction::None;
          state_ = State::List;
          bind_message_ = tr(STR_WIFI_CONN_FAILED);
          rows_dirty_ = true;
          requestUpdate(true);
        }
      }
    }
  }
  if (bind_guide_ && !busy() && static_cast<int32_t>(millis() - next_probe_ms_) >= 0 &&
      static_cast<int32_t>(millis() - probe_deadline_ms_) < 0) {
    startProbe(false, false);
  }
  const bool busy_now = state_ == State::Connecting || state_ == State::Binding || state_ == State::Fetching ||
      state_ == State::Downloading;
  if (busy_now) {
    pollCancelInput();
    const uint32_t now = millis();
    if (now - last_paint_ms_ >= 1000) {
      last_paint_ms_ = now;
      requestUpdate();
    }
    return;
  }
  UiListActivity::loop();
}

void PlusResourcesActivity::debugConfirm() {
  if (state_ == State::Error) {
    retryLastJob();
    return;
  }
  if (bind_guide_) {
    startProbe();
    return;
  }
  if (state_ == State::List) activateIndex(nav.selected);
}

void PlusResourcesActivity::debugCancel() {
  if (bind_guide_) {
    bind_guide_ = false;
    releaseOwnedRadio();
    requestUpdate();
    return;
  }
  if (state_ == State::Connecting || state_ == State::Binding || state_ == State::Fetching ||
      state_ == State::Downloading) {
    cancel_requested_ = true;
    return;
  }
  finish();
}
