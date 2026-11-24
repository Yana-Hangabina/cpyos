#include "plus_ota_activity.h"

#include <Arduino.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <WiFi.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BackgroundWork.h"
#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "components/StatusCards.h"
#include "components/UITheme.h"

namespace {
constexpr const char* kTempPath = "/.crosspoint/ota/update.bin";

void otaProgress(size_t done, size_t total, void* ctx) {
  if (auto* self = static_cast<PlusOtaActivity*>(ctx)) self->publishProgress(done, total);
}

bool hit(const Rect& r, int x, int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}
}  // namespace

void PlusOtaActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("OTA", "page: local=%s id=%s channel=%s configured=%d", plus_ota::currentVersion(), plus_ota::deviceId(),
          "partner", plus_ota::configured() ? 1 : 0);
  if (!plus_ota::configured()) setError("此构建未配置 CloudZao 产品标识");
  requestUpdate();
}

void PlusOtaActivity::onExit() {
  cancelRequested_ = true;
  const unsigned long deadline = millis() + 50000;
  while (worker_ != nullptr && millis() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (worker_ != nullptr) {
    /* The worker may still be inside a socket or a flash write, and it holds
     * `this`: freeing the activity here is a use-after-free, deleting the task
     * corrupts the socket/flash, and releasing the radio deinitializes lwIP
     * under its socket. A reset is the only safe recovery; the boot slot has
     * not been switched, so an interrupted inactive-slot write is harmless. */
    LOG_ERR("OTA", "worker did not stop; restarting instead of freeing its owner");
    esp_restart();
  }
  NetworkStartup::releaseRadio();
  Activity::onExit();
}

void PlusOtaActivity::setError(const std::string& text) {
  error_ = text;
  state_ = State::Failed;
  LOG_ERR("OTA", "error: %s", text.c_str());
  requestUpdate();
}

void PlusOtaActivity::debugCheck() {
  channel_ = plus_ota::Channel::Test;
  startJob(Job::Check);
}
void PlusOtaActivity::debugSetChannel(const int channel) {
  channel_ = channel == 0 ? plus_ota::Channel::Stable : plus_ota::Channel::Test;
  requestUpdate();
}

void PlusOtaActivity::debugInstallUrl(const char* url) {
  startInstallUrl(url);
}

void PlusOtaActivity::startInstallUrl(const char* url) {
  plus_ota::Info info;
  info.ok = true;
  info.hasUpdate = true;
  info.url = url ? url : "";
  info.version = "direct";
  info.size = 0;
  startInstall(info);
}

void PlusOtaActivity::startInstall(const plus_ota::Info& info) {
  if (busy()) return;
  info_ = info;
  state_ = State::Downloading;
  progressBytes_.store(0);
  progressTotal_.store(info.size);
  startJob(Job::Install);
}

// Silent connect to the last saved AP on the UI task (the worker must not touch
// the renderer). OTA needs the radio for the check and the download.
bool PlusOtaActivity::ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  HalPowerManager::Lock powerLock(/*quiet=*/true);
  WIFI_STORE.loadFromFile();
  const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential.has_value()) {
    LOG_INF("OTA", "no saved network to connect silently");
    return false;
  }
  LOG_INF("OTA", "connecting saved network %.32s", credential->ssid.c_str());
  NetworkStartup::prepare(renderer);
  if (!NetworkStartup::setMode(renderer, WIFI_STA)) return false;
  WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(deadline - millis()) > 0) {
    delay(100);
  }
  const bool connected = WiFi.status() == WL_CONNECTED;
  LOG_INF("OTA", "saved network connect %s", connected ? "ok" : "failed");
  if (!connected) NetworkStartup::releaseRadio();
  return connected;
}

