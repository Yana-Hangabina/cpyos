#pragma once

#include <atomic>
#include <string>

#include "activities/Activity.h"
#include "plus_ota.h"

// Vendor OTA page (CloudZao Burner). Two buttons: check / switch channel, and
// install once an update is offered. All network + flash work runs on a PSRAM
// worker task (background_work guard), never on the UI task.
class PlusOtaActivity final : public Activity {
 public:
  explicit PlusOtaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PlusOta", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void debugConfirm() override;
  void debugCancel() override;
  // Bench hooks (shell_demo): check now / install a direct URL / pick channel.
  void debugCheck() override;
  void debugInstallUrl(const char* url) override;
  void debugSetChannel(int channel) override;
  bool preventAutoSleep() override { return busy(); }
  bool skipLoopDelay() override { return busy(); }

 private:
  enum class State : uint8_t { Idle, Checking, Available, Downloading, Installing, Done, Failed };
  enum class Job : uint8_t { None, Check, Install };

  bool busy() const { return job_ != Job::None; }
  void startJob(Job job);
  void startInstall(const plus_ota::Info& info);
  void startInstallUrl(const char* url);
  static void workerTrampoline(void* param);
  void runWorker();
  void pollWorker();
  void reapWorker();
  void onPrimary();
  void onSecondary();
  void setError(const std::string& text);
  bool ensureWifi();
 public:
  // Called from the download progress callback (worker task).
  void publishProgress(size_t done, size_t total) {
    progressBytes_.store(done);
    progressTotal_.store(total);
  }
 private:

  State state_ = State::Idle;
  Job job_ = Job::None;
  plus_ota::Channel channel_ = plus_ota::Channel::Stable;
  plus_ota::Info info_;
  std::string error_;
  std::atomic<bool> workerDone_{false};
  std::atomic<size_t> progressBytes_{0};
  std::atomic<size_t> progressTotal_{0};
  std::atomic<int> stage_{0};  // 0 check, 1 download, 2 install
  bool cancelRequested_ = false;
  TaskHandle_t worker_ = nullptr;
  bool workerStackInPsram_ = false;
  bool rebootPending_ = false;
  unsigned long rebootAt_ = 0;
  Rect primaryRect_{};
  Rect secondaryRect_{};
};
