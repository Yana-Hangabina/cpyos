#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
//
// Recursive lookup: when armed with enableLookup() (the word-select screen
// passes the entry's DictLocation), Confirm or a long press selects a word in
// the displayed definition and looks it up without leaving this activity.
// Definitions are replaced one at a time — the current payload is released
// before the next entry is read — and a fixed-size back stack keeps the
// navigation metadata only. Back returns to the previous definition; a long
// Back jumps straight back to book word selection.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        content{.headword = std::move(headword), .text = std::move(definition)},
        htmlDefinition(htmlDefinition) {}

  // Arm recursive lookup. The dictionary must outlive this activity (it is the
  // parent word-select screen's member) and location must address the entry
  // this activity was constructed with.
  bool enableLookup(Dictionary& dictionary, DictLocation location);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `text`. Wrapping keeps lines
  // under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };

  // One definition level. Its payload (text, pages, lines) is released before
  // another entry is loaded, so two definitions never have to fit in memory.
  struct Definition {
    std::string headword;
    // Not const: prepareDefinition() normalizes embedded NULs (StarDict
    // multi-type separators) to newlines so C-string APIs see the whole text.
    std::string text;
    std::vector<std::unique_ptr<Page>> pages;
    std::vector<Line> lines;
    int currentPage = 0;
    int totalPages = 1;
    int linesPerPage = 1;

    void releasePayload() {
      std::string{}.swap(text);
      std::vector<std::unique_ptr<Page>>{}.swap(pages);
      std::vector<Line>{}.swap(lines);
    }
  };

  // Navigation metadata kept while a definition's payload is released. The
  // entry is re-read from the SD card on Back, which is why only the location
  // (not the text) is stored.
  struct HistoryEntry {
    std::string headword;
    DictLocation location;
    int page = 0;
    bool styled = false;
  };

  static constexpr size_t MAX_HISTORY = 16;

  struct LookupState {
    LookupState(Dictionary& dictionary, DictLocation location) : dictionary(dictionary), location(location) {}
    Dictionary& dictionary;  // owned by the retained DictionaryWordSelectActivity parent
    DictLocation location;   // exact entry currently displayed
    std::array<HistoryEntry, MAX_HISTORY> history;
    size_t depth = 0;
    std::string pendingWord;
  };

  BodyArea bodyArea() const;
  bool prepareDefinition(Definition& next, bool forcePlain = false);
  bool layoutHtmlPages(Definition& next);
  bool wrapText(Definition& next);
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY, int currentPage) const;
  // Plain-text page packed into a Page only while the selection screen runs.
  bool buildPlainSelectionPage();
  void startSelection();
  void navigateDefinition(bool back);
  bool restoreCurrentDefinition(bool styled);
  void showMessage(StrId message);

  Definition content;
  const bool htmlDefinition;
  std::unique_ptr<LookupState> lookup;
  // Plain-mode selection page: the child borrows it, so it lives only while
  // the selection screen is on top.
  std::unique_ptr<Page> selectionPage;
  StrId message = StrId::STR_DICT_NOT_FOUND;
  unsigned long messageTime = 0;
  bool messageVisible = false;
  bool initialLayoutFailed = false;
  ButtonNavigator buttonNavigator;
};
