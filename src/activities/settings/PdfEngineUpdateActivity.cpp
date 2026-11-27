#include "PdfEngineUpdateActivity.h"

#if defined(CROSSPOINT_PDF_XIP)

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#include "I18n.h"
#include "Logging.h"
#include "HalPowerManager.h"
#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "pdf/PdfModuleRuntime.h"

#include <esp_heap_caps.h>

namespace {

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kRedrawMs = 1000;
// The stall windows themselves live in PdfUpdateWatchdog.h, where they are testable.
// The worker checks cancelFlag between route attempts and inside every TLS
// read, so the only uninterruptible wait left is one blocking connect.
constexpr uint32_t kExitWaitMs = 30000;

const char* failureText(const pdf_engine::Failure failure) {
  switch (failure) {
    case pdf_engine::Failure::NoWifi:
      return tr(STR_WIFI_CONN_FAILED);
    case pdf_engine::Failure::Timeout:
      return tr(STR_FONT_EXTRAS_TIMEOUT);
    case pdf_engine::Failure::Network:
      return tr(STR_FONT_EXTRAS_NETWORK);
    case pdf_engine::Failure::Manifest:
      return tr(STR_FONT_EXTRAS_BAD_MANIFEST);
    case pdf_engine::Failure::WrongImage:
      return tr(STR_PDF_ENGINE_MISMATCH);
    case pdf_engine::Failure::Download:
      return tr(STR_DOWNLOAD_FAILED);
    case pdf_engine::Failure::Verify:
      return tr(STR_FONT_EXTRAS_VERIFY_FAILED);
    case pdf_engine::Failure::SdCard:
      return tr(STR_FONT_EXTRAS_STORAGE);
    case pdf_engine::Failure::NotConfigured:
    case pdf_engine::Failure::None:
    case pdf_engine::Failure::Cancelled:
      return nullptr;
  }
  return nullptr;
}

// "1.2 / 1.9 MB" while the body arrives; empty once the size is unknown.
std::string byteSummary(const size_t got, const size_t total) {
  auto fmt = [](const double bytes) {
    char buf[24];
    if (bytes >= 1024.0 * 1024.0) {
      snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024.0) {
      snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    } else {
      snprintf(buf, sizeof(buf), "%.0f B", bytes);
    }
    return std::string(buf);
  };
  if (total == 0) return fmt(static_cast<double>(got));
  return fmt(static_cast<double>(got)) + " / " + fmt(static_cast<double>(total));
}

}  // namespace

void PdfEngineUpdateActivity::onEnter() {
  Activity::onEnter();
  startedAtMs_ = millis();
  lastTickMs_ = startedAtMs_;
  title_ = tr(STR_PDF_ENGINE_UPDATE);
  body_ = nullptr;
  detail_.clear();
  LOG_INF("PDFENG", "engine update requested: manifest=%s", pdf_engine::manifestUrl());
}

void PdfEngineUpdateActivity::connectAndStart() {
  // Reuse the live session when there is one (Settings > Wi-Fi, the browser);
  // otherwise associate with the last saved network, silently -- the user
  // already chose it once, and a second sheet here would just be a wall.
  if (WiFi.status() != WL_CONNECTED) {
    // No pre-flight here, deliberately.  A memory gate can only be judged by the code
    // that owns the radio and only *after* it has reclaimed render memory: an earlier
    // version of this screen demanded a 12 KB contiguous block, unconditionally, and
    // on device refused a download whose radio was already connected (largest block
    // 3,828 B), then refused another that would have succeeded (setMode's own reclaim
    // frees the SD font arenas).  NetworkStartup::setMode() now applies the measured
    // free-heap floor after that reclaim, and this screen just reports why it lost.
    WIFI_STORE.loadFromFile();
    const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
    if (!credential.has_value()) {
      LOG_INF("PDFENG", "no saved network to connect silently");
    } else {
      HalPowerManager::Lock powerLock(/*quiet=*/true);
      // setMode, not WiFi.mode: it stops BLE and reclaims render memory first.
      NetworkStartup::StationClaim stationClaim;
      if (NetworkStartup::setMode(renderer, WIFI_STA)) {
        radioOwnedByUs_ = true;
        NetworkStartup::joinStation(credential->ssid.c_str(), credential->password.c_str());
        const uint32_t deadline = millis() + kConnectTimeoutMs;
        while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(deadline - millis()) > 0) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        LOG_INF("PDFENG", "saved network connect %s", WiFi.status() == WL_CONNECTED ? "ok" : "failed");
      }
      if (WiFi.status() != WL_CONNECTED && radioOwnedByUs_) {
        NetworkStartup::releaseRadio();
        radioOwnedByUs_ = false;
      }
    }
  }
  if (WiFi.status() != WL_CONNECTED) {
    // Name the reason the user can act on: "not enough RAM right now" and "the
    // network refused" call for different things to do.
    const bool lowMemory = !NetworkStartup::canAffiliate(/*verbose=*/false);
    LOG_INF("PDFENG", "no session before the update: %s", lowMemory ? "internal heap too small"
                                                                    : "association did not complete");
    showResult(tr(STR_UPDATE_FAILED), lowMemory ? tr(STR_DICT_LOW_MEMORY) : tr(STR_WIFI_CONN_FAILED), false);
    return;
  }
  if (!pdf_engine::startUpdate()) {
    // Either another run is still in flight or the task could not be created;
    // both are retried from here, so say that rather than showing a bare title.
    showResult(tr(STR_UPDATE_FAILED), tr(STR_TAP_TO_RETRY), false);
    return;
  }
  phase_ = Phase::Running;
  requestUpdate();
}

