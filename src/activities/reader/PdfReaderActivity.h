#pragma once

#if defined(CROSSPOINT_MUPDF)

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "EpubReaderMenuActivity.h"

#include "ReaderActivity.h"
#include "components/Toast.h"
#include "PdfReaderUi.h"
#include "ReaderToolbarUi.h"
#include "Epub/AnnotationStore.h"
#include "SelectionActionBar.h"
#include "SelectionGeometry.h"
#include "SelectionHold.h"
#if defined(CROSSPOINT_PDF_XIP)
#include "pdf/PdfDocumentXip.h"
#else
#include "pdf/PdfDocument.h"
#endif

class PdfReaderActivity final : public ReaderActivity {
  static constexpr size_t kMaxCachedPages = 16;

  std::unique_ptr<PdfDocument> pdf;
  std::string cachePath_;
  std::deque<int> cachedPages_;
  bool fullCacheComplete_ = false;
  bool fullCacheActive_ = false;
  int currentPage = 0;
  // Row the classic list menu reopens on; owned here so it survives the menu.
  EpubReaderMenuActivity::MenuAction selectedMenuAction = EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN;
  std::vector<int> bookmarks_;
  std::vector<annotations::Annotation> pageNotes_;

  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  bool toolbarOpen = false;
  bool toolbarPanel = false;
  int focusedTool = 0;
  // Clean page snapshot under the toolbar. Without it every panel key stroke
  // re-rendered the PDF page and pushed twice, which is the flicker/lag the
  // EPUB toolbar avoids with the same snapshot pattern.
  bool toolbarPageStored_ = false;
  int toolbarStoredPage_ = -1;
  void discardToolbarPage();
  void openTextSettings();
  void invalidatePageCache();

  // Long-press word selection on reflowed PDF pages. The interaction model is
  // the EPUB reader's: a (line, word) range over word rectangles, two draggable
  // endpoint handles, and a Lookup/Highlight/Note action bar. The geometry and
  // drag rules come from SelectionGeometry.h so both readers behave alike; the
  // only PDF-specific piece is that the word rectangles are produced by the
  // reflow layout itself (layoutReflowWords), so what you select is exactly
  // what was drawn.
  struct ReflowWord {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    uint16_t line = 0;
    uint16_t word = 0;
    uint32_t start = 0;  // byte offset into reflowLines_[line]
    uint32_t end = 0;
    bool gapBefore = false;  // a space or line break preceded this word
  };
  std::vector<ReflowWord> reflowWords_;  // current screen only
  bool selectionMenuActive_ = false;
  int selectionMenuChoice_ = 0;
  // >= 0 while the bar is editing an existing annotation: the first chip then
  // means Delete, exactly as in the EPUB reader, and the chip count drops to 2.
  int selectionEditingIndex_ = -1;
  selection_bar::Hits selectionBars_{};
  // The press-and-hold gesture itself is the shared tracker (SelectionHold.h), so
  // the PDF reader cannot drift from the EPUB reader's timing again.
  selection_hold::Tracker selectionHold_;
  bool selectionActive_ = false;
  selection_geom::Range selectionRange_{};
  uint16_t selectionAnchorLine_ = 0;
  uint16_t selectionAnchorWord_ = 0;
  // Column the vertical key step tries to keep (shared rule: selection_geom::stepWord).
  int16_t selectionAnchorX_ = 0;
  // When the action bar came up, for the shared opening-press window (kBarOpenSwallowMs).
  uint32_t selectionMenuOpenedMs_ = 0;
  bool selectionDragging_ = false;
  int selectionDragHandle_ = 0;  // 0 none, 1 start, 2 end
  // Where the handle was grabbed. A press that does not move must not snap the
  // range to whatever word happens to be nearest the handle's centre.
  int16_t selectionDragGrabX_ = 0;
  int16_t selectionDragGrabY_ = 0;
  uint32_t selectionLastRedrawMs_ = 0;
  uint32_t selectionClosedMs_ = 0;
  // The tap that opens a book can still be reported as a held contact after the
  // (multi-second) load finishes. Feeding that to the hold tracker selected the
  // first word under the finger the moment the page appeared. This drops the
  // leftover contact until it is released, and only once per entry.
  bool selectionDropTouch_ = false;
  uint32_t selectionDropSinceMs_ = 0;
  // A press only starts a selection when the reader itself saw its touch-down.
  // The Read Pico panel can report a finger as held long after the tap that
  // opened the book (its release is not delivered across the multi-second load),
  // and that ghost has no down edge in this activity.
  bool selectionTouchArmed_ = false;
  int16_t selectionHandleX_[2] = {0, 0};
  int16_t selectionHandleY_[2] = {0, 0};
  int16_t selectionHandleW_[2] = {0, 0};
  int16_t selectionHandleH_[2] = {0, 0};
  // Clean 1-bit page under the selection overlay (PSRAM).  Valid only for the
  // page/screen it was captured from; any page turn or reflow rebuild drops it.
  uint8_t* selectionBase_ = nullptr;
  uint32_t selectionBaseBytes_ = 0;
  bool selectionBaseValid_ = false;
  int selectionBasePage_ = -1;
  int selectionBaseScreen_ = -1;
  bool selectionFramePainted_ = false;
  // One bench line per selection: the handle rectangles are only knowable after
  // the page that drew them, and a serial test needs their coordinates.
  bool selectionHandlesLogged_ = false;
  // Action bar anchor: the first/last covered word's box, in page coordinates.
  int16_t selectionFirstTopY_ = 0;
  int16_t selectionLastBottomY_ = 0;
  int16_t selectionFirstX_ = 0;
  int16_t selectionLastX_ = 0;
  int16_t selectionLastW_ = 0;
  bool handleSelectionTouch();
  bool handleSelectionHandles();
  void beginSelection(int x, int y);
  // Widen by one word with the keys, leaving the anchored end fixed (EPUB parity).
  void widenSelectionTo(uint16_t line, uint16_t word, int centerX);
  void updateSelectionDragTo(int x, int y);
  void clearSelection();
  void showSelectionMenu();
  void activateSelectionChoice(int choice);
  std::string selectionQuote(size_t* anchorPrefixBytesOut = nullptr) const;
  void commitSelection(bool asNote);
  void promptSelectionNote();
  void startPhraseLookup();
  void debugOpenPanel(int tool) override;
  void debugMoveRing(int direction) override;
  void debugConfirm() override;
  // CMD:BACK while the end-of-book notice is up performs exactly the menu's
  // short-Back action (return to the last page). Outside the notice it does
  // nothing.
  void debugBack() override;

