#include "PluginActivity.h"

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include "GfxRenderer.h"
#include "HalDisplay.h"
#include "Logging.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "I18n.h"
#include "components/StatusCards.h"
#include "plugins/PluginHost.h"

void PluginActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("PLUG", "open %s (%s)", info_.id.c_str(), info_.elfPath.c_str());
  startFailed_ = !PluginHost::instance().start(info_);
  if (startFailed_) {
    LOG_ERR("PLUG", "start failed: %s", PluginHost::instance().lastError());
    requestUpdate(true);
  }
}

void PluginActivity::onExit() {
  PluginHost::instance().stop();
  Activity::onExit();
}

void PluginActivity::loop() {
  PluginHost& host = PluginHost::instance();
  if (exitPending_) {
    // Wait for the injected Back release to leave the manager's input snapshot
    // before popping: otherwise the parent list sees the same edge and closes.
    if (!host.active() && !mappedInput.isPressed(MappedInputManager::Button::Back)) finish();
    return;
  }
  if (!host.active()) {
    if (host.state() == PluginHost::State::Failed && !startFailed_) {
      // Relocation/open failures happen on the loader task, after start()
      // already returned true. Show them instead of silently returning.
      startFailed_ = true;
      firstFrame_ = true;
      LOG_ERR("PLUG", "plugin %s failed: %s", info_.id.c_str(), host.lastError());
      requestUpdate(true);
    }
    if (!startFailed_) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      exitPending_ = true;
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    beginExit();
    return;
  }

  forwardButtons();
  forwardTouch();
}

void PluginActivity::beginExit() {
  if (PluginHost::instance().active()) PluginHost::instance().requestExit();
  exitPending_ = true;
}

void PluginActivity::debugBack() { beginExit(); }

void PluginActivity::debugMoveRing(int delta) {
  CpyosPluginEvent event{};
  event.type = CPYOS_EVENT_BUTTON_DOWN;
  event.button = delta > 0 ? CPYOS_BUTTON_DOWN : CPYOS_BUTTON_UP;
  PluginHost::instance().pushEvent(event);
}

void PluginActivity::debugConfirm() {
  CpyosPluginEvent event{};
  event.type = CPYOS_EVENT_BUTTON_DOWN;
  event.button = CPYOS_BUTTON_CONFIRM;
  PluginHost::instance().pushEvent(event);
}

void PluginActivity::forwardButtons() {
  struct Binding {
    MappedInputManager::Button button;
    uint16_t code;
  };
  static const Binding kBindings[] = {
      {MappedInputManager::Button::Confirm, CPYOS_BUTTON_CONFIRM},
      {MappedInputManager::Button::Left, CPYOS_BUTTON_LEFT},
      {MappedInputManager::Button::Right, CPYOS_BUTTON_RIGHT},
      {MappedInputManager::Button::Up, CPYOS_BUTTON_UP},
      {MappedInputManager::Button::Down, CPYOS_BUTTON_DOWN},
  };
  PluginHost& host = PluginHost::instance();
  for (const auto& binding : kBindings) {
    if (mappedInput.wasPressed(binding.button)) {
      CpyosPluginEvent event{};
      event.type = CPYOS_EVENT_BUTTON_DOWN;
      event.button = binding.code;
      host.pushEvent(event);
    }
    if (mappedInput.wasReleased(binding.button)) {
      CpyosPluginEvent event{};
      event.type = CPYOS_EVENT_BUTTON_UP;
      event.button = binding.code;
      host.pushEvent(event);
    }
  }
}

void PluginActivity::forwardTouch() {
  PluginHost& host = PluginHost::instance();
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    CpyosPluginEvent event{};
    event.type = CPYOS_EVENT_TAP;
    event.x = static_cast<int16_t>(x);
    event.y = static_cast<int16_t>(y);
    host.pushEvent(event);
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::None) return;
  CpyosPluginEvent event{};
  event.type = CPYOS_EVENT_SWIPE;
  switch (swipe) {
    case MappedInputManager::SwipeDir::Left:
      event.button = CPYOS_SWIPE_LEFT;
      event.dx = -1;
      break;
    case MappedInputManager::SwipeDir::Right:
      event.button = CPYOS_SWIPE_RIGHT;
      event.dx = 1;
      break;
    case MappedInputManager::SwipeDir::Up:
      event.button = CPYOS_SWIPE_UP;
      event.dy = -1;
      break;
    case MappedInputManager::SwipeDir::Down:
      event.button = CPYOS_SWIPE_DOWN;
      event.dy = 1;
      break;
    default:
      return;
  }
  host.pushEvent(event);
}

void PluginActivity::render(RenderLock&&) {
  if (!startFailed_ || !firstFrame_) return;
  firstFrame_ = false;
  const char* error = PluginHost::instance().lastError();
  status_card::paintMessage(renderer, tr(STR_ERROR_MSG),
                            error == nullptr || *error == '\0' ? "Plugin failed to start" : error);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

#endif  // CROSSPOINT_PLUGIN_RUNTIME
