#include "plus_bt_audio_activity.h"
#include "plus_bt_audio_parse.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <Logging.h>
#include <plus_audio.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "I18n.h"
#include "components/UITheme.h"

namespace {

constexpr unsigned long kDrainIntervalMs = 200;
/* Bytes one drain may collect; the module emits one "AT+BT:" line per device. */
constexpr size_t kDrainBytes = 512;
constexpr unsigned long kLongPressMs = 900;
constexpr int kGap = 8;
// xTaskCreatePinnedToCoreWithCaps counts StackType_t words: 4K words = 16 KiB.
constexpr uint32_t kWorkerStackBytes = 4 * 1024;

/* Worker hand-off lives at file scope, not in the activity: the task self-deletes
 * after publishing its result, so a task that outlives the page (it cannot be
 * killed safely mid-UART) must never dereference freed memory. One page instance
 * exists at a time and jobs never overlap, which is what startJob() enforces. */
struct WorkerCtx {
  int job;          /* PlusBtAudioActivity::Job */
  // Copied, not pointed at: the page may be destroyed while a timed-out worker
  // is still inside the 700 ms UART gaps, and its device vector dies with it.
  char address[64];
  std::atomic<bool> done{true};
  std::atomic<int> error{0};
};
WorkerCtx g_worker{0, {0}};

using plus_bt_audio::isConnectSuccess;
using plus_bt_audio::isConnectTimeout;
using plus_bt_audio::isInquiryComplete;
using plus_bt_audio::parseDeviceLine;

void trim(char* text) {
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) text[--len] = '\0';
}

/* The vendor command flow, one job per entry. Runs on the worker task. */
void runJob(const int job, const char* address) {
  esp_err_t err = ESP_OK;
  switch (job) {
    case static_cast<int>(PlusBtAudioActivity::Job::Prepare):
    case static_cast<int>(PlusBtAudioActivity::Job::Rescan):
      if (!plus_audio_wait_ready(8000)) {
        err = ESP_ERR_TIMEOUT;
        break;
      }
      if (plus_audio_bt_mode() != 2) {
        err = plus_audio_bt_set_mode(2);
        if (err != ESP_OK) break;
      }
      err = plus_audio_bt_scan_start();
      break;
    case static_cast<int>(PlusBtAudioActivity::Job::Connect):
      err = plus_audio_bt_connect(address != nullptr ? address : "");
      break;
    case static_cast<int>(PlusBtAudioActivity::Job::Disconnect):
      /* Vendor "music mode" first (AT+BTSCO=0), then the mode 1 restore. The
       * vendor's follow-up AT+PP=1 resumes playback, so it is not sent here. */
      (void)plus_audio_bt_sco(false);
      err = plus_audio_bt_set_mode(1);
      break;
    default:
      break;
  }
  g_worker.error.store(static_cast<int>(err));
  g_worker.done.store(true);
}

void workerTrampoline(void* /*arg*/) {
  runJob(g_worker.job, g_worker.address);
  vTaskDeleteWithCaps(nullptr);
}

}  // namespace

/* ------------------------------------------------------------------ life -- */

void PlusBtAudioActivity::onEnter() {
  Activity::onEnter();
  deviceCount_ = 0;
  pendingConnectIndex_ = -1;
  connectedIndex_ = -1;
  selected_ = kRescanRow;
  phase_ = Phase::Preparing;
  failReason_ = StrId::STR_BT_CONNECTION_FAILED;
  needsPrepare_ = false;
  exiting_ = false;
  lastDrainMs_ = 0;
  LOG_INF("BTAUD", "page enter: audio=%d mode=%d", static_cast<int>(plus_audio_state()), plus_audio_bt_mode());
  requestUpdate();
  startJob(Job::Prepare, nullptr);
}

