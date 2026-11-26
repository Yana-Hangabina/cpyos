#pragma once

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <string>
#include <vector>

#include "components/InlineConfirm.h"
#include "components/StatusCards.h"
#include "plugins/PluginCatalog.h"
#include "plugins/PluginManager.h"
#include "activities/UiListActivity.h"

// SD-installed native plugin picker and catalog installer.
//
// Rows follow the CPYOS list grammar: a utility row (rescan), a catalog row
// (check updates), installed packages with version/size/arch and a status, then
// catalog-only packages. Installed packages launch; update/not-installed rows
// download and install. Swipe/long-press deletes a local package after the
// system inline confirmation. Network work runs on a PSRAM worker and uses the
// same saved-network path as Font Extras; the card is the shared wait surface.
class PluginsActivity final : public UiListActivity {
 public:
  explicit PluginsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Plugins", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum class RowKind : uint8_t { PdfEngine, Rescan, Check, Installed, Catalog };
  enum class State : uint8_t { List, Fetching, Downloading, Error };
  enum class Job : uint8_t { None, Catalog, Install };

  // Pinned prefix rows; the installed/catalog packages follow in append order.
  // kPdfEngineRow only exists in PDF builds (see the #if in rebuildRows()).
  static constexpr int kPdfEngineRow = 0;
  static constexpr int kRescanRow = 1;
  static constexpr int kCheckRow = 2;
  static constexpr int kFirstPluginRow = 3;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool canSwipeDelete(int index) const override;
  void swipeDelete(int index) override;
  bool handleCustomInput() override;
  bool paintOverlayBeforePresent() override;
  const char* headerTitle() const override;
  void drawFooter() override;

  void rescan();
  void rebuildRows();
  void promptRemove(int row);
  int catalogIndexFor(const std::string& id) const;

  void startCheck();
  void startInstall(int catalogIndex);
  bool ensureWifi();
  bool startWorker(Job job);
  static void workerTrampoline(void* parameter);
  void runWorker();
  void pollWorker();
  void reapWorker();
  void finishWorkerState();
  void setError(const char* title, const char* body);
  void releaseNetwork();

  InlineConfirm confirm_;
  std::vector<native_plugins::PluginInfo> plugins_;
  std::vector<native_plugins::CatalogEntry> catalog_;
  bool catalogLoaded_ = false;

  std::vector<RowKind> rowKinds_;
  std::vector<int> rowIndexes_;
  std::vector<std::string> labels_;
  std::vector<std::string> subtitles_;
  std::vector<std::string> values_;
  std::vector<freeink::ui::ListItem> rows_;

  State state_ = State::List;
  std::string errorTitle_;
  std::string errorBody_;
  uint32_t waitStartedMs_ = 0;
  uint32_t lastWaitPaintMs_ = 0;
  Rect waitCancelRect_{};

  Job job_ = Job::None;
  native_plugins::CatalogEntry jobEntry_{};
  std::vector<native_plugins::CatalogEntry> jobCatalog_;
  std::string jobError_;
  bool jobOk_ = false;
  std::atomic<bool> workerDone_{false};
  bool cancelRequested_ = false;
  std::atomic<size_t> progressBytes_{0};
  std::atomic<size_t> totalBytes_{0};
  TaskHandle_t worker_ = nullptr;
  bool workerStackInPsram_ = false;

  bool wifiOwned_ = false;
  bool timeSyncSuspended_ = false;
};

#endif  // CROSSPOINT_PLUGIN_RUNTIME
