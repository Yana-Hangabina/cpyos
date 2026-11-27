#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "I18n.h"
#include "activities/Activity.h"

// Pond v5 frame 10's "搜索设置": a global, read-only search over the setting
// labels and category names. A hit opens that category (or the hub that owns
// the row); the search itself never changes a setting.
class SettingsSearchActivity final : public Activity {
 public:
  SettingsSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialQuery = {})
      : Activity("SettingsSearch", renderer, mappedInput), query_(std::move(initialQuery)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Hit {
    StrId label;
    int category = -1;
    // -1 = open the category page; 0 = Fonts & Dictionaries hub; 1 = Updates
    // hub; 2 = Web plugins. Action rows are not part of getSettingsList(), so this table is what
    // keeps "下载字体" or "更新" findable at all.
    int8_t hub = -1;
  };

  void prompt();
  void buildResults();
  void activate(int index);

  std::string query_;
  std::vector<Hit> hits_;
  int selected_ = 0;
  int scroll_ = 0;
};
