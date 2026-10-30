#pragma once

#include <LibraryIndexFile.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

// One Library screen: every indexed book on the card shown by recency, title,
// or author. The Recent shelf orders by file modification time (when a book
// landed on the card) and pins the recently OPENED books from RecentBooksStore
// on top, so active reads and fresh arrivals share one list.
//
// The two-slot row is the whole point rather than a styling choice: the problem
// being solved is "I cannot find my books because I do not know the authors",
// and that is answered by a column the eye can sweep, not by a tidier filename.
//
// Rows render through fui::list on the UiTabListActivity ring (0 = the sort
// strip, 1..N = the books), which is what brings touch to rows and tabs. Titles
// are truncated to one line by the widget — more books on the screen, even if
// half a name is hidden.
//
// Only the visible window of rows is materialized per render (strings and
// ListItems for at most one page). The ordinary shelf therefore keeps one page
// of strings; an active search additionally uses one fallible uint16_t slot per
// indexed book so an allocation failure remains recoverable on the C3.
class LibraryListActivity final : public UiTabListActivity {
 public:
  LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  // The grid presents through the 16-level canvas (real cover tones, and no
  // GC16 promotion when the previous screen was Home). Band updates are 1-bit
  // only; the list layout keeps them.
  bool presentsGrayscale() const override { return usesCoverGrid(); }
  bool wantsBandUpdates() const override { return !usesCoverGrid(); }
  void debugOpenSearch() override { openSearch(); }
  void debugOpenSearchSample() override { query = "\xE6\x88\x91"; openSearch(); }

  // Debug-only: snapshot the exact pre-display framebuffer of this UI screen.
  void debugRequestSnapshot();
  void debugForceEmpty() override { debugEmpty_ = true; requestUpdate(); }
  static const uint8_t* debugSnapshotData(size_t& size);
  static void debugClearSnapshot();
  // The unified shelf is the Library tab's screen (it replaced FileBrowser in
  // the tab role; the file browser stays reachable from the home dashboard).
  MainTab mainTab() const override { return MainTab::Library; }

