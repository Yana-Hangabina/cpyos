#pragma once

#include <Epub.h>
#include <Epub/AnnotationStore.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/JjwxcCommentSidecar.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>
#include <VectorFontSupport.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "ChapterPosition.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderFontPreview.h"
#include "ReaderPageCache.h"
#include "ReaderToolbarUi.h"
#include "components/OptionPopup.h"
#ifdef BOARD_HAS_PSRAM
#include "activities/apps/jjwxc/JjwxcPsramAllocator.h"
#endif

class TtfEpdFont;

class EpubReaderActivity final : public ReaderActivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  uint8_t pageTurnRate = 15;
  // Net-delta turn accumulator: taps queued while a render holds the lock (or
  // inside the minimum turn gap) collapse into one jump to the final page —
  // intermediate pages are never rendered. Bounded so a long render cannot
  // accumulate an unbounded skip. Netting also fixes the old two-deep queue's
  // habit of dropping the middle direction on a fast double tap.
  int8_t pendingTurnDelta = 0;
  static constexpr int8_t kMaxCoalescedTurns = 20;
  void queueManualTurn(int8_t direction) {
    const int next = pendingTurnDelta + direction;
    pendingTurnDelta = static_cast<int8_t>(next > kMaxCoalescedTurns   ? kMaxCoalescedTurns
                                           : next < -kMaxCoalescedTurns ? -kMaxCoalescedTurns
                                                                        : next);
  }
  void clearPendingTurns() { pendingTurnDelta = 0; }
  // 水波纹翻页请求：pageTurn() 记录方向（±1），renderContents() 在提交前交给显示层
  // 并清零；非顺序翻页（章节跳转、重排、图片页、清屏周期）不消费它。
  // / Water-ripple turn request: pageTurn() records the direction (±1);
  // renderContents() hands it to the display layer right before the commit and
  // clears it. Non-sequential renders never consume it.
  int8_t waterTurnDirection = 0;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool pendingSyncLaunchError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool currentPageBookmarked = false;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;

#if FREEINK_DEVICE_READPICO
  // Two slots of four reusable PSRAM frames each (~812 KiB total); too large for
  // the render task's stack. The activity owns them and releases them in onExit().
  //
  // Two slots because the cache only pays off for the page the reader actually turns
  // to, and a reader turns both ways. With one slot the build could only ever cover
  // currentPage + 1, so every backward turn was a guaranteed miss -- measured: a
  // session that alternated directions hit once in eight turns, while one that mostly
  // advanced hit four in six. Slot 0 is the forward page (+1), slot 1 the backward
  // one (-1); a human reading pace leaves room to build both (each takes ~700 ms).
  static constexpr int kPageCacheSlots = 2;
  memory::ByteBuffer pageCacheBase_[kPageCacheSlots];
  memory::ByteBuffer pageCacheLsb_[kPageCacheSlots];
  memory::ByteBuffer pageCacheMsb_[kPageCacheSlots];
  memory::ByteBuffer pageCacheStash_[kPageCacheSlots];
  ReaderPageCache pageCache_[kPageCacheSlots];
  // Which slot the hit test matched, so the three plane copies in renderContents()
  // read the same slot the test looked at rather than assuming slot 0.
  int pageCacheLiveSlot_ = 0;
  ReaderPageCacheKey renderedPageKey_;
  uint32_t sectionGeneration_ = 0;
  uint32_t renderEpoch_ = 0;
  bool pageCacheFailed_ = false;
#ifdef ENABLE_CHINESE_VERSION
  uint32_t pageCacheMissingCodepoint_[kPageCacheSlots] = {};
#endif

  bool pageCacheEligible() const;
  ReaderPageCacheKey pageCacheKey(int page, int top, int right, int bottom, int left) const;
  uint32_t idleRenderDelayMs() const override;
  void renderIdle(uint32_t generation) override;
  // Build the forward slot at the end of renderContents: the idle pass it
  // replaced ran too late and was cancelled by the next tap in most turns.
  void prebuildForwardSlot();
  // Build one page into one cache slot; only called from renderIdle().
  bool buildPageCacheSlot(int slot, const ReaderPageCacheKey& key, uint32_t generation);
  void freePageCache();

  // A2: per-minute status-bar clock refresh. When the reader sits idle with a
  // status-bar clock showing, the loop repaints only the status strip into the
  // framebuffer and pushes a windowed differential refresh; only the changed
  // clock pixels are driven, so the body text is never re-drawn. lastClockMinute_
  // is the minute the panel currently shows (set wherever the status bar is
  // painted), so the tick fires once per minute boundary.
  mutable int64_t lastClockMinute_ = -1;
  memory::ByteBuffer clockScratch_;
  void refreshStatusBarClockIfDue();