void PlusBtAudioActivity::onExit() {
  exiting_ = true;
  finishJob();
  plus_audio_bt_scan_stop();
  /* Leaving the page always returns the module to mode 1, which is what brings
   * the local I2S sink (sound feedback) back. Bounded at ~1.8 s and done on the
   * UI task on purpose: no helper task of ours may outlive this activity. */
  restoreMode1();
  LOG_INF("BTAUD", "page exit: mode=%d", plus_audio_bt_mode());
  Activity::onExit();
}

void PlusBtAudioActivity::prepareForSleep() {
  /* Never let the device doze with the module parked in mode 2: sleep cuts the BT
   * rail and the module would come back in an unknown mode while this page still
   * believes it is in mode 2. Re-prepare when the page resumes. */
  if (phase_ == Phase::Preparing) return;
  needsPrepare_ = true;
  plus_audio_bt_scan_stop();
  restoreMode1();
}

void PlusBtAudioActivity::restoreMode1() {
  if (plus_audio_bt_mode() == 1) return;
  const esp_err_t err = plus_audio_bt_set_mode(1);
  if (err != ESP_OK) LOG_ERR("BTAUD", "mode 1 restore: %s", esp_err_to_name(err));
}

/* ----------------------------------------------------------------- worker -- */

bool PlusBtAudioActivity::workerBusy() const { return worker_ != nullptr || !g_worker.done.load(); }

void PlusBtAudioActivity::startJob(const Job job, const char* address) {
  if (workerBusy()) return;
  job_ = job;
  g_worker.job = static_cast<int>(job);
  g_worker.address[0] = '\0';
  if (address != nullptr) {
    std::strncpy(g_worker.address, address, sizeof(g_worker.address) - 1);
  }
  g_worker.error.store(0);
  g_worker.done.store(false);

  /* PSRAM stack: the worker only talks UART and sleeps through the vendor's
   * 700 ms gaps; it never touches internal flash. With Wi-Fi/TLS resident the
   * internal heap cannot reliably provide an 8 KiB contiguous stack and the
   * page reported STR_BT_LOW_MEMORY. */
  const BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
      workerTrampoline, "plusBtAudio", kWorkerStackBytes, nullptr, tskIDLE_PRIORITY, &worker_, 0,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) {
    worker_ = nullptr;
    job_ = Job::None;
    g_worker.done.store(true);
    LOG_ERR("BTAUD", "worker task creation failed");
    setFailed(StrId::STR_BT_LOW_MEMORY);
    return;
  }
  LOG_INF("BTAUD", "job %d started", static_cast<int>(job));
}

/* Reaps a finished worker and applies its result. The task deletes itself, so the
 * handle is only dropped here - never passed to vTaskDelete(). */
void PlusBtAudioActivity::finishJob() {
  if (worker_ == nullptr) return;
  const unsigned long deadline = millis() + 15000;
  while (!g_worker.done.load() && static_cast<int32_t>(deadline - millis()) > 0) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (!g_worker.done.load()) {
    /* Two 700 ms gaps plus two 1 s acks bound a switch; beyond that the module
     * stopped answering. Keep the handle pending and refuse new jobs rather than
     * deleting a task mid-UART: the worker's context is static, so it is safe for
     * it to finish after the page is gone. */
    LOG_ERR("BTAUD", "worker did not finish; leaving it to exit on its own");
    return;
  }
  worker_ = nullptr;

  const Job done = job_;
  const esp_err_t err = static_cast<esp_err_t>(g_worker.error.load());
  job_ = Job::None;

  if (err != ESP_OK) {
    if (done == Job::Connect) {
      setFailed(StrId::STR_BT_CONNECTION_FAILED);
    } else if (!plus_audio_mode_ready()) {
      setFailed(StrId::STR_BT_UNAVAILABLE);
    } else {
      setFailed(StrId::STR_BT_CONNECTION_FAILED);
    }
    LOG_ERR("BTAUD", "job %d failed: %s", static_cast<int>(done), esp_err_to_name(err));
    return;
  }

  switch (done) {
    case Job::Prepare:
    case Job::Rescan:
      if (phase_ == Phase::Preparing) phase_ = Phase::Scanning;
      LOG_INF("BTAUD", "inquiry started (mode %d)", plus_audio_bt_mode());
      break;
    case Job::Connect:
      /* AT+CONNECT answers "CONNECTING" at once; the outcome arrives later as
       * "CONNECT SUCCESS" / "CONNECT TIMEOUT" through the drain, which is where
       * pendingConnectIndex_ becomes connectedIndex_. */
      break;
    case Job::Disconnect:
      phase_ = Phase::Idle;
      connectedIndex_ = -1;
      LOG_INF("BTAUD", "disconnected, module back in mode %d", plus_audio_bt_mode());
      break;
    case Job::None:
      break;
  }
  notifyUpdate();
}

