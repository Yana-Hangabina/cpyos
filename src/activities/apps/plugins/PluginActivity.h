#pragma once

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <utility>

#include "activities/Activity.h"
#include "plugins/PluginManager.h"

// Full-surface host for one native plugin. The plugin task owns rendering and
// presentation through the ABI; this activity only forwards input and enforces
// Back as a cooperative shutdown.
class PluginActivity final : public Activity {
 public:
  PluginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                 native_plugins::PluginInfo info)
      : Activity("Plugin", renderer, mappedInput), info_(std::move(info)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Bench hooks: replay a d-pad press through the same plugin event queue.
  void debugMoveRing(int delta) override;
  void debugConfirm() override;
  void debugBack() override;

 private:
  void beginExit();
  void forwardButtons();
  void forwardTouch();

  native_plugins::PluginInfo info_;
  bool startFailed_ = false;
  bool firstFrame_ = true;
  bool exitPending_ = false;
};

#endif  // CROSSPOINT_PLUGIN_RUNTIME
