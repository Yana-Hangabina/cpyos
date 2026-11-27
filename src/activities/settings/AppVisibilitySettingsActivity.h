#pragma once

#include <vector>

#include "activities/UiListActivity.h"

class AppVisibilitySettingsActivity final : public UiListActivity {
 public:
  explicit AppVisibilitySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void drawFooter() override;

  std::vector<freeink::ui::ListItem> rowItems;
  // Row -> app_catalog index; -1 marks a section header (not selectable).
  std::vector<int16_t> rowAppIndex;
  // Geometry captured in buildScreen() for the per-section card rings drawn in
  // drawFooter() after the list pass (same visual language as Settings).
  int16_t listTop_ = 0;
  int16_t listBottom_ = 0;
  int16_t listRowH_ = 0;
  bool waitForConfirmRelease = false;
  bool dirty = false;
};
