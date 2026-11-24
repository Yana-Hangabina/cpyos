#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Read-only view of /Vocabulary.txt (JSON Lines written by the dictionary when
// "Save vocabulary lookups" is on). Newest first, at most kMaxRows rows.
class VocabularyActivity final : public UiListActivity {
 public:
  explicit VocabularyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  static constexpr int kMaxRows = 200;

  struct Entry {
    std::string word;
    std::string context;
  };

  int listCount() const override { return static_cast<int>(entries_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
  const char* headerTitle() const override;

  std::vector<Entry> entries_;
  std::vector<freeink::ui::ListItem> rowItems_;
};