#endif

  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

#ifdef ENABLE_CHINESE_VERSION
  std::atomic<uint32_t> pendingMissingChineseCodepoint_{0};
  char wereadBookId_[64] = {};
  bool clearInitialProgressAfterSave_ = false;
  bool maybeOfferCompleteChineseFont();
#endif

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, Bookmarks, More };
  Overlay overlay = Overlay::None;
  // One-shot first-run map of the reader's touch zones. It is a full-screen
  // page of its own (refreshed UI frame 16), not a wash over the body text; it owns
  // input until dismissed once.
  bool gestureHintVisible = false;
  // A tap that opened the book can still be mid-release when the first frame
  // lands; ignore input for a moment so the page is not dismissed unseen.
  uint32_t gestureHintShownAt = 0;
  // renderContents() runs on the PSRAM-stack render task and must not write
  // internal flash; it marks the hint seen and leaves the NVS write to loop()/
  // onExit() on the UI task.
  std::atomic<bool> hintPersistPending_{false};
  Rect gestureHintStartRect_{};
  Rect gestureHintSettingsRect_{};
  void drawGestureHint();
  // Reader chrome (refreshed UI frame 04): a two-line header at the top, a wave
  // progress band and the page numbers at the foot. One geometry helper feeds
  // both the layout reserve in renderBook() and the per-minute clock repaint,
  // so the three cannot drift apart.
  struct ReaderChromeMetrics {
    int left = 0;
    int right = 0;
    int top = 0;
    int lineH = 0;
    int titleY = 0;
    int metaY = 0;
    int ruleY = 0;
    int headerBottom = 0;
    int waveY = 0;
    int waveH = 0;
    int pageY = 0;
    int pageH = 0;
    int footerTop = 0;
    int footerHeight = 0;
  };
  ReaderChromeMetrics readerChromeMetrics() const;
  // The content top used by renderContents()/drawSelection(): viewable top plus
  // screenMargin, pushed below the reader chrome header. Word hit-testing must
  // use this same value or a long-press selects the line below the finger.
  int selectionMarginTop() const;
  void drawReaderHeader() const;
  void drawReaderFooter() const;
  // Footnote preview card (B10): tapping a single footnote marker shows the note
  // in a card over the page instead of jumping away. Drawn into the same frame
  // as the page, so showing it costs no extra panel refresh; one tap on the
  // footer still jumps to the real footnote page.
  bool footnoteCardActive = false;
  bool footnoteCardHasPage = false;
  std::string footnoteCardText;
  std::string footnoteCardHref;
  int16_t footnoteCardX = 0;
  int16_t footnoteCardY = 0;
  int16_t footnoteCardW = 0;
  int16_t footnoteCardH = 0;
  int16_t footnoteCardButtonY = 0;
  void openFootnoteCard(const char* href);
  void closeFootnoteCard();
  void drawFootnoteCard();
  bool handleFootnoteCardInput();
  // Two-finger pinch → next/previous font size step. Runs from the reading
  // surface only (never while a sheet is open).
  bool handlePinchGesture();

  // --- Annotations (highlights / underlined notes), B9 ---
  // Loaded once per book from <cache>/annotations.bin; painted as a 1-bit
  // underline under the annotated words plus a small speech-bubble button at the
  // end of a note's range. Both are drawn into the page frame, so they ride
  // whichever refresh path the page takes (DU / GL16 / 16-level).
  std::vector<annotations::Annotation> annotations_;
  bool annotationsLoaded = false;
  struct AnnotationBubbleHit {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    size_t index;
  };
  std::vector<AnnotationBubbleHit> annotationBubbles_;
  bool annotationCardActive = false;
  size_t annotationCardIndex = 0;
  std::string annotationCardTitle;
  std::string annotationCardBody;
  int16_t annotationCardX = 0;
  int16_t annotationCardY = 0;
  int16_t annotationCardW = 0;
  int16_t annotationCardH = 0;
  int16_t annotationCardButtonY = 0;
  int16_t annotationCardButtonX = 0;
  int16_t annotationCardButtonW = 0;
  int16_t annotationCardDeleteX = 0;
  int16_t annotationCardDeleteW = 0;
  // >= 0 when the open selection is an existing annotation (tapping its text
  // re-selects the whole range and turns the first action into Delete).
  int selectionEditingIndex = -1;
  void loadAnnotations();
  void saveAnnotations();
  void drawAnnotations(const Page& page, int fontId, int marginLeft, int marginTop);
  void showAnnotationCard(size_t index);
  void closeAnnotationCard();
  void drawAnnotationCard();
  bool handleAnnotationCardInput();
  bool handleAnnotationBubbleTap();
  int hitTestAnnotationBubble(int x, int y) const;

  // --- Selection for annotations (long press on the page) ---
  // Word-granular: e-ink has no drag handles, so the side keys extend the range
  // one word at a time (CJK is one character per word, which reads naturally)
  // and a second long press on another word moves the focus end.
  struct AnnoWordRect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint16_t line;
    uint16_t word;
    uint32_t start;
    uint32_t end;
    const char* text;
  };
  bool selectionActive = false;
  // Self-timed long press + drag selection. The SDK's long-press event is gated
  // on flags a previous pinch can leave set (touchMultiContactSequence), which
  // made it silently dead in the reading surface; timing it here from the live
  // contact also gives us the drag-to-extend gesture for free.
  bool pressTracking = false;
  uint32_t pressStartMs = 0;
  int16_t pressStartX = 0;
  int16_t pressStartY = 0;
  int16_t pressLastX = 0;
  int16_t pressLastY = 0;
  uint32_t pressLastHeldMs = 0;
  bool selectionDragging = false;
  int selectionDragHandle = 0;  // 0 = none, 1 = start handle, 2 = end handle
  uint32_t selectionLastRedrawMs = 0;
  uint32_t selectionMenuOpenedMs = 0;
  uint32_t selectionClosedMs = 0;
  // Transient "N pt" badge after a pinch, so the change is visible on a panel
  // that has no other feedback for it.
  uint8_t fontStepHintPointSize = 0;
  uint32_t fontStepHintUntilMs = 0;
  bool handleSelectionTouch();
  bool handleSelectionHandleTouch();
  int wordIndexAt(const std::vector<AnnoWordRect>& words, int x, int y) const;
  void updateSelectionDragTo(int x, int y);
  // Handle rectangles drawn by drawSelection (0 = start, 1 = end).
  int16_t selectionHandleX[2] = {0, 0};
  int16_t selectionHandleY[2] = {0, 0};
  int16_t selectionHandleW[2] = {0, 0};
  int16_t selectionHandleH[2] = {0, 0};
  // Selection bounds in page coordinates, so the action bar can sit next to the
  // highlighted text instead of guessing at screen height (the reader draws in
  // rotated page coordinates, which made a bottom-anchored bar land off-screen).
  int16_t selectionFirstTopY = 0;
  int16_t selectionLastBottomY = 0;
  // Column geometry of the selection, so the vertical action bar can sit beside it.
  int16_t selectionFirstX = 0;
  int16_t selectionLastX = 0;
  int16_t selectionLastW = 0;
  bool selectionMenuDrawLogged = false;
  std::unique_ptr<Page> loadPageForSelection();
  void promptEditAnnotation(size_t index);
  // Selection action bar (highlight / note). Drawn and handled here, not through
  // the shared popup: that one routes its input through the overlay path, which
  // never runs on the reading surface — the menu could be shown but not answered.
  bool selectionMenuActive = false;
  int selectionMenuChoice = 0;  // 0 = highlight, 1 = note
  int16_t selectionMenuHitX[2] = {0, 0};
  int16_t selectionMenuHitY[2] = {0, 0};
  int16_t selectionMenuHitW[2] = {0, 0};
  int16_t selectionMenuHitH[2] = {0, 0};
  void drawSelectionMenu();
  void activateSelectionMenuChoice(int choice);
  void extendSelectionTo(const AnnoWordRect& word);
  uint16_t selectionStartLine = 0;
  uint16_t selectionStartWord = 0;
  uint16_t selectionEndLine = 0;
  uint16_t selectionEndWord = 0;
  int selectionAnchorX = 0;  // keeps the column when moving between lines
  std::vector<AnnoWordRect> layoutPageWords(const Page& page, int fontId, int marginLeft, int marginTop);
  void beginSelection(int x, int y);
  void endSelection();
  bool selectionCovers(uint16_t line, uint16_t word) const;
  void drawSelection(const Page& page, int fontId, int marginLeft, int marginTop);
  bool handleSelectionInput();
  void showSelectionMenu();
  void commitSelection(bool asNote);
  // Bookmarks panel: false = bookmark list, true = annotation list (chips).
  bool bookmarksPanelAnnotations = false;
  void promptSelectionNote(const annotations::Annotation& base);
  void exportAnnotations();
  int focusedTool = 0;  // toolbar tool focus: 0=Contents, 1=Text, 2=More
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // D5 steppers: the Text panel's Left/Right step the focused value while
  // held (press fires immediately, then repeats at this cadence).
  static constexpr uint32_t kTextStepRepeatMs = 160;
  uint32_t lastTextStepMs = 0;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font size / line spacing / alignment / orientation /
  // auto page turn. Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup overlayPopup{true};
  ReaderFontPreview fontPreview;
  enum class FontPromptState { Idle, Asking, Accepted, TooLarge };
  FontPromptState fontPromptState = FontPromptState::Idle;
  unsigned long fontNoticeStartedAt = 0;
  bool fontPromptWaitForBackRelease = false;
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  // True while a deferred overlay chrome refresh (pushOverlayRefresh) may still
  // be running on the panel. settleOverlayRefresh() must run before the
  // framebuffer is touched or another differential refresh is pushed.
  bool overlayRefreshPending = false;
  void pushOverlayRefresh();
  void settleOverlayRefresh();
  // The More panel's image-scaling row is a one-tap toggle that deliberately
  // does NOT re-render the page: the overlay page snapshot was decoded with the
  // old filter, so it is dropped on close and the page re-decodes once there.
  bool imageScalingDirty = false;
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;

  // Footnote support
  FootnoteList currentPageFootnotes;
  PageLinkList currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
    // Fixed-size return stack; offsets survive session CSS fallback without heap storage.
    std::optional<uint32_t> visibleTextOffset;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;
  // The back-stack outlives the reader (sleep, home) in links.bin so Back
  // still returns to where a followed link was tapped.
  void saveLinkStack() const;
  void loadLinkStack();

  // A5: an EPUB-embedded font. When the book declares one in its OPF manifest and
  // the user has not chosen an SD font, it is read out of the package into PSRAM,
  // registered with the renderer, and applied book-wide through
  // SETTINGS.readerFontOverrideId. The face and its bytes live for the reading
  // session and are released on exit (see releaseEmbeddedFont).
  // Declared unconditionally (they are no-ops without vector fonts); the state they
  // manage needs a complete TtfEpdFont at destruction, so only the members are gated.
  void loadEmbeddedFontIfPresent();
  void releaseEmbeddedFont();