// `acknowledged` picks the surface: a done/cancel card the user dismisses, or a
// failure card that also invites a retry from Settings.  Both wait for one
// input before popping, so nothing here needs a second button.
void PdfEngineUpdateActivity::showResult(const char* resultTitle, const char* resultBody, const bool acknowledged) {
  title_ = resultTitle;
  body_ = resultBody;
  phase_ = acknowledged ? Phase::Finished : Phase::Failed;
  requestUpdate(true);
}

void PdfEngineUpdateActivity::noteFailure(const pdf_engine::Failure failure, const std::string& why) {
  // The module's own detail string is developer-facing English; keep it in the
  // log only and show the localized reason on screen.
  LOG_ERR("PDFENG", "engine update failed (failure=%u) %s", static_cast<unsigned>(failure), why.c_str());
  if (failure == pdf_engine::Failure::Cancelled) {
    showResult(tr(STR_CANCEL), nullptr, true);
    return;
  }
  showResult(tr(STR_UPDATE_FAILED), failureText(failure), false);
}

void PdfEngineUpdateActivity::installNow() {
  phase_ = Phase::Installing;
  // Present the card and wait for it, then keep the render task off the panel
  // for the erase + write: a 37-phase scan interrupted by the flash operation
  // would underrun and fail (the frame then has to be rebuilt).
  requestUpdateAndWait();
  RenderLock renderLock(*this);
  // This task has an internal-RAM stack, so the erase + MMU unmap are safe here.
  switch (pdf_engine::installFromTemp()) {
    case pdf_module::InstallStatus::Ok:
      LOG_INF("PDFENG", "engine installed from the network image");
      if (returnOnSuccess_) {
        // Started from the reader: pop immediately and let it reopen the book
        // at the same position.  A "done" card here would only add a tap.
        finish();
      } else {
        showResult(tr(STR_DONE), nullptr, true);
      }
      break;
    case pdf_module::InstallStatus::Mismatch:
      showResult(tr(STR_UPDATE_FAILED), tr(STR_PDF_ENGINE_MISMATCH), false);
      break;
    case pdf_module::InstallStatus::NotFound:
      showResult(tr(STR_UPDATE_FAILED), tr(STR_PDF_ENGINE_NOT_FOUND), false);
      break;
    case pdf_module::InstallStatus::BadHeader:
      showResult(tr(STR_UPDATE_FAILED), tr(STR_PDF_ENGINE_MISMATCH), false);
      break;
    case pdf_module::InstallStatus::IoError:
      showResult(tr(STR_UPDATE_FAILED), tr(STR_FONT_EXTRAS_STORAGE), false);
      break;
    case pdf_module::InstallStatus::Busy:
      // A PDF document still holds the engine.  Nothing was erased; closing
      // the reader and retrying is the whole remedy.
      showResult(tr(STR_UPDATE_FAILED), tr(STR_TAP_TO_RETRY), false);
      break;
  }
}

