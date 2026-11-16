#include "plus_voice_activity.h"

#include <Arduino.h>
#include <CrossPointSettings.h>
#include <EpdFontFamily.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <plus_audio.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "I18n.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "NetworkStartup.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* kTag = "VOICEUI";
constexpr int kGap = 8;
constexpr int kTalkHeight = 120;
constexpr int kUrlMax = sizeof(plus_voice::Config::url) - 1;
constexpr int kTokenMax = sizeof(plus_voice::Config::token) - 1;

/* The token is a bearer secret: show only that one is stored. */
constexpr const char* kMasked = "********";

}  // namespace

/* ------------------------------------------------------------------ life -- */

void PlusVoiceActivity::onEnter() {
  Activity::onEnter();
  exiting_ = false;
  selected_ = kRowUrl;
  notice_ = StrId::STR_NONE_OPT;
  talkHeld_ = false;
  confirmTalk_ = false;
  wifiOwned_ = false;
  pendingAutoTalk_ = false;
  autoConnectPending_ = false;
  touchTalk_ = false;
  output_[0] = '\0';

  plus_voice::loadConfig(config_);
  /* A page reopened after an abnormal exit may still own the previous worker. */
  plus_voice::stop();
  lastPhase_ = plus_voice::Phase::Idle;
  lastPtt_ = false;
  if (autoTalk_ && config_.valid()) {
    // The physical AI key's long press asked for a turn. The channel is not up
    // yet; connect on the first loop iteration and start listening once it is.
    pendingAutoTalk_ = true;
    autoConnectPending_ = true;
  }
  LOG_INF(kTag, "enter: url=%d token=%d version=%u auto=%d", config_.url[0] != '\0' ? 1 : 0,
          config_.token[0] != '\0' ? 1 : 0, static_cast<unsigned>(config_.version),
          autoTalk_ ? 1 : 0);
  requestUpdate();
}

void PlusVoiceActivity::onExit() {
  exiting_ = true;
  const bool stopped = plus_voice::stop();
  plus_audio_pa(false);
  /* Only release a radio this page brought up, and only once the worker has
   * actually released the socket; a timed-out worker may still be inside TLS. */
  if (wifiOwned_) {
    if (stopped) {
      NetworkStartup::releaseRadio();
    } else {
      LOG_ERR(kTag, "worker still exiting; leaving the radio up");
    }
    wifiOwned_ = false;
  }
  /* beginConnect() hands render memory back before the TLS session, and the
   * popped-to reader's onEnter() does not run again. Reload the face here so a
   * key-launched session does not return to a book drawn in the 8 pt fallback. */
  if (fromKey_ && activityManager.isReaderActivity()) {
    LOG_INF(kTag, "restoring the reader face after the voice session");
    sdFontSystem.ensureLoaded(renderer);
  }
  LOG_INF(kTag, "exit");
  Activity::onExit();
}

bool PlusVoiceActivity::preventAutoSleep() {
  /* Only an active turn (or the connect that a physical-key long press asked
   * for) needs the panel awake; an idle open channel may sleep like any other
   * page (the session is torn down on exit either way). */
  return talkHeld_ || pendingAutoTalk_ || plus_voice::status().ptt;
}

void PlusVoiceActivity::prepareForSleep() {
  // Standby does not call onExit; leaving a WebSocket/TLS session open across
  // light sleep would cut the rails under a live socket. Stop it here, on the
  // UI task, before the park sequence.
  touchTalk_ = false;
  pendingAutoTalk_ = false;
  autoConnectPending_ = false;
  setTalk(false);
  plus_audio_pa(false);
  const bool stopped = plus_voice::stop();
  if (wifiOwned_) {
    // Same rule as onExit: never tear the radio down under a worker that is
    // still inside TLS. A failed stop leaves the page flagged so Standby's
    // sleep gate refuses the park instead of cutting a live socket.
    if (stopped) {
      NetworkStartup::releaseRadio();
    } else {
      LOG_ERR(kTag, "worker still exiting at sleep; leaving the radio up");
    }
    wifiOwned_ = false;
  }
}