/* ------------------------------------------------------------ module I/O -- */

void PlusBtAudioActivity::drainModule() {
  if (worker_ != nullptr) return; /* never read while a command is in flight */
  const unsigned long now = millis();
  if (lastDrainMs_ != 0 && now - lastDrainMs_ < kDrainIntervalMs) return;
  lastDrainMs_ = now;

  char buffer[kDrainBytes];
  plus_audio_at(nullptr, buffer, sizeof(buffer), 0);
  if (buffer[0] == '\0') return;

  bool changed = false;
  char* save = nullptr;
  for (char* line = strtok_r(buffer, "\n", &save); line != nullptr; line = strtok_r(nullptr, "\n", &save)) {
    trim(line);
    if (line[0] == '\0') continue;
    handleLine(line);
    changed = true;
  }
  if (changed) requestUpdate();
}

void PlusBtAudioActivity::handleLine(const char* line) {
  char address[13];
  char name[48];
  if (parseDeviceLine(line, address, sizeof(address), name, sizeof(name))) {
    addDevice(address, name);
    return;
  }
  if (strstr(line, "INQUIRING START") != nullptr) {
    phase_ = Phase::Scanning;
    deviceCount_ = 0;
    selected_ = kRescanRow;
    LOG_INF("BTAUD", "inquiry started");
    return;
  }
  if (isInquiryComplete(line)) {
    if (phase_ == Phase::Scanning) phase_ = Phase::Idle;
    LOG_INF("BTAUD", "inquiry complete: %d device(s)", deviceCount_);
    return;
  }
  if (isConnectSuccess(line)) {
    phase_ = Phase::Connected;
    connectedIndex_ = (pendingConnectIndex_ >= 0 && pendingConnectIndex_ < deviceCount_) ? pendingConnectIndex_
                                                                                       : connectedIndex_;
    pendingConnectIndex_ = -1;
    LOG_INF("BTAUD", "connect success");
    return;
  }
  if (isConnectTimeout(line)) {
    pendingConnectIndex_ = -1;
    setFailed(StrId::STR_BT_CONNECTION_FAILED);
    LOG_INF("BTAUD", "connect timeout (module gave up)");
    return;
  }
  if (strstr(line, "CONNECTING") != nullptr) {
    phase_ = Phase::Connecting;
    return;
  }
  /* Everything else (RECONNECT, SETUP SCO, DISC SCO, ...) is informative: the
   * vendor page shows the raw line as its status and takes no action. */
  LOG_INF("BTAUD", "module: %s", line);
}

void PlusBtAudioActivity::addDevice(const char* address, const char* name) {
  for (int i = 0; i < deviceCount_; ++i) {
    if (strcmp(devices_[i].address, address) == 0) {
      /* The module re-reports names as they resolve; keep the longer one. */
      if (strlen(name) > strlen(devices_[i].name)) {
        snprintf(devices_[i].name, sizeof(devices_[i].name), "%s", name);
        trim(devices_[i].name);
      }
      return;
    }
  }
  if (deviceCount_ >= kMaxDevices) {
    LOG_INF("BTAUD", "device list full (%d), dropping %s", kMaxDevices, address);
    return;
  }
  Device& slot = devices_[deviceCount_];
  snprintf(slot.address, sizeof(slot.address), "%s", address);
  snprintf(slot.name, sizeof(slot.name), "%s", name);
  trim(slot.name);
  ++deviceCount_;
  LOG_INF("BTAUD", "found %s (%s)", slot.name[0] ? slot.name : "-", slot.address);
  if (selected_ == kRescanRow && deviceCount_ == 1) selected_ = 1;
}