 public:
  // CMD:PDFSEL <x> <y> bench hook: runs the shared selection geometry on the
  // real reflow words and logs the resolved range + captured quote, so the
  // word-range/quote path is verifiable from the serial console without a
  // finger.  Selection/drag visuals themselves still need on-screen check.
  // dragHandle 1 = start handle, 2 = end handle: both endpoint paths (and the
  // shared clamp that keeps the start from crossing the end) are drivable without a
  // finger, not just the end one.
  void debugSelectionAt(int x, int y, int dragX = -1, int dragY = -1, int dragHandle = 2);
  // CMD:PDFSELACTION <chip>: drive one chip of the selection action bar.  The
  // bar was otherwise pressable only with a finger, which left the highlight
  // create / re-select / delete round trip -- the thing that proves the EPUB
  // parity editing state -- unverifiable in a bench run.
  void debugSelectionAction(int choice);
  /// The 智能合并断行 option, in one place: menu row 6 and the serial A/B hook both call
  /// this, so a bench run exercises the same path a finger does.
  void applySmartLineMerge(bool on);
  /// One line that makes the app<->engine pairing observable on the device (CMD:PDFENGINE).
  void debugEngineStatus();
  /// Show the render-failure card deliberately (CMD:PDFFAILDEMO).  The card is
  /// the only state a genuinely unrenderable page can reach, and waiting for a
  /// real one means waiting for a memory shape that may not occur on demand.
  void debugShowRenderFailed() {
    renderFailed_ = true;
    requestUpdate();
  }

 private:

  // Reflow path: text-only pages are laid out with the reader font instead of
  // being rasterised, which is what makes PDF text crisp on a 674-px panel.
  bool reflowMode_ = false;
  int reflowPage_ = -1;
  int reflowScreen_ = 0;
  int reflowScreens_ = 0;
  std::vector<std::string> reflowLines_;
  std::vector<int16_t> reflowLineIndent_;
  std::vector<int16_t> reflowLineExtra_;
  std::vector<uint8_t> reflowLineParaEnd_;
  std::vector<int> reflowLineFont_;
  std::vector<int> reflowScreenStarts_;
  // Returning from the end-of-book notice must land on the screen the reader
  // left (the last one), not the top of the page.
  bool reflowEndScreenPending_ = false;

