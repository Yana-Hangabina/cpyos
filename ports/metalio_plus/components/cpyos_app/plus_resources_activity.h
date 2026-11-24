#pragma once

#include <Arduino.h>

#include <atomic>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "plus_resources.h"

// Settings -> System -> Device resources on the Metalio E-INK4 Plus.
//
// The page is intentionally small: the first two rows bind the device and fetch
// the push-resource list; the remaining rows are the service's BOOK/BADGE/FONT
// tasks. A tap downloads one item, verifies size + SHA-256, installs it, and
// only then sends the server acknowledgement. Network and SD work runs on a
// PSRAM worker so the UI keeps its Cancel path alive.
class PlusResourcesActivity final : public UiListActivity {
 public:
  explicit PlusResourcesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("PlusResources", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void debugConfirm() override;
  void debugCancel() override;

  bool preventAutoSleep() override { return busy(); }
  bool skipLoopDelay() override { return busy(); }
  bool wantsBandUpdates() const override { return false; }

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleCustomInput() override;
  void drawFooter() override;
  bool paintOverlayBeforePresent() override;

 private:
  enum class State : uint8_t { List, Connecting, Binding, Fetching, Downloading, Error };
  enum class Job : uint8_t { None, Bind, Fetch, Download, Probe };
  enum class PendingAction : uint8_t { None, Bind, Fetch, Download, Probe };

  bool busy() const { return job_ != Job::None; }
  void rebuildRows();
  void startBind();
  void startFetch();
  void startDownload(int item_index);
  void queueJob(PendingAction action, int item_index);
  void beginBindJob();
  void beginFetchJob();
  void beginDownloadJob(int item_index);
  void beginProbeJob();
  void startProbe(bool user_initiated = true, bool from_enter = false);
  void scheduleProbe();
  void drawBindGuide();
  bool handleBindGuideInput();
  bool ensureWifi(bool allow_picker);
  void onWifiSelectionComplete(bool success);
  bool startWorker(Job job, int item_index);
  static void workerTrampoline(void* param);
  void runWorker();
  void pollWorker();
  void reapWorker();
  void releaseOwnedRadio();
  void setError(std::string message, Job retry_job = Job::None, int retry_index = -1);
  void retryLastJob();
  void drawWait();
  void drawErrorCard();
  bool handleErrorActions();
  void pollCancelInput();
  static void progressThunk(size_t done, size_t total, void* ctx);
  static void onCancel(const freeink::ui::ActionEvent& event, void* user);

  State state_ = State::List;
  Job job_ = Job::None;
  Job retry_job_ = Job::None;
  int retry_index_ = -1;
  int job_index_ = -1;
  PendingAction pending_action_ = PendingAction::None;
  int pending_index_ = -1;
  bool fetched_ = false;
  bool bound_ = false;
  bool bind_guide_ = false;
  bool auto_probe_pending_ = false;
  bool probe_from_enter_ = false;
  // While the guide is waiting for the mini-program, each probe must poll
  // /device-tokens (the exchange can only happen once); a plain resource fetch
  // can never turn an approved pairing into tokens.
  bool probe_polls_tokens_ = false;
  // 409 activation_unavailable right after removing the device in the
  // mini-program is transient (platform propagation); keep requesting a code
  // instead of parking on an error page.
  bool retry_pairing_ = false;
  std::string bind_url_;
  uint32_t next_probe_ms_ = 0;
  uint32_t probe_deadline_ms_ = 0;
  int probe_attempts_ = 0;
  bool wifi_owned_ = false;
  bool time_sync_suspended_ = false;

  plus_resources::Items items_;
  plus_resources::Item job_item_;
  plus_resources::BindResult bind_result_;
  plus_resources::FetchResult fetch_result_;
  plus_resources::TransferResult transfer_result_;

  std::string activation_code_;
  std::string bind_message_;
  std::string error_message_;
  std::string download_title_;
  std::vector<freeink::ui::ListItem> row_items_;
  std::vector<std::string> row_labels_;
  std::vector<std::string> row_subtitles_;
  std::vector<std::string> row_values_;
  bool rows_dirty_ = true;

  std::atomic<bool> worker_done_{false};
  std::atomic<size_t> progress_bytes_{0};
  std::atomic<size_t> progress_total_{0};
  std::atomic<int> stage_{0};  // 0 prepare, 1 connect, 2 receive, 3 verify
  bool cancel_requested_ = false;
  TaskHandle_t worker_ = nullptr;
  bool worker_stack_in_psram_ = false;
  uint32_t job_started_ms_ = 0;
  uint32_t last_paint_ms_ = 0;

  Rect wait_cancel_rect_{};
  Rect error_retry_rect_{};
  Rect error_back_rect_{};
  Rect bind_check_rect_{};
  Rect bind_retry_rect_{};
  Rect bind_back_rect_{};
};
