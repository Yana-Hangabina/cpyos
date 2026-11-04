#pragma once

#include <Arduino.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "ReaderExtractor.h"
#include "TextPager.h"
#include "WebTypes.h"
#include "WebPlaces.h"

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// A phone-shaped reading browser: fetch an HTTPS page, strip it to a linear
// reading document, and paginate it with the same fonts the reader uses.
//
// Not a general browser: no JavaScript, no images, no plain HTTP, no forms.
// Those are all deliberate. The device's TLS client only speaks HTTPS, the
// paginator has no concept of a viewport smaller than a page, and a web-sized
// DOM does not fit the surviving flash. See docs/engineering/web-browser.md.
class WebBrowserActivity final : public Activity {
 public:
  WebBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WebBrowser", renderer, mappedInput), optionPopup_(false) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == State::Loading; }
  void prepareForSleep() override;
  bool presentsGrayscale() const override { return state_ == State::Page; }

  // Open the app on this URL (serial test hook). Consumed by onEnter().
  static void requestOpen(const char* url);
  // Serial-only: open the built-in offline sample page (no network).
  static void requestFixture();

  void debugBrowserPage(int delta) override;
  void debugBrowserLinks() override;
  void debugBrowserBack() override;
  void debugBrowserFollow(int index) override;
  void debugBrowserOpen(const char* url) override;
  void debugBrowserFixture() override { loadFixtureDocument(); }
  void debugBrowserToggleBookmark() override;
  void debugBrowserBookmarks() override;
  void debugBrowserStatus() override;
  void debugBrowserMenu() override;
  void debugBrowserList(int which) override;
  void debugBrowserOpenPlace(int which, int index) override;

 private:
  enum class State : uint8_t { Home, Loading, Page, Error, Bookmarks, History };
  enum class JobResult : uint8_t { Ok, Network, Http, Empty, Oversize, Canceled, BadEncoding };

  struct LinkHit {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    uint32_t linkIndex = 0;
  };

  void openAddressBar();
  void navigateTo(const std::string& input, bool addHistory);
  void beginFetch(const std::string& url);
  void runFetch(std::string url);
  void pollJob();
  void reapJob();
  void requestCancel();
  void abandonJob(bool toPage, const char* message);
  bool trySavedNetwork();
  void paginateDocument();
  void goBack();
  void suspendTimeSync();
  void resumeTimeSync();
  void openLinkPicker();
  void gatherPageLinks(std::vector<std::string>& labels, std::vector<std::string>& urls) const;
  void followLink(const std::string& url);
  void pageTurn(int delta);
  void showNotice(const char* message);
  void openMenu();
  void enterList(State list);
  webbrowser::WebVector<webbrowser::Place>& activeList();
  const webbrowser::WebVector<webbrowser::Place>& activeList() const;
  void openPlace(const webbrowser::Place& place);
  void updateHistoryPage();
  bool currentPageBookmarked() const;
  void reloadCurrentPage();
  void goHomeState();
  void openListMenu();
  void confirmDeleteAt(int index);
  void confirmClearList();
  void renderList();
  void drawBookmarkMark(int x, int y, int width, int height);
  void loadFixtureDocument();
  void loadPlaces();
  void saveBookmarks();
  void saveHistory();
  webbrowser::Place currentPlace() const;

  void renderHome(bool withFooter = true);
  void renderLoading();
  void renderPage(const std::string* urlOverride = nullptr, bool withFooter = true);
  void renderError();
  void drawShell(const char* title, const char* subtitle, bool backButton = true);
  void drawHints(const char* back, const char* confirm, const char* previous, const char* next);
  void drawPageCounter();
  void drawListMenuPill(const Rect& safe);

  static void jobTrampoline(void* param);
  static int measureThunk(void* context, const char* text, size_t length, bool bold);
  static int spaceThunk(void* context, uint32_t left, uint32_t right, bool bold);
  int measureTextWidth(const char* utf8, size_t length, bool bold);
  int measureRange(const webbrowser::WebString& text, uint32_t start, uint32_t length, bool bold);
  int glyphWidthInternal(uint32_t cp, bool bold);
  int spaceWidthInternal(bool bold);
  int measureCodepoint(uint32_t cp, bool bold);