#if CROSSPOINT_VECTOR_FONTS
  // Raw pointer, not unique_ptr: the owning TU (EpubReaderActivity.cpp) is the
  // only one with a complete TtfEpdFont, and unique_ptr would demand completeness
  // from every TU that destroys the activity. Freed in releaseEmbeddedFont().
  TtfEpdFont* embeddedFont_ = nullptr;
  memory::ByteBuffer embeddedFontBytes_;
  int embeddedFontId_ = 0;
#endif

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;

  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Requires the render lock; heap admission is checked separately by the build tick.
  bool backgroundBuildWanted() const;
  bool buildTickHeapGate();
  void prepareChapterBuild();
  bool needsPartialRebuild() const;
  bool updateChapterBuild();
  bool stylesDisabledForSession_ = false;
  int renderedSpineIndex_ = -1;
  int failedBuildSpine_ = -1;

  // Bumped every time the JJWXC comment sidecar is (re)loaded. Part of the page
  // cache key: a slot holds finished pixels, bubbles included, and must not be
  // reused after the counts it was built from changed.
  uint32_t commentGeneration_ = 0;

#ifdef BOARD_HAS_PSRAM
  // Paragraph-comment bubbles on a JJWXC EPUB. The counts come from the
  // device-local sidecar (.crosspoint/jjwxc/<novelId>/comments/<chapterId>.bin),
  // so this works offline; nothing here touches the network.
  //
  // A page shows at most one bubble per paragraph, and only on that paragraph's
  // last line *on this page* (a paragraph continuing over the page break gets
  // its bubble at the bottom edge, which is where a reader looks for it).
  struct CommentBubble {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    uint16_t paragraphIndex = 0;
    uint16_t total = 0;
    uint32_t paragraphId = 0;
    // 0 = filled pill with the count, 1 = count only (no pill), 2 = marker.
    // The last two are only used when the final line leaves too little room to
    // the screen edge for the full pill; every style still starts after the last
    // glyph, so none of them can cover text.
    uint8_t style = 0;
  };
  // Worst page measured on a 1216x684 panel is ~30 short lines; 48 bounds the
  // hit array (48 * 16 = 768 bytes) without ever truncating a real page.
  static constexpr int kMaxCommentBubbles = 48;
  CommentBubble commentBubbles_[kMaxCommentBubbles];
  int commentBubbleCount_ = 0;

  char jjwxcNovelId_[16] = {};
  bool jjwxcBookResolved_ = false;  // the "/JJWXC/<title>-<id>.epub" test has run
  bool jjwxcBook_ = false;
  int jjwxcCommentSpine_ = -1;  // spine the counts below belong to
  uint32_t jjwxcChapterId_ = 0;

  // Per-chapter toc state for the toolbar Contents panel. 2 bytes per chapter
  // and read once per open/update; drawing a chip must not touch the SD card.
  struct JjwxcTocMeta {
    uint32_t chapterId = 0;
    uint8_t state = 0;
    uint8_t isVip = 0;
  };
  std::vector<JjwxcTocMeta, jjwxc::PsramAllocator<JjwxcTocMeta>> jjwxcTocMeta_;
  void loadJjwxcTocMeta();
  // Placeholder chapters (missing / unpurchased) get one native action button
  // at the bottom of the page: cache for a free chapter, purchase for a VIP
  // one. Drawn in every framebuffer pass; the rectangle is the tap target.
  void drawJjwxcPlaceholderAction(int fontId, int bottomMargin);
  // "N more chapters · continue" on the LAST page of the last packaged chapter.
  // This is where a serialised book actually ends for the reader, and it is the
  // one place where "the next chapter is not here" is a fact rather than a
  // guess: the update snapshot says how far behind the card is, and the action
  // is the existing UpdateChapterList deep link.
  void drawJjwxcMoreChapters(int bottomMargin);
  Rect jjwxcMoreRect_{};
  bool jjwxcMoreActive_ = false;
  uint32_t jjwxcMoreChapters_ = 0;
  Rect jjwxcActionRect_{};
  // Bottom-right? no: top-right corner bookmark target, in physical screen coords.
  Rect bookmarkCornerRect_{};
  uint32_t jjwxcActionChapterId_ = 0;
  int jjwxcActionOrdinal_ = -1;
  bool jjwxcActionPurchase_ = false;
  bool jjwxcActionActive_ = false;
  void openContentsPanel(int focusTocIndex);
  // PSRAM: up to 512 six-byte entries (3 KB) plus growth; internal RAM has no
  // room for the app's bookkeeping and the reader's page cache both.
  std::vector<JjwxcCommentSidecar::Count, jjwxc::PsramAllocator<JjwxcCommentSidecar::Count>> commentCounts_;

  void resolveJjwxcBook();
  void loadJjwxcCommentCounts();
  uint16_t commentTotalFor(uint32_t paragraphId) const;
  // Fills `out` with this page's bubbles. Returns the count. `top/right/bottom/
  // left` are the same oriented margins renderContents() passes to Page::render.
  int layoutCommentBubbles(const Page& page, int fontId, int top, int right, int bottom, int left, CommentBubble* out,
                           int maxOut) const;
  void drawCommentBubbles() const;
  void drawCommentBubbleList(const CommentBubble* bubbles, int count) const;
  void openCommentBubble(const CommentBubble& bubble);
  // Opens the JJWXC app as a child, deep-linked to this book and to one of its
  // two maintenance actions, then drops the derived section caches on return.
  void openJjwxcUpdate(bool currentChapter, bool returnToContents = false);
  // More -> book details: open the JJWXC app's BookDetail as a child and stay
  // there until Back, then reopen the (possibly repackaged) EPUB here.
  void openJjwxcBookDetail();
  // Contents tail chip: cache/refresh or purchase/acquire one chapter, then
  // reopen the same panel on return. The app detects "already bought" itself.
  void openJjwxcChapterAction(uint32_t chapterId, bool purchase, int tocIndex);
  void activateJjwxcChapterChip(int tocIndex);
  // Saves the reading position and releases the section/page cache before a
  // network child activity; shared by both entry points above.
  void prepareForJjwxcChild();
  int pendingContentsFocus_ = -1;
  // The app may have replaced the EPUB while this reader kept the old Epub
  // object (whose decryptor was opened against the plaintext file). Reopen the
  // file so openProtection() sees the new manifest and key, then restore the
  // current spine/page offset. Runs under the caller's render lock.
  bool reopenBookAfterExternalChange();