void PdfEngineUpdateActivity::loop() {
  mappedInput.update();
  const uint32_t now = millis();

  if (phase_ == Phase::Connecting) {
    // Paint the card before the blocking association so the screen is not
    // frozen on an empty page for up to 15 seconds.
    requestUpdateAndWait();
    connectAndStart();
    return;
  }

  if (phase_ == Phase::Finished || phase_ == Phase::Failed) {
    // One acknowledgement, then straight back to Settings.
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasScreenTapped(tapX, tapY) || mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (phase_ == Phase::Running) {
    int tapX = 0;
    int tapY = 0;
    const bool tappedCancel = cardLayout_.cancel.width > 0 && mappedInput.wasScreenTapped(tapX, tapY) &&
                              tapX >= cardLayout_.cancel.x && tapX < cardLayout_.cancel.x + cardLayout_.cancel.width &&
                              tapY >= cardLayout_.cancel.y && tapY < cardLayout_.cancel.y + cardLayout_.cancel.height;
    if (!cancelRequested_ && (tappedCancel || mappedInput.wasReleased(MappedInputManager::Button::Back))) {
      cancelRequested_ = true;
      pdf_engine::cancelUpdate();
      requestUpdate(true);
    }
    if (static_cast<int32_t>(now - lastTickMs_) < 0 || now - lastTickMs_ < kRedrawMs) return;
    lastTickMs_ = now;
    const pdf_engine::State state = pdf_engine::pollUpdate();
    if (state == pdf_engine::State::Running) {
      const pdf_engine::Progress p = pdf_engine::progress();
      const auto stage = static_cast<pdf_update::StallStage>(p.stage);
      lastProgressMs_ = pdf_update::nextBaseline(stage, lastStage_, now, lastProgressMs_,
                                                 static_cast<uint32_t>(p.downloaded),
                                                 static_cast<uint32_t>(lastProgressBytes_));
      lastStage_ = stage;
      lastProgressBytes_ = p.downloaded;
      const auto stall = pdf_update::checkStall(stage, static_cast<uint32_t>(p.downloaded), now,
                                                lastProgressMs_);
      if (stall.fired) {
        LOG_ERR("PDFENG", "download stalled at %u / %u bytes: %u ms since the last byte (limit %u ms); cancelling",
                static_cast<unsigned>(p.downloaded), static_cast<unsigned>(p.total),
                static_cast<unsigned>(stall.elapsedMs), static_cast<unsigned>(stall.limitMs));
        cancelRequested_ = true;
        pdf_engine::cancelUpdate();
        requestUpdate(true);
        return;
      }
    }
    if (state == pdf_engine::State::Running) {
      requestUpdate();  // advance the progress figure
      return;
    }
    if (state == pdf_engine::State::Done) {
      installNow();
      return;
    }
    // Failed, or Idle (the worker never got scheduled): never install.
    noteFailure(state == pdf_engine::State::Failed ? pdf_engine::result().failure : pdf_engine::Failure::Network,
                pdf_engine::result().detail);
    return;
  }
}

void PdfEngineUpdateActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const uint32_t elapsed = millis() - startedAtMs_;
  int permille = -1;
  const char* detail = nullptr;
  const char* detail2 = nullptr;
  std::string bytes;

  if (phase_ == Phase::Connecting) {
    detail = tr(STR_CONNECTING_SAVED_WIFI);
  } else if (phase_ == Phase::Running) {
    const pdf_engine::Progress p = pdf_engine::progress();
    switch (p.stage) {
      case pdf_engine::Stage::Manifest:
        detail = tr(STR_CHECK_UPDATES);
        permille = 150;
        break;
      case pdf_engine::Stage::Body: {
        detail = tr(STR_DOWNLOADING);
        bytes = byteSummary(p.downloaded, p.total);
        detail2 = bytes.c_str();
        const int share = p.total > 0 ? static_cast<int>(700ull * p.downloaded / p.total) : 0;
        permille = 300 + (share > 580 ? 580 : share);
        break;
      }
      case pdf_engine::Stage::Verify:
        bytes = byteSummary(p.total, p.total);
        detail2 = bytes.c_str();
        permille = 900;
        break;
      case pdf_engine::Stage::Idle:
        permille = -1;  // started but no stage published yet: swim the duck
        break;
    }
  } else if (phase_ == Phase::Installing) {
    permille = 950;
  }

  if (phase_ == Phase::Finished || phase_ == Phase::Failed) {
    status_card::paintMessage(renderer, title_, body_);
    cardLayout_ = {};
    return;
  }
  cardLayout_ = status_card::paintWait(renderer, title_, detail, detail2, permille,
                                       /*withCancel=*/phase_ == Phase::Running, elapsed);
}

void PdfEngineUpdateActivity::onExit() {
  Activity::onExit();
  if (pdf_engine::updateInProgress()) {
    // Back was pressed while a request was in flight.  Never delete the worker
    // mid-heap (that is the corruption JjwxcCommentActivity used to hit), so
    // cancel and give it a bounded window to stop itself.
    pdf_engine::cancelUpdate();
    const uint32_t deadline = millis() + kExitWaitMs;
    while (pdf_engine::updateInProgress() && static_cast<int32_t>(deadline - millis()) > 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    pdf_engine::pollUpdate();
    if (pdf_engine::updateInProgress()) {
      LOG_ERR("PDFENG", "engine worker did not stop in %lus; a retry needs a reboot",
              static_cast<unsigned long>(kExitWaitMs / 1000));
    }
  }
  if (radioOwnedByUs_) {
    NetworkStartup::releaseRadio();
    radioOwnedByUs_ = false;
  }
}

#endif
