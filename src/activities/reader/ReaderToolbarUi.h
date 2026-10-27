#pragma once

#include <functional>
#include <string>

#include "components/UiAppHost.h"
#include "components/lists/list.h"

class GfxRenderer;
class MappedInputManager;

// FreeInkUI chrome for the toolbar reader menu (Settings -> Reader -> Reader
// Menu Style -> Toolbar), painted over the page that is already on screen:
//
//  - Toolbar: a bottom sheet holding a chapter scrub row (< capsule >) and a
//    Contents / Text / More tile row; the page above it stays untouched.
//  - Panel: a taller bottom sheet with a title, a paged row list (name left,
//    value right) and the same tile row as a switcher.
//
// EpubReaderActivity owns the state (which overlay, selection, row contents)
// and the physical-button handling; this class owns the layout, the tap
// targets FreeInkUI registers for it, and the translation of dispatched
// actions back into the small event set the reader acts on. Nothing here
// clears the screen: the page stays visible around the chrome.
class ReaderToolbarUi : public UiAppHost {
 public:
  enum class Event {
    None = 0,
    Dismiss = 1,
    Tool = 2,
    PrevChapter = 3,
    NextChapter = 4,
    Scrub = 5,
    Row = 6,
    ChapterAction = 7,
    // The fixed maintenance strip above the Contents list (JJWXC only).
    UpdateList = 8,
    // The fixed strip above the Bookmarks list (add/remove page bookmark).
    BookmarkAction = 9,
    // Bookmarks panel chips: switch the list between bookmarks and annotations.
    BookmarkChip = 10,
    AnnotationChip = 11,
    // The fixed strip in the annotation view (write the export file).
    ExportAnnotations = 12,
    // Contents filter chips (JJWXC): 0 all, 1 cached, 2 not cached.
    ContentsFilter = 13,
    // The type panel's "Done" pill: close the sheet back to the page.
    TextDone = 14,
    // The type grid's 56x56 steppers: value = cell index.
    TextStepMinus = 15,
    TextStepPlus = 16
  };

  // Row-tail state/action icons used by the JJWXC Contents panel. The state
  // badge is a square bordered glyph (FREE / VIP / VIP+yen / lock); the action
  // is a frameless download/refresh glyph. Both are drawn at their native
  // 1-bpp size, Mask1, so they invert cleanly on the selected row.
  enum class TailBadge : uint8_t { None, Free, Vip, VipYen, Lock };
  enum class TailAction : uint8_t { None, Download, Refresh, Delete };
  struct RowTail {
    TailBadge badge = TailBadge::None;
    TailAction action = TailAction::None;
  };

  struct Model {
    int16_t screenWidth = 684;
    bool panel = false;  // false = toolbar, true = a Contents/Text/More panel
    // Toolbar
    const char* chapterTitle = nullptr;
    const char* pageInfo = nullptr;  // "12/40   51%"
    int progressPermille = 0;        // 0..1000 book progress (scrub handle)
    // Panel
    const char* panelTitle = nullptr;
    int itemCount = 0;
    int selectedIndex = -1;  // row the buttons' cursor sits on; -1 = none shown
    std::function<std::string(int)> rowText;
    std::function<std::string(int)> rowValue;
    // Contents only: a row-tail chip and the persistent current-chapter bar.
    // Both indexes are TOC row indexes; the update command is chrome above the
    // list and no longer consumes a row.
    std::function<RowTail(int)> rowTail;
    // Index (in this panel's own list) that gets the persistent left bar:
    // the current chapter in Contents, the bookmark on the open page in
    // Bookmarks.
    int currentChapterIndex = -1;
    bool fullRowSelection = false;  // invert the whole cursor row (no side bar)
    // JJWXC Contents: draw the fixed "update chapter list" strip above the
    // list. It is chrome, not a selectable row: it never scrolls away and the
    // capacitor-key cursor never lands on it.
    bool contentsUpdateAction = false;
    // JJWXC Contents: show the 全部 / 已缓存 / 未缓存 chips above the list.
    bool contentsFilters = false;
    int contentsFilterIndex = 0;
    // Bookmarks: the same fixed strip, for "add/remove page bookmark".
    bool bookmarkAction = false;
    bool bookmarkActionRemove = false;
    // Bookmarks panel: two chips above the list switch between the bookmark
    // list and the annotation list; the counts ride on the chips.
    bool bookmarkChips = false;
    bool annotationsChip = false;
    int bookmarkCount = 0;
    int annotationCount = 0;
    // Annotation view: the fixed strip is "export annotations", not "add
    // bookmark" — adding one is a long press on the page.
    bool annotationExportAction = false;
    void (*rowCheckbox)(void* ctx, int index, freeink::ui::ListItem& item) = nullptr;
    void* rowCheckboxContext = nullptr;
    // Tile row: the tool in focus (toolbar) / the open panel (panel). 0..2.
    int activeTool = 0;
    // Button boards keep the theme's denser list row height (as every other
    // list does there); touch boards use FreeInkUI's finger-sized rows.
    bool denseRows = false;
  };