bool PlusVoiceActivity::onAiKey(const bool /*longPress*/) {
  if (!config_.valid()) {
    // Do not latch a pending turn against an unconfigured page: it would keep
    // preventAutoSleep() true and auto-start later when a URL is entered.
    pendingAutoTalk_ = false;
    notice_ = StrId::STR_NO_SERVER_URL;
    requestUpdate();
    return true;
  }
  const plus_voice::Phase phase = plus_voice::status().phase;
  if (phase == plus_voice::Phase::Connected) {
    // One press, one state change: the key is the hands-free talk toggle while
    // this page is in front.
    if (talkHeld_) touchTalk_ = false;
    setTalk(!talkHeld_);
    return true;
  }
  if (phase == plus_voice::Phase::Connecting) {
    // Cancel the queued turn, but keep the channel coming up: the user may
    // still want to talk with the on-screen capsule.
    if (pendingAutoTalk_) {
      pendingAutoTalk_ = false;
      notice_ = StrId::STR_NONE_OPT;
      requestUpdate();
    }
    return true;
  }
  // Idle/Failed: start a session that listens as soon as it is ready.
  pendingAutoTalk_ = true;
  if (plus_voice::running()) plus_voice::stop();
  connectOrDisconnect();
  return true;
}

void PlusVoiceActivity::debugConfirm() { activateRow(selected_); }
void PlusVoiceActivity::debugCancel() {
  if (fromKey_) {
    activityManager.popActivity();
  } else {
    activityManager.goToApps();
  }
}

/* ----------------------------------------------------------------- input -- */

int PlusVoiceActivity::rowHeight() const { return GUI.getListRowStep(true); }

Rect PlusVoiceActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + kGap;
  return Rect{0, top, renderer.getScreenWidth(), rowHeight() * kRowCount};
}

Rect PlusVoiceActivity::talkRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - kGap;
  return Rect{40, bottom - kTalkHeight, width - 80, kTalkHeight};
}

Rect PlusVoiceActivity::textRect() const {
  const Rect list = listRect();
  const Rect talk = talkRect();
  const int top = list.y + list.height + kGap;
  const int height = std::max(0, talk.y - kGap - top);
  return Rect{40, top, renderer.getScreenWidth() - 80, height};
}

void PlusVoiceActivity::moveSelection(const int delta) {
  const int next = std::clamp(selected_ + delta, 0, kRowCount - 1);
  if (next == selected_) return;
  selected_ = next;
  requestUpdate();
}

void PlusVoiceActivity::activateRow(const int row) {
  switch (row) {
    case kRowUrl:
      editUrl();
      break;
    case kRowToken:
      editToken();
      break;
    case kRowConnect:
      connectOrDisconnect();
      break;
    case kRowTalk:
      /* Handled by the hold logic in loop(); a plain Confirm press here has no
       * meaning of its own. */
      break;
    default:
      break;
  }
}

void PlusVoiceActivity::editUrl() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto& kb = std::get<KeyboardResult>(result.data);
    {
      /* The render task reads config_ through rowSubtitle(); replace the string
       * under the render lock so a frame cannot see it half written. */
      RenderLock lock(*this);
      /* An "https://" / "http://" leftover from the URL keyboard's prefill is not
       * a server: treat it as cleared. */
      if (kb.text == "ws://" || kb.text == "wss://" || kb.text == "http://" || kb.text == "https://") {
        config_.url[0] = '\0';
      } else {
        std::strncpy(config_.url, kb.text.c_str(), kUrlMax);
        config_.url[kUrlMax] = '\0';
      }
      notice_ = StrId::STR_NONE_OPT;
    }
    plus_voice::saveConfig(config_);
    requestUpdate();
  };
  std::string prefill = config_.url;
  if (prefill.empty()) prefill = "wss://";
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_SYNC_SERVER_URL), prefill, kUrlMax,
                                                    InputType::Url);
}

void PlusVoiceActivity::editToken() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto& kb = std::get<KeyboardResult>(result.data);
    {
      RenderLock lock(*this);
      std::strncpy(config_.token, kb.text.c_str(), kTokenMax);
      config_.token[kTokenMax] = '\0';
      notice_ = StrId::STR_NONE_OPT;
    }
    plus_voice::saveConfig(config_);
    requestUpdate();
  };
  /* Password input: masked on screen and never echoed to the log. */
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_PASSWORD), config_.token, kTokenMax,
                                                    InputType::Password);
}

/* --------------------------------------------------------------- session -- */
void PlusVoiceActivity::connectOrDisconnect() {
  if (plus_voice::running()) {
    endSession();
    return;
  }
  if (!config_.valid()) {
    notice_ = StrId::STR_NO_SERVER_URL;
    requestUpdate();
    return;
  }
  notice_ = StrId::STR_NONE_OPT;

  if (WiFi.status() != WL_CONNECTED) {
    // The shared sheet auto-connects the last saved network and gives the user
    // progress/cancel; never block the UI task on a connect.
    wifiOwned_ = true;
    if (!startActivityForResultWith<WifiSelectionActivity>(
            [this](const ActivityResult& result) { onWifiResult(!result.isCancelled); })) {
      notice_ = StrId::STR_CONNECTION_FAILED;
      requestUpdate();
    }
    return;
  }
  beginConnect();
}

