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
  // `initialTab` lets the app launcher open the tool drawer directly; the
  // default keeps the historical Settings landing tab for the main tabs.
  MyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int initialTab = -1)
      : Activity("My", renderer, mappedInput), initialTab_(initialTab) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  MainTab mainTab() const override { return MainTab::Settings; }
  // The tool drawer is the only tab with a movable cursor; the other two
  // repaint a large area, so the renderer falls back to a full page push there.
  bool wantsBandUpdates() const override { return true; }
  void debugSetTab(int tab) override {
    if (tab < 0 || tab >= kTabCount) return;
    tab_ = static_cast<Tab>(tab);
    requestUpdate();
  }
  void debugMoveRing(int delta) override;

 private:
  enum class Tab : uint8_t { Records = 0, Tools = 1, Settings = 2 };
  static constexpr int kTabCount = 3;
  // Two local tools (join network, file browser) + the compiled app catalog +
  // the plugin-directory tile. The drawer is a 4-column grid.
  static constexpr int kToolMax = 2 + 32 + 1;
  static constexpr int kToolColumns = 4;

  Rect tabRect(int index) const;
  Rect bodyRect() const;
  Rect cardRect(int index) const;
  Rect cardRowRect(int card, int row) const;
  Rect toolCellRect(int index) const;
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
  int toolCount() const;
  void openTool(int index);
  void openBookList();

  Tab tab_ = Tab::Settings;
  int initialTab_ = -1;
  int toolSelected_ = 0;
  uint64_t storageFreeBytes_ = 0;
  uint64_t storageTotalBytes_ = 0;
  bool storageKnown_ = false;
};
