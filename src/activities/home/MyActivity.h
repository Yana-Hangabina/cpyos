#pragma once

#include <I18n.h>

#include <string>

#include "activities/Activity.h"

namespace freeink {
struct Icon;
}

// Fourth main tab, refreshed UI frames 09/10/11: three tabs (记录 / 工具 / 设置).
// 记录 is the printed reading chart, 工具 is the 4 x 3 icon drawer, and 设置 is
// the 2 x 2 card dashboard whose rows open the existing category pages.
class MyActivity final : public Activity {
 public:
  MyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("My", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  MainTab mainTab() const override { return MainTab::Settings; }
  void debugSetTab(int tab) override {
    if (tab < 0 || tab >= kTabCount) return;
    tab_ = static_cast<Tab>(tab);
    requestUpdate();
  }

 private:
  enum class Tab : uint8_t { Records = 0, Tools = 1, Settings = 2 };
  static constexpr int kTabCount = 3;
  static constexpr int kToolCount = 12;

  Rect tabRect(int index) const;
  Rect bodyRect() const;
  Rect cardRect(int index) const;
  Rect cardRowRect(int card, int row) const;
  Rect toolCellRect(int index) const;
  Rect toolPluginRect() const;
  Rect quickPillRect(int index) const;
  Rect searchRowRect() const;
  Rect aboutRect() const;

  void drawTabs();
  void drawRecords();
  void drawTools();
  void drawSettings();
  void drawSettingsCard(int index);
  void drawQuickPills();
  void drawSearchRow();
  void drawAboutRow();
  void drawWaveLine(int x, int y, int width);
  void drawRipples(int x, int y, int maxRadius);

  bool handleTabTouch(int x, int y);
  bool handleRecordTouch(int x, int y);
  bool handleToolTouch(int x, int y);
  bool handleSettingsTouch(int x, int y);
  void activateTab(Tab tab);
  void openSettingsCategory(int category);
  void openTool(int index);
  void openBookList();

  Tab tab_ = Tab::Settings;
  int toolSelected_ = 0;
  uint64_t storageFreeBytes_ = 0;
  uint64_t storageTotalBytes_ = 0;
  bool storageKnown_ = false;
};