#endif
  // Contents filter (JJWXC only): 0 all, 1 cached, 2 not cached. contentsRows_
  // maps a VISIBLE row back to its toc index; nothing may use the row index as
  // a chapter index while a filter is active.
  int contentsFilter_ = 0;
  std::vector<int16_t> contentsRows_;
  void rebuildContentsRows();
  int contentsTocForRow(int row) const;
  int contentsRowForToc(int toc) const;
  bool contentsFilterable() const;

  ReaderRenderSpec effectiveRenderSpec(uint16_t width, uint16_t height) const;
  bool handleBuildFailure(const char* stage, Section::BuildError error);
  bool buildHeapPaused = false;
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  static constexpr size_t RENDER_MIN_MAX_ALLOC = 24 * 1024;
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  bool jumpToFraction(float fraction);
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Jump to a progress point read from a bookmark (the activity path and the
  // in-sheet bookmarks panel share this body).
  void applyProgressChange(const ProgressChangeResult& sync);
  // Live section position, or the values cached before a child screen
  // released the section.
  ChapterPosition chapterPosition() const;
  int bookPercentFor(const ChapterPosition& position) const;
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  int currentTocIndex() const;
  std::string currentChapterTitle() const;
  // Text panel cells (refreshed UI frame 05, strict 2x4 grid): font, size, line
  // spacing, paragraph spacing, page margin, fast page turn, water ripple,
  // restore defaults. Every value here is backed by an existing setting.
  static constexpr int kTextPanelCells = 8;
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // D5: step a Text-panel value in place (font sizes / line spacing /
  // paragraph spacing / page margin), applying live -- the book behind the
  // sheet is the preview. Cell 0 (font family) is a list, not a value.
  void stepTextRow(int row, int delta);
  // Switch cells (fast page turn, water ripple) and the restore-defaults cell.
  void toggleReaderSwitch(int cell);
  void restoreReaderDefaults();
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  void finishFontPreview();
  void handleFontPreloadPrompt();

  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  void openFootnoteSelect(bool reopenMenuOnCancel);
  void openDictionaryWordSelect();
  bool launchKOReaderSync();
#ifdef ENABLE_CHINESE_VERSION
  bool launchWeReadSync();
#endif
  unsigned long confirmLongPressThreshold() const;
  void toggleAutoPageTurn(uint8_t requestedPageTurnRate);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();
  // Index in cachedBookmarks of the entry for the page on screen, or -1.
  int currentBookmarkIndex() const;
  // Paints the page-corner bookmark glyph and records the tap target.
  void drawBookmarkCorner(int top, int rightMargin);

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

  // Modal shown when a protected book refuses to open (loan expired /
  // date unverified); OK exits, "Sync time" (when offered) verifies the
  // clock over Wi-Fi and reopens the book.
  OptionPopup loadFailurePopup;
  // Protection error captured by loadBook() for handleLoadFailure(); the
  // failed Epub itself does not outlive loadBook().
  std::string loadProtectionError;
  bool legacyProgressPending = false;

  bool loadBook() override;
  bool handleLoadFailure() override;
  // Wi-Fi join + SNTP for a loan whose date could not be verified, then a
  // clean re-open of the book.
  void beginLoanTimeSync();
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  int getProgressBasisPoints() const override;
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

  // Debug-only (serial CMD:PANEL): opens a reader sheet without a tap.
  void debugOpenPanel(int tool) override;
  void debugCloseOverlay() override;
  void debugAreaTest(int iterations) override;
  // Serial soak hook: moves the open sheet's row cursor like Up/Down would.
  void debugMoveRing(int delta) override;
  // A sheet is a 1-bit overlay on an already-pushed page: row moves change only
  // the cursor rows, so band updates apply while one is open. Page turns (no
  // sheet) keep full-page pushes, and so do the AA base/gray pairs.
  bool wantsBandUpdates() const override { return overlay != Overlay::None; }
  void debugSetContentsFilter(int filter) override;
  void debugActivateContentsRow(int row) override;
  void debugDismissHint() override;
  void debugOpenJjwxcComment() override;
  void debugOpenFakeJjwxcComment() override;
  void debugPageTurn(bool forward) override;
  void debugDumpPage() override;
  void debugOpenStressComment() override;

  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  bool deferBluetoothStart() const override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