/* --------------------------------------------------------------- actions -- */

void PlusBtAudioActivity::rescan() {
  if (workerBusy()) return;
  plus_audio_bt_scan_stop();
  phase_ = Phase::Preparing;
  failReason_ = StrId::STR_BT_CONNECTION_FAILED;
  requestUpdate();
  startJob(Job::Rescan, nullptr);
}

void PlusBtAudioActivity::disconnect() {
  if (workerBusy()) return;
  phase_ = Phase::Preparing;
  requestUpdate();
  startJob(Job::Disconnect, nullptr);
}

/* requestUpdate() would queue a frame for an activity that is being destroyed. */
void PlusBtAudioActivity::notifyUpdate() {
  if (!exiting_) requestUpdate();
}

void PlusBtAudioActivity::setFailed(const StrId reason) {
  phase_ = Phase::Failed;
  if (reason != StrId::STR_NONE_OPT) failReason_ = reason;
  notifyUpdate();
}

int PlusBtAudioActivity::rowCount() const { return 1 + deviceCount_; }

int PlusBtAudioActivity::rowHeight() const { return GUI.getListRowStep(true); }

int PlusBtAudioActivity::listTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + kGap;
}

int PlusBtAudioActivity::listHeight() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return std::max(0, renderer.getScreenHeight() - listTop() - metrics.buttonHintsHeight - kGap);
}

void PlusBtAudioActivity::clampSelection() {
  const int last = rowCount() - 1;
  if (selected_ < kRescanRow) selected_ = kRescanRow;
  if (selected_ > last) selected_ = last;
}

void PlusBtAudioActivity::moveSelection(const int delta) {
  selected_ += delta;
  clampSelection();
  requestUpdate();
}

void PlusBtAudioActivity::activateRow(const int row) {
  if (row == kRescanRow) {
    rescan();
    return;
  }
  const int index = row - 1;
  if (index < 0 || index >= deviceCount_) return;
  if (workerBusy()) return;
  phase_ = Phase::Connecting;
  pendingConnectIndex_ = index;
  requestUpdate();
  LOG_INF("BTAUD", "connect %s", devices_[index].address);
  startJob(Job::Connect, devices_[index].address);
}

void PlusBtAudioActivity::longPressRow(const int row) {
  /* Disconnect is a property of the page, not of the highlighted row: whichever
   * row is under the finger, the only headset that can be connected is the one
   * the module reported. The vendor page likewise only offers this after CONNECT
   * SUCCESS. */
  if (phase_ != Phase::Connected && connectedIndex_ < 0) {
    LOG_INF("BTAUD", "long press on row %d ignored: not connected", row);
    return;
  }
  disconnect();
}

void PlusBtAudioActivity::debugConfirm() { activateRow(selected_); }
void PlusBtAudioActivity::debugCancel() { finish(); }

/* ------------------------------------------------------------------ loop -- */

void PlusBtAudioActivity::loop() {
  if (worker_ != nullptr && g_worker.done.load()) finishJob();
  if (needsPrepare_ && !workerBusy()) {
    needsPrepare_ = false;
    phase_ = Phase::Preparing;
    requestUpdate();
    startJob(Job::Prepare, nullptr);
  }
  if (worker_ == nullptr) drainModule();

  /* Long-press first: it consumes the Confirm release that would otherwise be a
   * plain "connect". */
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, kLongPressMs)) {
    longPressRow(selected_);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (phase_ == Phase::Connecting || phase_ == Phase::Failed) {
      /* Backing out of a connection attempt rescans instead of leaving the page,
       * so a retry is one keypress away. */
      rescan();
      return;
    }
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) moveSelection(-1);
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) moveSelection(1);
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow(selected_);
    return;
  }