  void buildReflow(int usableWidth, int pageHeight, bool allowMixed = false);
  bool renderReflow(int usableWidth, int pageTop, int marginLeft, bool showStatus);
  void resetReflow();
  // Selection is an overlay on an already-drawn reflow page.  Redrawing the whole
  // page to show it took ~6 s on Read Pico (one panel turn looked like the long
  // press had not registered); keep a PSRAM copy of the clean page and repaint
  // only the highlight/handles/bar over it.
  void ensureSelectionBase();
  void captureSelectionBase();
  void drawReflowSelection(int lineFont, int lineHeight);
  void invalidateSelectionBase();

  void renderPage();
  // One place that paints the "cannot display this page" card and its two
  // actions.  It used to live inline in the failure branch, so the bench hook
  // that sets renderFailed_ showed nothing at all (captured as an all-white
  // frame) and the card could not be photographed.
  void paintRenderFailed();
  // Tappable actions on that card.  Key hints are suppressed on this touch-only
  // device, so a card with no buttons is a card with no visible way out: the
  // captured frame showed the sentence and nothing else.
  int failRetryX_ = -1, failRetryY_ = -1, failRetryW_ = 0, failRetryH_ = 0;
  int failSkipX_ = -1, failSkipY_ = -1, failSkipW_ = 0, failSkipH_ = 0;
  // A failed page is a state, not a paint: Confirm retries it, Next/Prev skips
  // past it.  Without this the failure card was a picture the reader could not
  // act on, and the only way out was Back.
  bool renderFailed_ = false;
  // The document itself could not be opened (corrupt, unsupported, OOM).  The
  // reader stays up to say so and lets Back leave, instead of vanishing to Home
  // with only a serial line.
  bool loadFailed_ = false;
  // Consecutive failed pages. Three in a row means this file cannot be shown
  // here, and the card stops pretending "next page" will help (PdfFailStreak.h).
  int failedStreak_ = 0;
  // The page box renderPage() last used, so the prefetch pass (which runs after
  // the frame is presented) does not recompute the status-bar geometry.
  int lastUsableWidth_ = 0;
  int lastPageHeight_ = 0;
  // Last reflow build's wall clock: the next one shows the wait card when the
  // previous build was slow enough for the turn to look dead (see renderPage).
  uint32_t lastReflowMs_ = 0;
  bool failEscalated_ = false;
  // The radio is released on entry (see onEnter); this remembers that it was on
  // so the one-shot hint can say so on the first PDF of a boot.
  uint8_t wifiWasOn_ = 0;
  bool wifiHintPending_ = false;
  uint32_t wifiHintShownMs_ = 0;
  pdf_reader_ui::Input readReaderInput(const pdf_reader_ui::State& ui);
  void drawStatusBar() const;
  void saveProgress() const;
  void loadProgress();
  bool ensureCacheValid();
  std::string pageCachePath(int page) const;
  void rememberCachedPage(int page);
  bool ensureFullCache(int usableWidth, int pageHeight);
  bool fullCacheMarkerValid() const;
  void writeFullCacheMarker();
  void drawCacheProgress(int done, int total);
  void loadBookmarks();
  void loadPageNotes();
  bool savePageNotes();
  const annotations::Annotation* noteForPage(int page) const;
  void editPageNote();
  void toggleScreenHighlight();
  bool pageHasLineHighlight(int line) const;
  bool lineHighlightSpan(int line, int& startByte, int& endByte, bool* isNoteOut = nullptr) const;
  bool annotationSpanOnLine(size_t index, int line, int& startByte, int& endByte,
                              bool* isNoteOut = nullptr) const;
  // The annotation covering the given word, if any, plus the first/last word
  // indices that make up its whole range -- the PDF equivalent of the EPUB
  // reader's "long-press inside an annotation re-selects it" rule.  Index-based
  // because a selection-made PDF highlight carries no offsets at all (a PDF page
  // has no stable text coordinate across re-renders), so it could otherwise
  // never be found again to be deleted.
  int annotationCoveringWord(size_t wordIndex, int& firstWordOut, int& lastWordOut) const;
  void deleteSelectionAnnotation(size_t index);
  bool findQuoteLineRange(const std::string& quote, int& firstLine, int& lastLine) const;
  // The (start,end) line-index pair to persist for `quote`, following this file's
  // convention that PDF annotation offsets are LINE INDICES.  Falls back to the
  // page-marker range (0,1) when the quote no longer resolves -- which is what
  // `endOffset > startOffset` in annotations::normalize() also needs in order to
  // clamp the stored quote/note/title at all.
  bool annotationLineRange(const std::string& quote, uint32_t& start, uint32_t& end) const;
  bool saveBookmarks();
  void toggleBookmark(int page);
  bool isPageBookmarked(int page) const;
  // The Bookmarks panel lists bookmarks and page notes together: both are
  // anchored to a page and both are "jump there" rows.
  struct BookmarkRow {
    int page;
    bool note;
    size_t noteIndex = static_cast<size_t>(-1);
    uint8_t kind = 0;
  };
  std::vector<BookmarkRow> bookmarkRows() const;
  int currentBookmarkIndex() const;
  int currentOutlineIndex() const;
  bool usesToolbarMenu() const;
  void openToolbar();
  void closeToolbar();
  void renderToolbarOverlay();
  void handleToolbarInput();
  void activateToolbarRow(int index);
  int toolbarItemCount() const;
  void openReaderMenu();
  void onReaderMenuConfirm(int action);
  int progressPercent() const;
  bool pendingScreenshot = false;
  mutable bool grayLogged = false;