 protected:
  // --- UiListActivity / UiTabListActivity contract ---------------------------
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  int tabCount() const override;
  int activeTab() const override;
  const char* tabLabel(int index) const override;
  freeink::ui::TabIndicator tabIndicator(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  // The FreeInkUI header owns both the title and search touch target; CPY
  // overrides this to paint the main-tab band below.
  void drawChrome() override;
  void beforeDisplayHook() override;
  void captureSnapshot();
  void drawFooter() override;
  // OptionPopup is a self-contained modal: it owns rendering (and the button
  // hints) whenever it is up.
  void render(RenderLock&& lock) override;

 private:
  bool debugEmpty_ = false;
  // The screen's own actions, after the base's ACTION_ROW / ACTION_TAB.
  static constexpr freeink::ui::ActionId ACTION_SEARCH = ACTION_TAB_USER;
  static constexpr freeink::ui::ActionId ACTION_REBUILD = ACTION_SEARCH + 1;
  static constexpr freeink::ui::ActionId ACTION_BACK = ACTION_REBUILD + 1;
  static constexpr freeink::ui::ActionId ACTION_CHIP = ACTION_BACK + 1;
  static constexpr freeink::ui::ActionId ACTION_SORT = ACTION_CHIP + 1;
  // The sort lives in a dropdown capsule on the chip row (device feedback:
  // two stacked control rows wasted height and the design language is one
  // row of capsules).
  bool cpyHideTabStrip() const override { return true; }

  // Source filter (design frame 03's chip row). The WeRead chip filters the
  // books WeRead cached to /WeRead/*.epub: fully downloaded books DO land on
  // the card as readable files, so the chip selects real content (the earlier
  // "streams only, always empty" audit was wrong — verified on the device
  // log, 2026-10-16). The three named chips are disjoint: 本地 excludes both
  // app folders.
  enum class SourceChip : uint8_t { All, Local, WeRead, Jjwxc };
  SourceChip chipFilter = SourceChip::All;
  bool hasFilter() const { return !query.empty() || chipFilter != SourceChip::All; }

  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();

  // Input
  void openSelectedBook();
  void openSearch();
  static std::vector<std::string> liveResultsTrampoline(void* ctx, const char* query);
  std::vector<std::string> liveResults(const char* query);
  static void jjwxcSearchTrampoline(void* ctx, const char* query);
  void openJjwxcSearch(const char* query);
  // Shared tail of row activation and the options menu's Open entry.
  void openBookByPath(const std::string& path);
  void promptRebuildIndex();
  // The header refresh button's action: rescan the card AND, when a JJWXC
  // session exists, walk the online shelf for new chapters. The network part is
  // run by the app as a child activity (see JjwxcActivity::RefreshUpdates):
  // the shelf itself never brings up Wi-Fi, because a failed handshake on the
  // UI task is the 60-second freeze this project already paid for once.
  void syncShelfAndUpdates();
  void resetAfterRebuild();
  // Recent-row long-press menu: open / remove from recents / delete / rebuild.
  void showRecentBookOptions(int entry);
  void promptRemoveRecentBook(const std::string& path, const std::string& title);
  // Long-press delete owns the gesture where grouping does not apply: the
  // Recent sort, degraded lists, and any active search result.
  bool deleteEligible() const;
  // Resolves the row's path and title, then confirms via promptDeleteBookByPath.
  void promptDeleteBook(int entry);
  void promptDeleteBookByPath(const std::string& path, const std::string& title);
  bool collapseGroups(int bookEntry);
  void expandGroup(int groupEntry);
  void restoreExpandedList();
  void selectTab(int index, bool toggleIfActive);
  void toggleSortDirection();
  // Sub-screens act on button press, so a button still held when we resume must
  // not also act here. Records what to swallow on the next release.
  void swallowHeldReleases();
  // Staged back-out shared by Button::Back and the header back arrow.
  void handleBackAction();
  static void searchActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void rebuildActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void backActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void chipActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void sortActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  // Applies a chip selection and re-runs the filter when it changed.
  void setSourceChip(int value);
  // The sort dropdown: options over the three orders plus a direction
  // toggle, applied through selectTab so the list rebuild stays in one path.
  void promptSortMenu();

  // Data
  void applyFilter();
  int bookRowCount() const;
  int rowFor(int entry) const;

  // --- CPY cover grid (refreshed UI frame 02), the default shelf style -----------
  // Reads SETTINGS.inxLibraryLayout (the settings menu's 书架显示 selector):
  // Icons -> cover grid, List -> the text rows. Covers are prewarmed one per
  // frame: EPUB parsing is far too slow to do several in one pass.
  // Four columns at the mockup's 188:250 cover ratio; the slot table is sized
  // for the tallest page this geometry can show (5 rows) plus one.
  static constexpr int kGridCols = 3;
  static constexpr int kGridCoverSlots = 20;
  enum class GridCover : uint8_t { Unchecked, Ready, Missing, Unavailable };
  GridCover gridCovers[kGridCoverSlots] = {};
  std::string gridCoverPaths[kGridCoverSlots];
  int16_t gridSlotEntry[kGridCoverSlots] = {};
  // Bumped whenever the entry list's meaning changes (filter, search, sort,
  // rebuild). Row POSITIONS renumber across those, so a slot must not keep a
  // cover just because "position 1" still exists.
  uint16_t gridEpoch = 0;
  // Off-screen cover prefetch cursor (one generation per frame, monotonic;
  // reset by an epoch change) and its done flag.
  int gridPrefetchCursor = 0;
  bool gridPrefetchDone = false;
  uint16_t gridPrefetchEpoch = 0xFFFF;
  // Resolves an entry to its book path (pinned recents, else the index).
  bool slotBookPath(int entry, std::string& path);
  uint16_t gridSlotEpoch[kGridCoverSlots] = {};
  int gridTopRow = 0;
  // Measured page height in grid ROWS (written while rendering). The swipe handler
  // steps the window by this many rows, so one turn equals one page rather than one row.
  int gridPageRows_ = 0;
  bool usesCoverGrid() const;
  void buildCoverGrid(UiScreen& screen);
  bool prepareNextGridCover();
  // fileName, when asked for, is the on-card name the row's icon derives from
  // (the display title may come from metadata and carry no extension).
  bool rowTextFor(int entry, std::string& title, std::string& author, std::string* fileName = nullptr);
  // Appends " · <source>" to the author line for books that came from a store
  // app (today: JJWXC exports under /JJWXC/, which the card-wide index picks
  // up like any other book — the shelf already mixes sources), plus the
  // "· N new" tail when the update snapshot says the card is behind.
  void appendSourceSuffix(int entry, std::string& author);
  // The chip-filtered empty shelf is an invitation, not a dead end: these
  // return the source app's title and open it. nullptr when the empty shelf is
  // not source-specific (no filter, or a search with no hits).
  const char* emptySourceTitle() const;
  void openEmptySource();
  // Painted by render() and hit by handleCustomInput(); both read this one
  // rect, which is the rule for any control drawn outside the fui list.
  Rect emptySourceRect_{};
  // Per-cell source badge, "N new" count and reading progress, cached exactly
  // like the cover slot it belongs to: entry+epoch changes reset it, a plain
  // repaint reuses it. Progress 0xFF = the stats store has no record for the
  // path, which draws NOTHING — "0 %" would be a claim we cannot make.
  uint8_t gridSlotSource[kGridCoverSlots] = {};  // 0 unknown, 1 local, 2 WeRead, 3 JJWXC
  uint16_t gridSlotNew[kGridCoverSlots] = {};    // new chapters (0 = unknown/none)
  uint8_t gridSlotProgress[kGridCoverSlots] = {};  // 0xFF = unknown
  void drawGridBadges(int slot, const Rect& cover) const;
  // Paints the invitation pill inside `band` and records emptySourceRect_.
  void drawEmptySourceAction(const Rect& band);

  // --- refreshed UI frame 02 shelf chrome -----------------------------------------
  // Headline row (serif page title, shelf count, quiet search/sync actions)
  // and the source tab strip with real counts under it. Both are drawn and
  // hit-tested here because the fui header no longer owns the CPY action row.
  void drawHeadline(UiScreen& screen);
  void drawSourceTabs(UiScreen& screen);
  // One pass over the index per index generation; the counts are facts about
  // the card, so an unread index draws no numbers rather than zeros.
  struct ShelfCounts {
    uint32_t stamp = 0;
    uint16_t total = 0;
    uint16_t local = 0;
    uint16_t weread = 0;
    uint16_t jjwxc = 0;
    bool valid = false;
  };
  ShelfCounts shelfCounts_;
  uint32_t indexStamp = 1;
  void ensureShelfCounts();

  // --- refreshed UI frame 15 empty shelf ------------------------------------------
  // Three concrete ways to put a first book on the card, each a real
  // destination. Drawn after the frame (like the invitation pill), so the hit
  // rects live here and are tested in handleCustomInput().
  static constexpr int kEmptyPathKinds = 3;
  Rect emptyPathRects_[kEmptyPathKinds] = {};
  uint8_t emptyPathKind_[kEmptyPathKinds] = {};
  int emptyPathCount_ = 0;
  bool showEmptyPaths() const;
  int emptyPathRowCount() const;
  void drawEmptyPaths(const Rect& band);
  void openEmptyPath(int index);
  // Message for the empty placeholder (query/search/unavailable), shared by
  // buildScreen and the render-time backdrop painter. The source-specific text
  // is formatted into this buffer, which is why the method is logically const
  // but mutates only that buffer.
  const char* emptyStateMessage() const;
  mutable std::string emptySourceMessage_;
  uint32_t titleInitialFor(int entry);
  bool buildGroupStarts();
  int groupForBook(int bookEntry) const;
  bool groupable() const;

  // Screen building
  void buildHeader(UiScreen& screen);
  // Materializes ListItems and their strings for the visible window only.
  void buildRows(UiScreen& screen);
  static void formatInitialHeading(uint32_t initial, std::string& out);
  void formatAuthorHeading(const std::string& author, std::string& out) const;
  void drawPositionReadout() const;
  void drawHoldHelp() const;
  const char* headerTitle() const override;

  // Ring 0 is the strip; the selected BOOK is ring - 1, with the strip keeping
  // row 0 as the working selection exactly as the pre-ring code did.
  int selectedEntry() const;
  bool tabsFocused() const { return ringPos() == 0; }

  // --- pinned recently-opened overlay ---------------------------------------
  // On the unfiltered Recent shelf the RecentBooksStore entries sit on top, in
  // read order; the modification-time list follows with those books skipped.
  // Entries below pinnedCount() are store rows; the rest go through rowFor().
  int pinnedCount() const;
  // Re-match the store against the index (chunked scan). Call whenever the
  // index or the store changes.
  void resolvePinned();
  // Direction-space translation of the matched rows. Call on sort toggles.
  void refreshOverlap();

  library::LibraryIndexFile index;
  int activeTabIndex = 0;
  library::SortOrder sortOrder = library::SortOrder::RecentDesc;
  // One bit per tab; Recent starts descending (newest first).
  uint8_t descendingTabs = 1u << 0;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this owns no allocation, so the ordinary
  // shelf pays nothing proportional to the library for the feature.
  std::string query;
  // The active query, pre-quoted for the header: headerTitle() returns a
  // stable c_str the render task can hold across a build.
  std::string headerSearchTitle;
  std::unique_ptr<uint16_t[]> filtered;
  uint16_t filteredCount = 0;
  bool filterFailed = false;

  // One start row per group. Grouping is only offered for the sorted <=512-book
  // index, so this fallible allocation is at most 1 KiB and is reused after its
  // first successful allocation.
  std::unique_ptr<uint16_t[]> groupStarts;
  uint16_t groupCapacity = 0;
  uint16_t groupCount = 0;
  bool groupsCollapsed = false;
  freeink::ui::ListNav expandedNav;

  // Visible-window row storage, reused across renders (buildRows). Bounded by
  // the densest page, never by the library. Headings get their own storage:
  // the surname-first inversion must not overwrite the author slot, whose raw
  // value the next row's group comparison reads.
  std::vector<freeink::ui::ListItem> winItems;
  std::vector<std::string> winTitles;
  std::vector<std::string> winAuthors;
  std::vector<std::string> winHeaders;

  // Pinned overlay state: per store entry its RecentAsc row (0xFFFF when the
  // book is not in the index), and the current-direction rows to skip, sorted
  // ascending, so unpinned entries map to sort rows with a <=10-step walk.
  uint16_t pinnedAscRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint16_t overlapRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint8_t pinnedTotal = 0;
  uint8_t overlapCount = 0;

  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // Row options modal (Recent long-press menu); owned here so it outlives the
  // touch event that opened it.
  OptionPopup optionPopup;
};
