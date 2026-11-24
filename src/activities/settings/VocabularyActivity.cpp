#include "VocabularyActivity.h"

#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>

#include "components/UITheme.h"
#include "vocabulary/VocabularyStore.h"

namespace fui = freeink::ui;

namespace {

std::string jsonField(const std::string& line, const char* key) {
  const std::string pat = std::string("\"") + key + "\":\"";
  const size_t pos = line.find(pat);
  if (pos == std::string::npos) return {};
  std::string out;
  for (size_t i = pos + pat.size(); i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') break;
    if (c == '\\' && i + 1 < line.size()) {
      const char n = line[++i];
      out += (n == 'n' || n == 'r' || n == 't') ? ' ' : n;
    } else {
      out += c;
    }
  }
  return out;
}

std::string truncateUtf8(std::string text, size_t maxBytes) {
  if (text.size() <= maxBytes) return text;
  size_t cut = maxBytes;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  text.resize(cut);
  return text;
}

}  // namespace

VocabularyActivity::VocabularyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Vocabulary", renderer, mappedInput) {}

const char* VocabularyActivity::headerTitle() const { return tr(STR_VIEW_VOCABULARY); }

void VocabularyActivity::onEnter() {
  UiListActivity::onEnter();
  entries_.clear();
  rowItems_.clear();

  HalFile file;
  if (!Storage.openFileForRead("VOCAB", VocabularyStore::FILE_PATH, file)) {
    rowItems_.resize(1);
    rowItems_[0].label = tr(STR_NO_VOCABULARY);
    requestUpdate();
    return;
  }

  // Stream in small chunks; one lookup line is ~1-4 KB and the file can be
  // much larger than internal RAM. Keep only the newest kMaxRows entries.
  std::string line;
  line.reserve(2048);
  char buf[512];
  bool first = true;
  const auto take = [&](std::string&& record) {
    if (first) {  // skip a UTF-8 BOM on the first record
      first = false;
      if (record.size() >= 3 && static_cast<unsigned char>(record[0]) == 0xEF &&
          static_cast<unsigned char>(record[1]) == 0xBB && static_cast<unsigned char>(record[2]) == 0xBF) {
        record.erase(0, 3);
      }
    }
    if (record.empty()) return;
    std::string word = jsonField(record, "word");
    if (word.empty()) word = jsonField(record, "headword");
    if (word.empty()) return;
    Entry entry;
    entry.word = truncateUtf8(std::move(word), 96);
    std::string chapter = jsonField(record, "chapter");
    if (chapter.empty()) chapter = jsonField(record, "title");
    entry.context = truncateUtf8(std::move(chapter), 80);
    entries_.push_back(std::move(entry));
    if (entries_.size() > static_cast<size_t>(kMaxRows)) entries_.erase(entries_.begin());
  };

  while (true) {
    const int got = file.read(buf, sizeof(buf));
    if (got <= 0) break;
    for (int i = 0; i < got; ++i) {
      if (buf[i] == '\n') {
        take(std::move(line));
        line.clear();
      } else if (line.size() < 4096) {
        line += buf[i];
      }
    }
  }
  if (!line.empty()) take(std::move(line));
  file.close();

  std::reverse(entries_.begin(), entries_.end());  // newest first
  rowItems_.resize(entries_.empty() ? 1 : entries_.size());
  if (entries_.empty()) {
    rowItems_[0].label = tr(STR_NO_VOCABULARY);
  } else {
    for (size_t i = 0; i < entries_.size(); ++i) {
      rowItems_[i].label = entries_[i].word.c_str();
      rowItems_[i].value = entries_[i].context.c_str();
    }
  }
  requestUpdate();
}

void VocabularyActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<int>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
