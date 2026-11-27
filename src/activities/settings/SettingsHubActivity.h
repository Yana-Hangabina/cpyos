#pragma once

#include <GfxRenderer.h>
#include <I18nKeys.h>

#include <vector>

#include "activities/UiListActivity.h"

// One-level grouping pages reached from the Settings tabs:
//
//   Fonts   - text settings, font download, font extensions, dictionaries and
//             the vocabulary history. These five rows used to sit flat in the
//             Reader tab, mixing typography with asset management.
//   Updates - OTA check, SD-card firmware, the PDF engine and (Plus) vendor
//             device resources. One row per update path made the System tab
//             read like a list of maintenance chores.
//
// Rows are rebuilt from a static table on entry, so nothing here holds memory
// while a memory-hungry child (font download, OTA, engine image) is open.
class SettingsHubActivity final : public UiListActivity {
 public:
  enum class Page : uint8_t { Fonts, Updates, WebPlugins };

  SettingsHubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Page page)
      : UiListActivity("SettingsHub", renderer, mappedInput), page_(page) {}

  void onEnter() override;

 private:
  enum class Kind : uint8_t {
    TextSettings,
    FontDownload,
    FontExtras,
    Dictionaries,
    VocabularyToggle,
    VocabularyHistory,
    CheckUpdates,
    SdFirmware,
    PdfEngine,
    PlusResources,
    WebPluginToggle,
    WebPluginCatalog,
  };

  int listCount() const override { return static_cast<int>(rows_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  void rebuildRows();
  void releaseRows();
  void openChild(int index);

  Page page_;
  std::vector<Kind> kinds_;
  std::vector<freeink::ui::ListItem> rows_;
};