void PlusOtaActivity::startJob(const Job job) {
  if (worker_ != nullptr) return;
  if (!plus_ota::configured() && job == Job::Check) {
    setError("此构建未配置开发者资源密钥（plus_resource_key.local）");
    return;
  }
  if (!ensureWifi()) {
    setError("需要 Wi-Fi 连接（请先在设置里加入网络）");
    return;
  }
  job_ = job;
  cancelRequested_ = false;
  workerDone_.store(false);
  stage_.store(job == Job::Check ? 0 : 1);
  if (job == Job::Check) state_ = State::Checking;
  // Internal stack on purpose: the install step erases/writes internal flash,
  // and a PSRAM stack trips esp_task_stack_is_sane_cache_disabled() during the
  // cache-off window (measured: assert right after "downloaded …"). TLS already
  // allocates from PSRAM via the wolfSSL allocator, so 16 KiB internal is enough.
  constexpr uint32_t kWorkerStackBytes = 16 * 1024;
  workerStackInPsram_ = false;
  const BaseType_t ok = xTaskCreatePinnedToCore(workerTrampoline, "plusOta", kWorkerStackBytes,
                                                this, tskIDLE_PRIORITY, &worker_, 0);
  if (ok != pdPASS) {
    worker_ = nullptr;
    job_ = Job::None;
    setError("cannot start the OTA task");
    return;
  }
  LOG_INF("OTA", "worker up (internal stack; flash writes need it)");
  requestUpdate();
}

void PlusOtaActivity::workerTrampoline(void* param) {
  auto* self = static_cast<PlusOtaActivity*>(param);
  {
    background_work::Guard guard;  // no light/deep sleep while writing flash
    self->runWorker();
  }
  self->workerDone_.store(true);
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));  // reaped by the UI task
}

void PlusOtaActivity::runWorker() {
  if (job_ == Job::Check) {
    info_ = plus_ota::check(channel_);
    stage_.store(0);
    return;
  }
  // Install: download first (informational), then flash.
  stage_.store(1);
  std::string error;
  const bool down = plus_ota::download(info_, kTempPath, otaProgress, this, &cancelRequested_, error);
  if (!down) {
    info_.ok = false;
    info_.error = error;
    return;
  }
  stage_.store(2);
  if (!plus_ota::install(kTempPath, error)) {
    info_.ok = false;
    info_.error = error;
    return;
  }
  info_.ok = true;
}

void PlusOtaActivity::pollWorker() {
  if (worker_ == nullptr || !workerDone_.load()) return;
  const bool wasInstall = job_ == Job::Install;
  reapWorker();
  job_ = Job::None;
  if (state_ == State::Failed) return;
  if (!wasInstall) {
    if (!info_.ok) {
      setError(info_.error.empty() ? "cannot check for updates" : info_.error);
    } else if (!info_.hasUpdate) {
      state_ = State::Idle;
      error_ = "已是最新版本";
    } else {
      state_ = State::Available;
    }
  } else {
    if (!info_.ok) {
      setError(info_.error.empty() ? "update failed" : info_.error);
    } else {
      state_ = State::Done;
      rebootPending_ = true;
      rebootAt_ = millis() + 1500;
    }
  }
  requestUpdate();
}

void PlusOtaActivity::reapWorker() {
  if (worker_ == nullptr) return;
  if (workerStackInPsram_) {
    vTaskDeleteWithCaps(worker_);
  } else {
    vTaskDelete(worker_);
  }
  worker_ = nullptr;
  workerStackInPsram_ = false;
}

void PlusOtaActivity::onPrimary() {
  switch (state_) {
    case State::Available:
      startInstall(info_);
      break;
    case State::Failed:
    case State::Idle:
      startJob(Job::Check);
      break;
    case State::Done:
      esp_restart();
      break;
    default:
      break;
  }
}

void PlusOtaActivity::onSecondary() {
  if (busy()) {
    cancelRequested_ = true;
    return;
  }
  finish();
}

void PlusOtaActivity::debugConfirm() { onPrimary(); }
void PlusOtaActivity::debugCancel() { onSecondary(); }