  State state_ = State::Home;
  webbrowser::Document document_;
  // Filled by the fetch task; swapped into document_ only after jobDone_, so a
  // cancelled or failed navigation leaves the previous page intact.
  webbrowser::Document incomingDocument_;
  webbrowser::Pagination pagination_;
  webbrowser::WebVector<webbrowser::WebString> history_;
  webbrowser::WebVector<webbrowser::Place> bookmarks_;
  webbrowser::WebVector<webbrowser::Place> visits_;
  std::string currentUrl_;
  std::string pendingUrl_;
  std::string errorText_;
  std::string notice_;
  uint32_t noticeUntilMs_ = 0;
  size_t pageIndex_ = 0;
  uint32_t loadingStartedMs_ = 0;
  uint32_t lastLoadingTickMs_ = 0;
  uint32_t loadingDeadlineMs_ = 0;
  uint32_t cancelDeadlineMs_ = 0;
  bool connectingWifi_ = false;
  bool jobAbandoned_ = false;
  // True only when this activity brought the radio up (saved-network connect or
  // the selection sheet). A connection the user already had is not ours to close.
  bool wifiSessionActive_ = false;
  bool historyDirty_ = false;
  bool bookmarksDirty_ = false;
  State listReturnState_ = State::Page;
  int listIndex_ = 0;
  int listScroll_ = 0;
  bool pendingRestore_ = false;
  uint16_t pendingRestorePage_ = 0;
  bool pendingKeepPage_ = false;
  uint16_t pendingKeepPageIndex_ = 0;
  uint16_t turnsSinceHistory_ = 0;
  std::string jobContentType_;
  Rect homeAddressRect_{};
  Rect homeBookmarksRect_{};
  Rect homeHistoryRect_{};
  std::vector<Rect> homeRecentRects_;
  std::vector<Rect> listRowRects_;
  std::vector<Rect> listTrashRects_;
  Rect loadingCancelRect_{};
  Rect pageMenuRect_{};
  Rect listMenuRect_{};
  Rect errorRetryRect_{};
  Rect errorBackRect_{};
  bool timeSyncSuspended_ = false;
  bool hasQueuedNav_ = false;
  std::string queuedNavUrl_;
  bool queuedNavAddHistory_ = false;
  // Resolved once per document: measuring a fallback glyph per character and
  // drawing the whole line with another face made lines ~20% wider than the
  // pager thought (device: widest 740 px against a 612 px column).
  int bodyFont_ = 0;
  // Glyph-width model: measuring through the renderer walks font groups (a
  // whole CJK line costs ~50 ms). ASCII and spaces are measured once, CJK uses
  // the face's single advance, and everything else is memoised.
  int cachedFont_ = 0;
  int asciiWidth_[2][95]{};
  bool asciiReady_[2][95]{};
  int wideAdvance_[2]{};
  bool wideReady_[2]{};
  int spaceWidth_[2]{};
  bool spaceReady_[2]{};
  struct GlyphMemo {
    uint32_t cp;
    int width;
  };
  static constexpr int kGlyphMemoSize = 64;
  GlyphMemo glyphMemo_[2][kGlyphMemoSize]{};
  uint8_t glyphMemoCount_[2]{};

  // Job task. The worker writes document_/jobResult_/jobError_; the UI thread
  // reads them only after jobDone_ (release/acquire).
  std::atomic<bool> jobDone_{false};
  std::atomic<int> jobPermille_{-1};
  std::atomic<uint8_t> jobStage_{0};  // 0 wifi, 1 fetching, 2 parsing
  std::atomic<uint32_t> jobBytes_{0};
  std::atomic<uint32_t> jobTotal_{0};
  std::atomic<uint32_t> jobLastByteMs_{0};
  std::atomic<bool> jobCancel_{false};
  TaskHandle_t jobTask_ = nullptr;
  bool jobStackInPsram_ = false;
  JobResult jobResult_ = JobResult::Ok;
  int jobHttpStatus_ = 0;
  std::string jobUrl_;

  OptionPopup optionPopup_;
  bool optionPopupClosing_ = false;

  bool entryFrame_ = false;
  bool waitForBackRelease_ = false;
  bool waitForConfirmRelease_ = false;

  std::vector<LinkHit> visibleLinks_;
  // Mirrors the JJWXC transport, which is the path proven to work on this
  // device: one persistent TLS session per job, a heap-resident read buffer and
  // a 45 s timeout. A fresh Session per fetch plus a 12 s timeout did not.
  uint8_t requestBuffer_[2048]{};

  static char pendingOpenUrl_[512];
  static bool pendingFixture_;

  static constexpr const char* kTag = "WEB";
};
