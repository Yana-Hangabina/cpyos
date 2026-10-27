#pragma once

#if defined(CROSSPOINT_MUPDF)

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "ReaderActivity.h"
#include "ReaderToolbarUi.h"
#include "Epub/AnnotationStore.h"
#include "pdf/PdfDocument.h"

class PdfReaderActivity final : public ReaderActivity {
  static constexpr size_t kMaxCachedPages = 16;

  std::unique_ptr<PdfDocument> pdf;
  std::string cachePath_;
  std::deque<int> cachedPages_;
  bool fullCacheComplete_ = false;
  bool fullCacheActive_ = false;
  int currentPage = 0;
  std::vector<int> bookmarks_;
  std::vector<annotations::Annotation> pageNotes_;

  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  bool toolbarOpen = false;
  bool toolbarPanel = false;
  int focusedTool = 0;

  // Reflow path: text-only pages are laid out with the reader font instead of
  // being rasterised, which is what makes PDF text crisp on a 674-px panel.
  bool reflowMode_ = false;
  int reflowPage_ = -1;
  int reflowScreen_ = 0;
  int reflowScreens_ = 0;
  int reflowLinesPerScreen_ = 0;
  std::vector<std::string> reflowLines_;

  void buildReflow(int usableWidth, int pageHeight, bool allowMixed = false);
  bool renderReflow(int usableWidth, int pageTop, int marginLeft, bool showStatus);
  void resetReflow();

  void renderPage();
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
  bool saveBookmarks();
  void toggleBookmark(int page);
  bool isPageBookmarked(int page) const;
  // The Bookmarks panel lists bookmarks and page notes together: both are
  // anchored to a page and both are "jump there" rows.
  struct BookmarkRow {
    int page;
    bool note;
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
  std::string getBookTitle() const override;
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
  /// those would drop the whole body.  Sampled once when the book opens.
  bool reflowBookEnabled_ = false;
  void probeReflowSupport();

  void onExit() override;
  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;
  ScreenshotInfo getScreenshotInfo() const override;
};

#endif