void PlusOtaActivity::loop() {
  pollWorker();
  if (rebootPending_ && millis() >= rebootAt_) {
    LOG_INF("OTA", "rebooting into the new slot");
    esp_restart();
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (hit(primaryRect_, x, y)) {
      onPrimary();
      return;
    }
    if (hit(secondaryRect_, x, y)) {
      onSecondary();
      return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (busy()) {
      cancelRequested_ = true;
      return;
    }
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onPrimary();
  }
}

void PlusOtaActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, "系统更新");

  int y = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 16;
  const int x = metrics.contentSidePadding;
  const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);

  char buf[160];
  std::snprintf(buf, sizeof(buf), "当前版本  %s", plus_ota::currentVersion());
  renderer.drawText(UI_12_FONT_ID, x, y, buf, true, EpdFontFamily::BOLD);
  y += line12 + 8;
  std::snprintf(buf, sizeof(buf), "板型  %s · %s · %s", plus_ota::boardName(), plus_ota::chipName(),
                plus_ota::appName());
  renderer.drawText(UI_10_FONT_ID, x, y, buf);
  y += line10 + 4;
  std::snprintf(buf, sizeof(buf), "设备  %s", plus_ota::deviceId());
  renderer.drawText(UI_10_FONT_ID, x, y, buf);
  y += line10 + 16;

  switch (state_) {
    case State::Checking:
      renderer.drawText(UI_12_FONT_ID, x, y, "正在检查更新…");
      break;
    case State::Available:
      std::snprintf(buf, sizeof(buf), "发现新版本  %s  (%u KB)", info_.version.c_str(),
                    static_cast<unsigned>(info_.size / 1024));
      renderer.drawText(UI_12_FONT_ID, x, y, buf, true, EpdFontFamily::BOLD);
      y += line12 + 6;
      if (!info_.versionStatus.empty()) {
        std::snprintf(buf, sizeof(buf), "版本状态  %s", info_.versionStatus.c_str());
        renderer.drawText(UI_10_FONT_ID, x, y, buf);
      }
      break;
    case State::Downloading: {
      const size_t done = progressBytes_.load();
      const size_t total = progressTotal_.load();
      std::snprintf(buf, sizeof(buf), "正在下载  %u / %u KB", static_cast<unsigned>(done / 1024),
                    static_cast<unsigned>(total / 1024));
      renderer.drawText(UI_12_FONT_ID, x, y, buf);
      y += line12 + 8;
      renderer.drawRect(x, y, w - 2 * x, 14, 1, true);
      if (total > 0) {
        const int fill = static_cast<int>(static_cast<uint64_t>(w - 2 * x) * done / total);
        renderer.fillRect(x + 1, y + 1, std::max(0, fill - 2), 12, true);
      }
      break;
    }
    case State::Installing:
      renderer.drawText(UI_12_FONT_ID, x, y, "正在写入新版本…");
      break;
    case State::Done:
      renderer.drawText(UI_12_FONT_ID, x, y, "更新已写入，正在重启…", true, EpdFontFamily::BOLD);
      break;
    case State::Failed:
      renderer.drawText(UI_12_FONT_ID, x, y, "更新失败", true, EpdFontFamily::BOLD);
      y += line12 + 6;
      renderer.drawText(UI_10_FONT_ID, x, y, error_.c_str());
      break;
    case State::Idle:
      renderer.drawText(UI_12_FONT_ID, x, y, error_.empty() ? "点击下方按钮检查更新" : error_.c_str());
      break;
  }

  // Two capsules at the bottom: primary (check / install / retry) and channel.
  const int btnH = 56;
  const int gap = 16;
  const int btnW = (w - 2 * x - gap) / 2;
  const int btnY = h - metrics.buttonHintsHeight - btnH - 16;
  const char* primary = state_ == State::Available ? "安装更新"
                       : state_ == State::Done    ? "立即重启"
                                                  : "检查更新";
  if (state_ == State::Checking || state_ == State::Downloading || state_ == State::Installing) primary = "请稍候";
  primaryRect_ = Rect{x, btnY, btnW, btnH};
  secondaryRect_ = Rect{x + btnW + gap, btnY, btnW, btnH};
  renderer.fillRoundedRect(primaryRect_.x, primaryRect_.y, primaryRect_.width, primaryRect_.height, btnH / 2,
                           Color::Black);
  renderer.drawCenteredText(UI_12_FONT_ID, primaryRect_.y + (btnH - line12) / 2, primary, false,
                            EpdFontFamily::BOLD);
  renderer.drawRoundedRect(secondaryRect_.x, secondaryRect_.y, secondaryRect_.width, secondaryRect_.height, 2,
                           btnH / 2, true);
  const char* secondary = busy() ? "取消" : "返回";
  renderer.drawCenteredText(UI_10_FONT_ID, secondaryRect_.y + (btnH - line10) / 2, secondary, true);
  // Like every activity, this one owns its push; without it the page only
  // appeared when some later frame happened to flush the framebuffer.
  renderer.displayBuffer();
}