void PlusVoiceActivity::onWifiResult(const bool connected) {
  if (!connected) {
    notice_ = StrId::STR_CONNECTION_FAILED;
    requestUpdate();
    return;
  }
  beginConnect();
}

/* The Wi-Fi sheet auto-connects the last saved network and keeps the connect on
 * its own task; this page never blocks the UI on a join. */

void PlusVoiceActivity::beginConnect() {
  notice_ = StrId::STR_NONE_OPT;
  /* Hand render memory back before the TLS session and libopus allocate. */
  NetworkStartup::prepare(renderer);
  if (!plus_voice::start(config_)) {
    // start() only fails on a task/heap allocation here (the caller checks
    // running() first); say so instead of blaming the server.
    pendingAutoTalk_ = false;
    notice_ = StrId::STR_BT_LOW_MEMORY;
    LOG_ERR(kTag, "worker start failed (memory)");
  }
  lastPhase_ = plus_voice::status().phase;
  requestUpdate();
}

void PlusVoiceActivity::endSession() {
  touchTalk_ = false;
  setTalk(false);
  plus_voice::stop();
  plus_audio_pa(false);
  lastPhase_ = plus_voice::status().phase;
  requestUpdate();
}

void PlusVoiceActivity::setTalk(const bool held) {
  /* PTT is only meaningful on an open channel; keeping the local flag honest
   * stops the capsule from inverting while the session is still connecting. */
  if (held && plus_voice::status().phase != plus_voice::Phase::Connected) return;
  if (held == talkHeld_) return;
  talkHeld_ = held;
  plus_voice::setPtt(held);
  requestUpdate();
}

void PlusVoiceActivity::pollSession() {
  const plus_voice::Status st = plus_voice::status();
  char text[sizeof(output_)];
  plus_voice::copyLastText(text, sizeof(text));
  const bool textChanged = std::strcmp(text, output_) != 0;
  if (textChanged) std::snprintf(output_, sizeof(output_), "%s", text);

  if (st.phase == lastPhase_ && st.ptt == lastPtt_ && !textChanged) return;
  if (st.phase != lastPhase_) {
    LOG_INF(kTag, "phase %d -> %d (err %d)", static_cast<int>(lastPhase_), static_cast<int>(st.phase), st.error);
  }
  lastPhase_ = st.phase;
  lastPtt_ = st.ptt;
  if (st.phase == plus_voice::Phase::Connected && pendingAutoTalk_) {
    pendingAutoTalk_ = false;
    setTalk(true);
  } else if (st.phase == plus_voice::Phase::Failed) {
    pendingAutoTalk_ = false;
  }
  if (!exiting_) requestUpdate();
}

/* ------------------------------------------------------------------ loop -- */

void PlusVoiceActivity::loop() {
  if (autoConnectPending_) {
    autoConnectPending_ = false;
    if (config_.valid()) {
      connectOrDisconnect();
    } else {
      pendingAutoTalk_ = false;
    }
  }
  pollSession();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (fromKey_) {
      activityManager.popActivity();
    } else {
      activityManager.goToApps();
    }
    return;
  }

  /* Talk row: hold Confirm to stream, release to stop. The release must not
   * also activate the row (one gesture, one action). */
  if (selected_ == kRowTalk) {
    if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      if (!confirmTalk_) {
        confirmTalk_ = true;
        setTalk(true);
      }
    } else if (confirmTalk_) {
      confirmTalk_ = false;
      touchTalk_ = false;
      setTalk(false);
      return;
    }
  }

#if FREEINK_CAP_TOUCH
  /* The capsule is the primary (touch) hold target. Level state, not a tap:
   * the release edge, a drag off the capsule and Back all stop the turn. */
  int tx = 0;
  int ty = 0;
  const bool contact = mappedInput.isScreenTouchHeld(tx, ty);
  const Rect talk = talkRect();
  const bool onCapsule = contact && tx >= talk.x && tx < talk.x + talk.width && ty >= talk.y &&
                         ty < talk.y + talk.height;
  // Touch owns only the turn it started. The old "no contact => stop" rule
  // also fired for a turn started by the AI key or Confirm, stopping it one
  // frame after it began.
  if (onCapsule && !touchTalk_ && !talkHeld_) {
    touchTalk_ = true;
    setTalk(true);
  } else if (!onCapsule && touchTalk_) {
    touchTalk_ = false;
    setTalk(false);
  }
#else
  (void)0;
#endif

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) moveSelection(-1);
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) moveSelection(1);

  /* Confirm activates the highlighted row, except on the Talk row where the
   * press/release pair above already owned the gesture. */
  if (selected_ != kRowTalk && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow(selected_);
    return;
  }

  const Rect list = listRect();
  if (handleListTouch(selected_, kRowCount, list.y, list.height, true) == ListTouchResult::Activated) {
    activateRow(selected_);
    return;
  }
}

