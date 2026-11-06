#pragma once

#include <Arduino.h>

#include <atomic>
#include <string>
#include <vector>

#include "FontExtras.h"
#include "activities/UiListActivity.h"
#include "components/InlineConfirm.h"

// Downloads optional font-extension packs from the CPYOS manifest to the SD
// card. The firmware carries no font data for them; app0 and the OTA data-pack
// set stay unchanged. Fallback packs are registered as additive renderer
// fallbacks by SdCardFontSystem; the emoji pack replaces EmojiFont's partition
// subset at runtime.
class FontExtrasActivity final : public UiListActivity {
 public:
  enum class Purpose : uint8_t { Manage, ReaderPrompt };

  explicit FontExtrasActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              Purpose purpose = Purpose::Manage);

  static bool wasPromptShownThisBoot();
  static void suppressPromptThisBoot();

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void debugConfirm() override;
  void debugCancel() override;
  void drawFooter() override;
  bool preventAutoSleep() override {
    return state_ == State::FETCHING || state_ == State::DOWNLOADING;
  }
  bool skipLoopDelay() override { return state_ == State::FETCHING || state_ == State::DOWNLOADING; }
  bool wantsBandUpdates() const override { return false; }

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleCustomInput() override;
  bool paintOverlayBeforePresent() override;

 private:
  static constexpr freeink::ui::ActionId ACTION_CANCEL_DOWNLOAD = ACTION_USER;
  enum class State : uint8_t { LIST, FETCHING, DOWNLOADING, ERROR };
  enum class PackState : uint8_t { NotInstalled, Installed, Update };
  enum class PendingAction : uint8_t { None, Check, Download };
  enum class Job : uint8_t { None, Manifest, Download };

  Purpose purpose_;
  State state_ = State::LIST;
  InlineConfirm confirm_;

  std::vector<font_extras::Pack> packs_;
  std::vector<font_extras::InstalledPack> installed_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::vector<std::string> rowLabels_;
  std::vector<std::string> rowSubtitles_;
  std::vector<std::string> rowValues_;
  bool rowsDirty_ = true;

  std::string errorMessage_;
  std::string downloadTitle_;
  font_extras::Failure failure_ = font_extras::Failure::None;
  int retryPackIndex_ = -1;  // -1: retry the check; else re-download this pack
  bool offlineNotice_ = false;  // cached list shown after a failed refresh
  PendingAction pendingAction_ = PendingAction::None;
  int pendingDownloadIndex_ = -1;
  std::atomic<size_t> downloadProgress_{0};
  std::atomic<size_t> downloadTotal_{0};
  std::atomic<int> stage_{0};  // 0 prepare, 1 connect, 2 receive, 3 verify
  bool cancelRequested_ = false;
  uint32_t downloadStartedAt_ = 0;
  uint32_t lastPaintMs_ = 0;
  Rect waitCancelRect_{};
  Rect errorRetryRect_{};
  Rect errorBackRect_{};
  bool wifiOwned_ = false;
  bool timeSyncSuspended_ = false;

  // Network work runs on its own PSRAM-stack task (same discipline as the
  // browser and the JJWXC jobs). Blocking the UI task made Cancel unresponsive
  // during a TLS handshake and let a stalled connect look like a freeze.
  TaskHandle_t worker_ = nullptr;
  bool workerStackInPsram_ = false;
  std::atomic<bool> workerDone_{false};
  Job job_ = Job::None;
  int jobPackIndex_ = -1;
  bool jobOk_ = false;
  bool jobFromCache_ = false;
  font_extras::Failure jobFailure_ = font_extras::Failure::None;
  font_extras::Pack jobPack_;
  std::vector<font_extras::Pack> jobPacks_;
  std::string jobError_;

  PackState packState(const font_extras::Pack& pack) const;
  void rebuildRows();
  void startCheck();
  void ensureWifi();
  bool trySilentSavedNetwork();
  void onWifiSelectionComplete(bool success);
  void startDownload(int packIndex);
  void startDownloadNow(int packIndex);
  bool startWorker(Job job);
  static void workerTrampoline(void* param);
  void runWorker();
  void pollWorker();
  void reapWorker();
  void afterInstall(const font_extras::Pack& pack);
  void promptRemove(int packIndex);
  void removePack(int packIndex);
  void setError(const char* message, font_extras::Failure failure = font_extras::Failure::None);
  const char* failureText(font_extras::Failure failure) const;
  void drawWait();
  void drawErrorCard();
  bool handleErrorActions();
  bool handleOfflineNotice();
  void pollCancelInput();
  static void onCancel(const freeink::ui::ActionEvent& event, void* user);
};