  struct Routed {
    Event event = Event::None;
    int value = 0;        // Tool: tool index; Row: row index
    int permille = -1;    // Scrub: 0..1000 along the track
    bool routed = false;  // the gate was open and a touch frame was routed
    int x = 0;            // touch position of the routed frame (logical px)
    int y = 0;
  };

  explicit ReaderToolbarUi(GfxRenderer& renderer);

  // Screen-entry reset + action wiring. Call once when the overlay opens.
  void begin();
  // What the next render() draws. The strings must stay alive until render()
  // returns; the row callbacks are invoked during render() for the visible rows.
  void setModel(const Model& model) { model_ = model; }
  // Paint into the framebuffer (no refresh, no clear). Run from the render task.
  void render();
  // Draw the 604 px Contents focus frame after the FreeInkUI canvas is
  // blitted; the list component clips its own target at the title column.
  void drawContentsFocusFrame() const;
  // Route one loop-task input frame; returns the action it mapped to, if any.
  Routed route(const MappedInputManager& input);

  // Panel list selection/viewport, shared with the reader's input handling.
  // The same fui::ListNav the list-menu screens use: scrollBy() pages the
  // viewport (measured page size, no-op detection), the top/selected fields
  // are the live state, and buildPanel() syncs it into the list each build.
  freeink::ui::ListNav& nav() { return nav_; }
  // Rows one page holds, measured after the first render.
  int visibleRows() const { return nav_.pageRows(); }
  // Current sheet's top edge in logical screen pixels, and the bottom of the
  // grabber band. The reader uses these to tell a grabber swipe (collapse /
  // expand) from an ordinary list scroll.
  int sheetTop() const { return sheetTop_; }
  int grabberZoneBottom() const { return sheetTop_ + 64; }

 private:
  freeink::ui::Rect contentsFocusRect_{};
  GfxRenderer& renderer_;
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void buildToolbar(UiScreen& screen);
  void buildPanel(UiScreen& screen);
  // One tab strip for both states: the toolbar shows it idle above the scrub
  // row, a panel shows the active tab filled. Same geometry, same labels, so
  // opening a panel reads as the sheet (and its header) growing upward.
  void buildToolTabs(UiScreen& screen, freeink::ui::Rect row, int activeTool);

  Model model_;
  Routed pending_;
  freeink::ui::ListNav nav_;

  // Row window materialised for the visible page only (a contents list runs to
  // hundreds of entries; labels are copied here so ListItem can point at them).
  static constexpr int kMaxWindow = 16;
  std::string windowLabels_[kMaxWindow];
  std::string windowValues_[kMaxWindow];
  freeink::ui::ListItem windowItems_[kMaxWindow];
  // fui::ButtonProps / ListProps / HeaderProps embed a 324-byte StyleSet: keep
  // them off the stack (AGENTS.md: locals stay under 256 bytes).
  freeink::ui::ButtonProps stepProps_;
  freeink::ui::ListProps listProps_;
  int16_t sheetTop_ = 0;
};