/* ---------------------------------------------------------------- render -- */

const char* PlusVoiceActivity::statusText() const {
  const plus_voice::Status st = plus_voice::status();
  if (st.ptt || talkHeld_) return tr(STR_LISTENING);
  switch (st.phase) {
    case plus_voice::Phase::Connecting:
      return tr(STR_CONNECTING);
    case plus_voice::Phase::Connected:
      return tr(STR_CONNECTED);
    case plus_voice::Phase::Failed:
      return st.error == plus_voice::kErrorNoNetwork ? tr(STR_NO_NETWORKS) : tr(STR_CONNECTION_FAILED);
    case plus_voice::Phase::Idle:
    default:
      if (notice_ != StrId::STR_NONE_OPT) return I18N.get(notice_);
      if (!config_.valid()) return tr(STR_NO_SERVER_URL);
      return "";
  }
}

std::string PlusVoiceActivity::rowTitle(const int row) const {
  switch (row) {
    case kRowUrl:
      /* Reused label: the EN translation blob has no headroom for a dedicated
       * "server URL" key, and this is the closest existing wording. */
      return tr(STR_SYNC_SERVER_URL);
    case kRowToken:
      return tr(STR_PASSWORD);
    case kRowConnect:
      return tr(STR_CONNECT);
    case kRowTalk:
      return tr(STR_PTT);
    default:
      return "";
  }
}

std::string PlusVoiceActivity::rowSubtitle(const int row) const {
  switch (row) {
    case kRowUrl:
      return config_.url[0] != '\0' ? config_.url : tr(STR_NO_SERVER_URL);
    case kRowToken:
      return config_.token[0] != '\0' ? kMasked : "";
    case kRowConnect:
      /* The right-hand value already says Connect/Disconnect; no second line. */
      return "";
    case kRowTalk: {
      const plus_voice::Status st = plus_voice::status();
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%u / %u", static_cast<unsigned>(st.sentFrames),
                    static_cast<unsigned>(st.recvFrames));
      return plus_voice::running() ? buf : "";
    }
    default:
      return "";
  }
}

std::string PlusVoiceActivity::rowValue(const int row) const {
  if (row == kRowConnect) return plus_voice::running() ? tr(STR_BT_DISCONNECT) : tr(STR_CONNECT);
  return "";
}

void PlusVoiceActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_VOICE_CLIENT));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, width, metrics.tabBarHeight},
                    statusText(), nullptr);

  const Rect list = listRect();
  auto title = [this](const int i) { return rowTitle(i); };
  auto subtitle = [this](const int i) { return rowSubtitle(i); };
  auto value = [this](const int i) { return rowValue(i); };
  GUI.drawList(renderer, list, kRowCount, selected_, title, subtitle, nullptr, value, true);

  /* Everything below the list: the failure notice (never a reboot page) and the
   * last transcript/sentence from the server. */
  const Rect text = textRect();
  if (text.height > 0) {
    int y = text.y;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    if (notice_ != StrId::STR_NONE_OPT) {
      const std::string line = renderer.truncatedText(UI_10_FONT_ID, I18N.get(notice_), text.width);
      renderer.drawText(UI_10_FONT_ID, text.x, y, line.c_str());
      y += lineHeight + 4;
    }
    if (output_[0] != '\0' && y + lineHeight <= text.y + text.height) {
      const std::string line = renderer.truncatedText(UI_10_FONT_ID, output_, text.width);
      renderer.drawText(UI_10_FONT_ID, text.x, y, line.c_str());
    }
  }

  /* Hold-to-talk capsule. Drawn and hit-tested from the same rect; inverted
   * while the worker reports an active turn. */
  const Rect talk = talkRect();
  const plus_voice::Status st = plus_voice::status();
  const bool talking = st.ptt || talkHeld_;
  const int radius = std::min(talk.height / 2, 40);
  if (talking) {
    renderer.fillRoundedRect(talk.x, talk.y, talk.width, talk.height, radius, Color::Black);
  } else {
    renderer.fillRoundedRect(talk.x, talk.y, talk.width, talk.height, radius, Color::White);
    renderer.drawRoundedRect(talk.x, talk.y, talk.width, talk.height, 2, radius, true);
  }
  /* The physical AI key toggles this capsule hands-free; the label has to say
   * which state the next press produces, not just "Talk". */
  const char* label = talking ? tr(STR_LISTENING) : tr(STR_PTT);
  const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, talk.x + (talk.width - labelWidth) / 2,
                    talk.y + (talk.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2, label, !talking,
                    EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