  bool loadBook() override;
  uint8_t engineProblem_ = 0;  ///< pdf_module::Refusal + 1; 0 = no engine problem
  bool engineUpdateAttempted_ = false;  ///< auto-update once per reader session
  Toast toast_;  ///< EPUB parity: declines are capsules, not blocking cards
  bool handleLoadFailure() override;
  /// Body text for the engine-refused card. Existing keys only: the EN i18n
  /// blob sits at its 15-bit offset ceiling.
  const char* engineProblemText() const;
  /// True when the refusal is one a download can fix (no engine, or an engine
  /// bound to another firmware) as opposed to this boot's memory state (MMU
  /// window reserved failed / no PSRAM heap), whose remedy is a power cycle.
  /// Sending those users to fetch an engine they already have is worse than
  /// telling them to restart.
  bool engineProblemIsDownloadable() const;
  /// Opens the in-reader engine update page and re-tries the open afterwards.
  void openEngineUpdate();
  void promptPdfPassword();
  std::string pdfPassword_;
  bool pdfPasswordRequired_ = false;
  bool pdfPasswordWrong_ = false;
  std::string getBookTitle() const override;
  // EPUB parity: write the reader's own highlights/notes to <cache>/annotations.txt.
  // The body, the file name and the date stamp all come from annotation_export, so the two
  // readers cannot drift apart.
  void exportAnnotations();
  bool handleFormatInput() override;
  void renderBook() override;
  void loop() override;
  void applyInitialOrientation() override;

 public:
  explicit PdfReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("PdfReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~PdfReaderActivity() override = default;

  /// Debug/serial-test hook: -1 = normal progress file.  Consumed once by loadBook.
  static int debugInitialPage;
  /// Reflow is behind a serial switch until the extraction and the rotation are
  /// verified on a real page: "CMD:REFLOW".
  static bool debugReflowEnabled;
  /// Serial self-test hook: seed a note on this page when the book opens (-1 off).
  static int debugSeedNotePage;
  static int debugSeedHighlightPage;
  /// Serial self-test: render every page once inside this activity (same code
  /// path as reading, one page per loop tick) and print one summary line.
  static bool debugSelfTest;
  int selfTestPage_ = 0;
  int selfTestReflow_ = 0;
  int selfTestBitmap_ = 0;
  int selfTestFailed_ = 0;
  std::string selfTestFailures_;
  void runSelfTestStep();
  /// Reflow is a per-document decision: some PDFs (subset fonts without
  /// Unicode mappings) only expose headers and page numbers, and reflowing
  /// those would drop the whole body.  Sampled once, asynchronously: the first
  /// page must appear before the probe spends seconds in MuPDF.
  bool reflowBookEnabled_ = false;
  bool reflowProbeStarted_ = false;
  bool reflowProbePending_ = false;
  bool firstPagePresented_ = false;
  void startReflowProbe();
  void pollReflowProbe();

  void onEnter() override;
  void onExit() override;
  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;
  ScreenshotInfo getScreenshotInfo() const override;
};

#endif