#if FREEINK_CAP_TOUCH
  int row = -1;
  const int step = rowHeight();
  switch (mappedInput.rowTouch(row, listTop(), step, rowCount())) {
    case MappedInputManager::RowTouch::Down:
      if (row >= 0 && row < rowCount()) {
        selected_ = row;
        requestUpdate();
      }
      break;
    case MappedInputManager::RowTouch::Tap:
      if (row >= 0 && row < rowCount()) {
        selected_ = row;
        activateRow(row);
      }
      break;
    case MappedInputManager::RowTouch::None:
      break;
  }

  int lx = 0;
  int ly = 0;
  if (mappedInput.wasScreenLongPress(lx, ly) && step > 0 && ly >= listTop() &&
      ly < listTop() + rowCount() * step) {
    longPressRow((ly - listTop()) / step);
  }
#endif
}

/* ---------------------------------------------------------------- render -- */

const char* PlusBtAudioActivity::statusText(char* buf, const size_t cap) const {
  switch (phase_) {
    case Phase::Preparing:
      return tr(STR_CONNECTING);
    case Phase::Scanning:
      return tr(STR_SCANNING);
    case Phase::Idle:
      return tr(STR_BT_NOT_CONNECTED);
    case Phase::Connecting:
      return tr(STR_CONNECTING);
    case Phase::Connected: {
      const char* name = "?";
      if (connectedIndex_ >= 0 && connectedIndex_ < deviceCount_) {
        const Device& device = devices_[connectedIndex_];
        name = device.name[0] ? device.name : device.address;
      }
      snprintf(buf, cap, tr(STR_BT_CONNECTED_TO), name);
      return buf;
    }
    case Phase::Failed:
      return I18N.get(failReason_);
  }
  return tr(STR_BT_NOT_CONNECTED);
}

std::string PlusBtAudioActivity::rowTitleOf(const int row) const {
  if (row == kRescanRow) return tr(STR_BT_SCAN_PAIR);
  const int index = row - 1;
  if (index < 0 || index >= deviceCount_) return "";
  const Device& device = devices_[index];
  return device.name[0] ? device.name : device.address;
}

std::string PlusBtAudioActivity::rowSubtitleOf(const int row) const {
  /* The name is the row title; the address stays visible so two headsets with
   * the same name remain distinguishable. Which one is connected is the status
   * line's job (it always names the device). */
  if (row == kRescanRow) return "";
  const int index = row - 1;
  if (index < 0 || index >= deviceCount_) return "";
  const Device& device = devices_[index];
  return device.name[0] != '\0' ? device.address : "";
}

void PlusBtAudioActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_BT_AUDIO));

  const int subY = metrics.topPadding + metrics.headerHeight;
  char status[96];
  GUI.drawSubHeader(renderer, Rect{0, subY, width, metrics.tabBarHeight}, statusText(status, sizeof(status)), nullptr);

  const Rect list{0, listTop(), width, listHeight()};
  auto rowTitle = [this](const int i) { return rowTitleOf(i); };
  auto rowSubtitle = [this](const int i) { return rowSubtitleOf(i); };
  GUI.drawList(renderer, list, rowCount(), selected_, rowTitle, rowSubtitle);

  if (deviceCount_ == 0) {
    /* Below the single "Scan & Pair" row. The status line above already says
     * whether the inquiry is running, so both states read the same here. */
    const int step = rowHeight();
    const Rect empty{0, list.y + step, width, std::max(0, list.height - step)};
    if (empty.height > 0) GUI.drawEmptyState(renderer, empty, tr(STR_BT_NO_DEVICES));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
