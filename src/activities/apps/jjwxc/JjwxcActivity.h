#pragma once

#include <Bitmap.h>
#include <JjwxcDiscover.h>
#include <JjwxcComments.h>
#include <JjwxcDeviceAuth.h>
#include <JjwxcFavorites.h>
#include <JjwxcProgress.h>
#include <JjwxcProtocol.h>
#include <JjwxcPurchase.h>
#include <JjwxcWapClient.h>
#include <JjwxcWapLogin.h>
#include <JjwxcWapShelf.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

#include "JjwxcHttpTransport.h"
#include "JjwxcPsramAllocator.h"
#include "JjwxcStore.h"
#include "JjwxcTextWrap.h"

class HalFile;

// JJWXC (晋江文学城) reader app.
//
// Flow: disclaimer -> menu -> {online shelf, search, downloaded, account}.
//
// One novel is one EPUB at one fixed path (/JJWXC/<title>-<novelId>.epub), and
// that file is rebuilt from *every* chapter currently on the card. Caching a
// single chapter, a range or the whole book therefore all converge on the same
// artefact instead of forking the library: RecentBooksStore keys on the path
// (src/RecentBooksStore.h:8-15), so the old per-chapter `preview-<novelId>-
// <chapterId>.txt` files each became their own "book", and LibraryBuilder skips
// dot-directories entirely (lib/LibraryIndex/LibraryBuilder.cpp:196), so nothing
// under /.crosspoint could reach the shelf at all. EPUB also carries the title,
// author and cover that a TXT could not (BookCoverLoader only builds thumbnails
// for EPUB/XTC).
//
// Downloads are incremental: toc.bin records a per-chapter state, and a chapter
// whose file is on the card with a matching remote size is never refetched.
// There is deliberately no WeRead-style `refreshingBook` half-logic that makes an
// existing artefact trigger a full re-pull; refreshing is an explicit menu item.
//
// Network rules that are easy to get wrong and are therefore enforced here:
//   - free endpoints use https://app-cdn.jjwxc.net and carry no token;
//   - anything carrying the token uses https://app.jjwxc.net, never HTTP;
//   - only one TLS session is held, and it is dropped before SD-heavy phases.
class JjwxcActivity final : public Activity {
 public:
  JjwxcActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Jjwxc", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

  // One-tap entry from the EPUB reader's menu. The reader pushes this activity
  // as a child, so Back returns to the open book. The request is consumed once.
  enum class DeepLinkAction : uint8_t {
    None,
    UpdateCurrentChapter,
    UpdateChapterList,
    CacheChapter,
    PurchaseChapter,
    OpenBookDetail,
    // One-shot "walk the online shelf, write the update snapshot, come straight
    // back". Started by the Library shelf's sync button; carries no novel id.
    RefreshUpdates,
    // Open the WAP search results for a query supplied by the library's search
    // page. Carries no novel id.
    Search
  };
  // novelId may be empty ONLY for RefreshUpdates; every other action needs a book.
  static void requestDeepLink(const char* novelId, DeepLinkAction action, uint32_t chapterId = 0,
                              bool openChapter = false);
  static void requestSearchDeepLink(const char* query);

  // Find-page shortcuts: land directly on the app's own surface instead of
  // making the user walk the menu that is already visible on the card.
  enum class MenuShortcut : uint8_t { None, Shelf, Discover, Search, Account };
  static void requestMenuShortcut(MenuShortcut shortcut);

  // Called by the worker-task trampoline; not part of the public surface.
  void runJobBody();
  void signalJobDone();

 private:
  enum class State : uint8_t {
    Disclaimer,
    Menu,
    Account,
    Shelf,
    Discover,
    DiscoverList,
    AdvancedSearch,
    Favorites,
    FavoritePicker,
    FavoriteManage,
    ParagraphComments,
    CommentList,
    SearchResults,
    BookDetail,
    DownloadRange,
    ChapterList,
    Downloading,
    Library,
    Message,
  };

  // Which half of the download page is on screen. Written by the worker task and
  // read by render(), so it is atomic: the chapter phase has a real denominator,
  // the packaging phase is SD-bound and shows its label instead of a fake bar.
  enum class JobPhase : uint8_t { Chapters, Packaging };

  // Why a job failed, so the message page can give actionable advice instead of
  // one generic string. Mirrors WeReadActivity::errorMessage()'s split between
  // storage, memory and network failures (WeReadActivity.cpp:1978).
  // The enumerator names avoid `Storage`: that identifier is a macro expanding to
  // HalStorage::getInstance(), so `FailKind::Storage` would not compile.
  // LoginReturned is its own kind because the message it needs is not the one
  // BadCredentials maps to. WAP renders no server-side error text -- the div is filled
  // by JavaScript, which this device does not run -- so "wrong password" would claim a
  // specific cause we cannot see, and the page is byte-identical to the login form the
  // site serves to an anonymous visitor.
  enum class FailKind : uint8_t { Protocol, StorageWrite, OutOfMemory, LoginReturned };

  // Which chapters a cache run is allowed to touch. One enum rather than two
  // booleans, because the three combinations that matter are mutually exclusive
  // and "refresh + retry blocked" would be meaningless.
  enum class CacheMode : uint8_t {
    Incremental,   // fetch what is missing; never re-ask about a known refusal
    Refresh,       // re-fetch everything in the range, stubs included
    RefreshCached, // re-fetch ONLY chapters that already have a real body
    RetryBlocked,  // re-fetch ONLY the chapters a previous run recorded as refused
  };


  // A uniform row model so one renderer and one hit-tester serve every list.
  struct Row {
    jjwxc::PsramString title;
    jjwxc::PsramString subtitle;
  };

  // Geometry of the book-detail block: cover slot, the fact lines beside it, and
  // the wrapped blurb underneath. ONE cached computation feeds the drawing and the
  // row hit-test alike (AGENTS.md rule #14: drawing and hit-testing share a
  // geometry source), which matters here because counting wrapped blurb lines means
  // measuring every code point against the font -- far too expensive to redo per
  // frame, and a mismatch would push the action rows off the panel.
  //
  // All Y values are relative to blockTop() (listTop() minus the page's own scroll
  // offset); all sizes are in panel pixels derived from the theme's safe area and
  // metrics, never from the 684x1216 panel.
  struct DetailLayout {
    int contentWidth = -1;  // the width this was computed for; a change recomputes
    int coverW = 0;         // 0 when there is no cover to draw
    int coverH = 0;
    int heroH = 0;          // title/author/tagline block above the band
    int factX = 0;          // left edge of the fact lines
    int factWidth = 0;      // their wrap width
    int bandH = 0;          // data-band height above the fact lines (0 when hidden)
    // Wrapped fact lines TO DRAW: a prefix of factLines_, cut back when the collapsed
    // page has to choose between the facts and the action rows (factCut says so).
    int factLines = 0;
    bool factCut = false;
    int blurbX = 0;
    int blurbY = 0;      // top of the blurb heading
    int blurbWidth = 0;  // wrap width of the body
    int blurbLines = 0;  // wrapped body lines drawn, the heading excluded
    // True when the body did not fit the space left above the action rows, so the
    // last drawn line gets an ellipsis instead of stopping mid-sentence.
    bool blurbCut = false;
    // Which mode this was computed in. Part of the cache key: an expanded page has no
    // row reserve and no cap, so reusing a collapsed layout for it (or the reverse)
    // would draw a different number of lines than the geometry reports.
    bool expanded = false;
    int height = 0;  // what rowTop() adds to blockTop()
  };

  void enter(State next);
  void armInputBarrier();
  void rebuildRows();
  void showMessage(std::string text, State returnTo = State::Menu, std::string heading = std::string());
  void fail(jjwxc::Status s);
  static StrId statusMessageId(jjwxc::Status s);
  static const char* stateLabel(uint8_t rawState);

  // Job runner. `openAfter` marks the job as one that hands the finished EPUB
  // straight to the reader; it is a launch parameter rather than a field the
  // caller sets beforehand so a withNetwork() that never reaches the launch
  // cannot leave a stale flag behind for the next, unrelated job to act on.
  bool startJob(std::function<void()> body, const char* busyText, bool openAfter = false);
  // startJob() plus the failure path every caller needs: when the worker task
  // cannot be created, jobRunning_ stays false and pollJob() returns at once, so
  // a caller that ignored the result would leave the user on a busy page that
  // never clears.
  bool launchJob(std::function<void()> body, const char* busyText, bool openAfter = false);
  void stopJob();
  void reapJob();
  void pollJob();
  // `onCancel` is where a Wi-Fi page the user backed out of (or that failed to
  // connect) leaves them. Without it every network entry point dumped the user on
  // the main menu, losing the book they had just opened or the range they had just
  // picked.
  // returnDeepLinkOnCancel: when the caller is a reader deep link, a canceled
  // Wi-Fi page must finish the child activity instead of leaving the user on
  // an in-app message with the reader waiting underneath.
  void withNetwork(std::function<void()> job, State onCancel = State::Menu,
                   bool returnDeepLinkOnCancel = false);

  // Actions
  void onMenuConfirm();
  void onAccountConfirm();
  void onListConfirm();
  void promptAccountName();
  void promptPassword();
  void promptDeviceCode();
  // `type` is the site's own search selector: 1 by title, 2 by author. Both land
  // in the same results state; the header says which one ran.
  void promptSearchKeyword(uint8_t type);
  void loadDiscoverSource(size_t index);
  void loadDiscoverPage(const std::string& url, bool append);
  jjwxc::Status fetchDiscoverPage(const std::string& url, bool append, uint32_t requestedPage);
  static bool onDiscoverEntry(void* ctx, const jjwxc::DiscoverEntry& entry, size_t index);
  void promptAdvKeyword();
  void promptAdvInclude();
  void promptAdvExclude();
  void cycleAdvFinish();
  void cycleAdvSort();
  void startAdvancedSearch();
  jjwxc::Status runAdvancedSearch();
  // Favourites. `after` is where the list lands once fetched (Favorites itself,
  // or FavoritePicker when the fetch was triggered from a book).
  void loadFavoriteClasses(State after);
  jjwxc::Status fetchFavoriteClasses();
  static bool onFavoriteClass(void* ctx, const jjwxc::FavoriteClass& item, size_t index);
  void openFavoriteFolder(const std::string& classId);
  void promptFavoritePick();
  void addToFavoriteClass(const std::string& classId, const std::string& className);
  // Reads /book2/fv/<nid> to learn whether the book is already favourited. One
  // request only when the shelf cannot answer (a book opened from search or a
  // channel); best effort, never changes jobStatus_.
  jjwxc::Status fetchFavoriteStatus();
  // addNovelFavorite answers 1059 ("文章不存在") for a book that is already in
  // the favourites, so an unsure caller retries with editNovelFavoriteClass.
  jjwxc::Status runFavoriteSetClass(const std::string& classId, bool preferMove);
  // Fetches the paragraph-comment totals for one chapter and writes the reader's
  // JJC1 sidecar. Best effort: the text cache must not fail because comments did.
  // `includeBodies` additionally fetches the first page of every commented
  // paragraph (single-chapter runs only; a whole-book run would be thousands of
  // requests). Bodies already on the card are skipped.
  void fetchAndStoreCommentCounts(uint32_t chapterId, bool includeBodies);
  struct CommentPrefetchResult {
    size_t fetched = 0;
    size_t skipped = 0;
    size_t failed = 0;
    size_t requests = 0;
    bool budgetReached = false;
  };
  // Fetches the first page of every commented paragraph in `counts`. The caller
  // supplies an absolute deadline and a request budget so a whole-book pass can
  // stop cleanly between paragraphs instead of running for hours.
  CommentPrefetchResult prefetchCommentBodies(uint32_t chapterId, const JjwxcCommentSidecar::Count* counts,
                                              size_t count, uint32_t deadlineMs, size_t requestBudget);
  // Explicit, resumable whole-book pass over the chapters already on the card.
  // It fetches comment counts for chapters that have no JJC1 sidecar, then the
  // bodies. Existing JJD1 files are skipped, so running it again continues where
  // the last run stopped.
  void prefetchAllCommentBodies();
  void promptPrefetchCommentBodies();
  static bool onCommentCount(void* ctx, const jjwxc::ParagraphComment& item, size_t index);
  static bool onPrefetchComment(void* ctx, const jjwxc::CommentItem& item, size_t index);
  void promptRemoveFavorite();
  void showFavoriteClassActions();
  void promptNewFavoriteClass();
  void promptRenameFavoriteClass(const std::string& classId, const std::string& name);
  void promptDeleteFavoriteClass(const std::string& classId, const std::string& name);
  // One POST against a favourite endpoint. Returns Ok only when the JSON says
  // code 200; the service's own sentence is copied to jobServerMessage_.
  jjwxc::Status postFavorite(const char* path, const std::string& body);
  void launchFavoriteMutation(const char* path, const std::string& body, const std::string& notice, State after,
                              bool reloadClasses);
  // Reads the reader's progress.bin, reports the cloud position for this book,
  // and pushes a changed local position. Worker task, best effort: it never
  // changes jobStatus_. Requires a session.
  bool syncReadingProgress();
  void jumpToCloudProgress();
  // Chapter purchase. The price is fetched first so the confirmation can name
  // it; the payment password lives only in this object between the keyboard and
  // the POST, and is cleared before the job starts.
  void promptPurchase(uint32_t ordinal);
  jjwxc::Status fetchChapterPrice(uint32_t ordinal);
  void promptPurchaseConfirm();
  void promptPayPassword();
  void startPurchase();
  jjwxc::Status runPurchase(const std::string& password);
  // Paragraph comments. One request per user action, with a minimum interval so
  // holding the key or tapping repeatedly cannot turn the endpoint into a hammer.
  void promptComments(uint32_t ordinal);
  bool commentRequestAllowed(State returnTo);
  jjwxc::Status fetchParagraphComments(uint32_t ordinal);
  jjwxc::Status fetchCommentList(uint32_t ordinal, uint32_t paragraphId, uint32_t offset, bool append);
  static bool onParagraphComment(void* ctx, const jjwxc::ParagraphComment& item, size_t index);
  static bool onCommentItem(void* ctx, const jjwxc::CommentItem& item, size_t index);
  void openCommentList(uint32_t paragraphId);
  void loadMoreComments();
  void startLogin(bool withDeviceCode);
  // Main-device approval: the login fallback for a risk check this device cannot
  // solve. Uses the credentials already in memory or the saved shortcut.
  bool hasLoginPassword() const;
  void startMainDeviceApproval();
  jjwxc::Status runMainDeviceApproval(const std::string& loginName, const std::string& password);
  // Worker-task half shared by both login paths: persist the jar, the reader id
  // and the login shortcut after the server accepted the attempt.
  void applyLoginSuccessWorker(const jjwxc::WapLoginResult& res);
  // "用上次账号登录": logs straight in when the password was saved, otherwise asks
  // for it. Never re-prompts for the login name.
  void loginWithSavedAccount();
  // Asks what may be kept on the card, then logs in. The choice is remembered as
  // the next default, so a repeat login is one confirm rather than a fresh decision.
  void promptSaveScope();
  void promptClearAccount();
  void promptLogout();
  void performLogout();
  void openBook(const std::string& novelId);
  // Set for the lifetime of a RefreshUpdates job; pollJob() turns its completion
  // into a result instead of a screen.
  bool updateCheckOnly_ = false;
  void loadShelf();
  // Worker task: one row per online-shelf book into /.crosspoint/jjwxc/updates.bin.
  // The shelf, home card and reader read that file; none of them brings up Wi-Fi.
  bool saveShelfSnapshot();
  // Runs the walk as a child activity for the Library's sync button and finishes
  // with a result instead of entering State::Shelf.
  void runUpdateCheck();
  void loadChapters(State after, bool repackageAfter = false);
  void showBookActions();
  void showChapterActions();
  void promptRefresh();
  void startCachedRefresh();
  void promptFirstN();
  void promptRangeStart();
  void promptRangeEnd();
  void promptChapterJump();
  // State::ChapterList shows at most kMaxRows rows. chapterWindow_ is the first
  // chapter those rows come from; the row layout is [prev?][jump][chapters][next?],
  // so selected_ (a row index) is not a chapter ordinal any more.
  int chapterWindow_ = 0;
  int chapterRowStart_ = 0;
  int chapterPageSpan_ = 0;
  int chapterIndexForRow(int row) const;
  void startCaching(uint32_t fromOrdinal, uint32_t toOrdinal, bool openAfter, CacheMode mode);
  // The launch half of startCaching(): the part that runs once Wi-Fi is up, so a
  // withNetwork() that the user backs out of never touches the download state.
  void launchCacheJob(uint32_t fromOrdinal, uint32_t toOrdinal, bool openAfter, CacheMode mode, State origin);
  void cacheSingleChapter(uint32_t ordinal, bool openAfter);
  void cacheChapterRange(uint32_t fromOrdinal, uint32_t toOrdinal);
  // Points the reader at one chapter instead of at whatever progress.bin last held.
  // Written after invalidateReaderCache() and before handOffToReader().
  void writeReaderSpineTarget(const std::string& path, uint32_t spineIndex);
  // One line for the message page: what the book holds now and what this run did to
  // it. renderMessage() wraps greedily and has no notion of a newline, so the parts
  // are joined with a separator rather than laid out on separate lines.
  std::string cacheSummary() const;
  // Worker task.
  bool downloadCover();
  // cover.source.jpg -> cover.thumb.bmp at the size the detail page lays out, using
  // the same JPEG/PNG -> BMP converter WeRead uses for its shelf covers
  // (WeReadClient.cpp:3427-3430) and the same Bitmap the themes already draw. No new
  // image decoder enters the image, and both converters are already linked.
  bool buildCoverThumb();
  // Makes sure a drawable thumbnail exists, fetching the source first when the card
  // holds neither. Best effort and never fatal: a book without a cover still opens,
  // still caches and still packages -- the page just has no picture on it.
  bool ensureCoverThumb();
  // UI thread. Opens cover.thumb.bmp and parses its headers ONCE per BookDetail
  // entry, so the render path allocates nothing per frame: Bitmap's row scratch is
  // allocated by the first drawBitmap() and reused for the rest of the screen's life.
  void loadCoverThumb();
  // Destroys the bitmap BEFORE closing the file it points at, and is safe to call
  // when nothing is open. Member files are closed here rather than left to the
  // destructor (AGENTS.md rule #5).
  void releaseCover();
  // Worker task.
  bool writeChapterStub(const std::string& path, const JjwxcStore::TocRecord& rec, bool locked);
  // True when a toc record describes a paid chapter. `isVip` is the server's own
  // per-chapter flag; `vipChapterId` (the novel-level "first VIP chapter") is the
  // fallback for a catalogue that omits it, and cannot misclassify a free chapter
  // because every chapter below that id is free by definition.
  bool chapterLooksVip(const JjwxcStore::TocRecord& rec) const;
  // Worker-task only. Probes at most once per cache job and caches the verdict in
  // tokenLiveness_/probeStatus_.
  jjwxc::TokenLiveness tokenLiveness();
  void invalidateReaderCache(const std::string& path);
  JjwxcStore::NovelRecord buildNovelRecord(const JjwxcStore::TocSummary& sum, uint32_t remoteCount) const;
  // Worker task only: writes kAccountPath to match saveScope_ and records whether
  // the card now agrees with it.
  bool persistSavedAccount();
  // UI thread only, from pollJob() after jobDone_: mirrors the persisted scope into
  // savedAccount_. Split from persistSavedAccount() because savedAccount_ owns two
  // std::strings that rebuildRows() reads, and assigning them from the worker would
  // race with the render of the account page.
  void applyPersistedAccount();
  // Fills chapters_/chapterStates_ from the toc already on the card, so a book
  // that was catalogued before stays browsable when the network is not up.
  bool loadTocIntoMemory();
  void openLocalBook(const std::string& novelId);
  void handOffToReader(const std::string& path);
  void loadLibrary();
  // Downloaded-book actions.
  void promptDeleteNovelCache();
  jjwxc::Status runDeleteNovel();
  // Re-fetches the chapter the reader last showed (progress.bin + spine.bin).
  void refreshCurrentChapter();

  // Streaming sinks (static: the C API takes a function pointer + context)
  static bool onShelfEntry(void* ctx, const jjwxc::ShelfEntry& e, size_t index);
  // The WAP shelf parser emits WapShelfEntry, which is a field-for-field redeclaration
  // of ShelfEntry (verified identical: 432 bytes, same nine offsets, both trivially
  // copyable). The static_asserts live next to the memcpy in the .cpp so a future field
  // added to only one of the two fails the build instead of shifting offsets silently.
  static bool onWapShelfEntry(void* ctx, const jjwxc::WapShelfEntry& e, size_t index);
  // Fetches every page of GET /my/fav and feeds onShelfEntry. Runs on the worker task.
  jjwxc::Status fetchWapShelf();
  void runSearch();
  jjwxc::Status fetchWapSearch();
  // Fetches ONE VIP chapter over WAP and emits UTF-8 prose to `onText`.
  //
  // Free chapters do not come through here: app-cdn's chapterContent serves them
  // anonymously as plaintext JSON with no watermark (verified: PUA=0, zero-width=0), so
  // they stay on the existing jjwxc::fetchChapter path. Only VIP bodies need the sid,
  // the two-stage AES, the CSS displacement and the per-session font watermark.
  jjwxc::Status fetchWapVipChapter(const char* novelId, uint32_t chapterId, jjwxc::ChapterMeta& meta,
                                   jjwxc::TextSink onText, void* textCtx);
  static bool onSearchEntry(void* ctx, const jjwxc::SearchItem& item, size_t index);
  static bool onSearchEntryAdapter(void* ctx, const jjwxc::WapShelfEntry& e, size_t index);
  static bool onChapterEntry(void* ctx, const jjwxc::ChapterEntry& e, size_t index);
  static bool onChapterText(void* ctx, const char* utf8, size_t len);
  static bool onNovelEntry(void* ctx, const JjwxcStore::NovelRecord& rec, size_t index);
  // Receives `novelIntro`, the long blurb, from the SAX parser. Static because the
  // library's sink API is a function pointer plus a context. The value arrives RAW
  // (JJWXC escapes its markup: the line breaks are the literal `&lt;br/&gt;`), so it
  // is unescaped here with the same two functions the WAP chapter bodies use.
  static bool onNovelIntro(void* ctx, const char* value, size_t len);
  static bool onPackageCancelled(void* ctx);
  static void onPackageProgress(void* ctx);
  static bool onCoverBody(void* ctx, const uint8_t* data, size_t len);
  static void onCoverHeader(void* ctx, const char* name, size_t nameLen, const char* value, size_t valueLen);

  // Rendering
  void renderHeader(const char* title, const char* subtitle) const;
  void renderRows() const;
  // The unified search page's three mode chips (书名 / 作者 / 高级). One geometry
  // source feeds the band height infoBlockHeight() reports, the drawing and the
  // tap test in loop(), so the chips cannot drift from what the rows start below
  // (AGENTS.md rule #14). Returns the chip height; `out` is filled with up to
  // `maxOut` chip rects.
  int searchChipLayout(Rect* out, int maxOut) const;
  void renderSearchChips() const;
  // The unified search page's rows. Ordinary rows keep the app's label/value row
  // language; the final "start search" row is painted as a solid pill instead of
  // text, while staying in rows_/rowIds_ so the keyboard path, rowTouch() and
  // visibleRows() need no special case.
  void renderSearchRows() const;
  // CPY (frame 14) chapter rows: a number gutter, the title, and a state tag or
  // the 起/止 range markers, one line per row. Its own function rather than a
  // branch inside renderRows() because the row language shares nothing with the
  // two-line title+subtitle defaults.
  void renderCpyChapterRows() const;
  // CPY (frame 13) account page: the status card carrying the mascot, and the
  // account rows wrapped in the design's card. Geometry is the same split as
  // the chapter page -- infoBlockHeight() is the header half's height and both
  // drawing functions live off rowTop()/rowHeight().
  void renderCpyAccountHeader() const;
  // Frame 13's row language for every remaining app list (account, menu,
  // shelf, library, search, range presets): 96px single-line rows with the
  // subtitle as the right-hand value. Short action lists ride in the design's
  // card; long browsing lists stay plain.
  void renderCpyPlainRows(bool withCard) const;
  // CPY (frame 12) book detail: the action pills that replace the rows band, and
  // (in renderBookDetail) the card framing the cover + facts. cpyDetailPillLayout
  // is the one geometry source for the drawing, the tap test and rowHeight(),
  // so the band they reserve and the band they hit cannot differ.
  int cpyDetailPillLayout(int top, Rect* out, int maxOut) const;
  void renderCpyDetailPills() const;
  void renderDisclaimer() const;
  void renderMessage() const;
  void renderProgress() const;
  void renderBusy() const;
  // A job card drawn over the page that launched it, used where a dedicated
  // full-screen page would only add refreshes (deleting a local cache). The page
  // itself is still painted first so no stale dialog pixels survive underneath.
  void renderInlineJobOverlay() const;
  // Transient confirmation pill over the current page; cleared by loop().
  void renderToast() const;
  void showToast(const char* text, uint32_t durationMs = 1600);
  void renderInfoBlock() const;
  // Text lines drawn between the header and the first list row. One function
  // feeds both the block height (so rowTop()/visibleRows() and therefore the row
  // hit-test move down with it) and the drawing loop, which is what keeps
  // drawing and hit-testing on one geometry source (AGENTS.md rule #14).
  // BookDetail does NOT go through this: its facts wrap, so they are composed and
  // wrapped once into factText_/factLines_ by buildDetailFacts() instead of being
  // re-measured per frame. See composeDetailFact().
  bool infoLine(int index, char* buf, size_t bufSize) const;
  int infoLineCount() const;
  int infoBlockHeight() const;
  // BookDetail's own block: cover, the fact lines beside it, and the wrapped blurb
  // underneath. detailLayout() is the single source of the numbers both this and
  // rowTop()/visibleRows() read, and is computed once per entry rather than per
  // frame.
  const DetailLayout& detailLayout() const;
  void renderBookDetail() const;
  // The i-th fact of the detail block, composed as "label  value" into `buf`, plus how
  // that fact may be broken: novelTags at its separators, novelClass at its dashes,
  // everything else stays on one clipped line as before. False once the block's fields
  // are exhausted. This is the ONLY place those fields are read for display, so
  // buildDetailFacts() and nothing else decides what a fact says. The break rule comes
  // out as a parameter rather than being keyed on `index`: which facts are present
  // depends on the response (a book with nothing blocked has no "blocked" line), so the
  // tags are not at a fixed index.
  bool composeDetailFact(int index, char* buf, size_t bufSize, jjwxc::WrapField* field) const;
  // Composes every fact and wraps it into factText_/factLines_ at `wrapWidth`. Called
  // from detailLayout(), i.e. once per invalidation, never per frame.
  void buildDetailFacts(int wrapWidth) const;
  // Copies cached fact line `index` into `buf`, NUL-terminated. False when there is no
  // such line.
  bool detailFactLine(int index, char* buf, size_t bufSize) const;
  // The blurb body: novelIntro when the service sent one, otherwise the one-line
  // novelIntroShort. Never null; empty means "draw no blurb section at all".
  const char* blurbText() const;
  size_t blurbLen() const;
  // Pixel width of `len` bytes of UTF-8 in `fontId`. Sums per-code-point advances, so
  // it never needs a NUL-terminated copy and never measures half a sequence.
  int measureTextWidth(const char* text, size_t len, int fontId) const;
  // Byte length of the wrapped line of `text` starting at `start`, or 0 at the end.
  // Thin renderer-facing wrapper over jjwxc::wrapLineLen(), whose rules the host test
  // pins; shared by the counting pass and the drawing pass, so the block height and
  // what actually appears cannot disagree. Always consumes at least one code point, so
  // a glyph wider than the whole line cannot spin forever, and never returns a length
  // that ends inside a code point.
  size_t measureWrappedLine(const char* text, size_t textLen, size_t start, int fontId, int maxWidth) const;
  // The detail page's expand/collapse row. Flips blurbExpanded_, drops the cached
  // layout so it is recomputed in the new mode, rebuilds the row label and puts the
  // page's scroll back where a reader expects it.
  void toggleBlurb();
  void moveSelection(int delta);
  void moveSelectionByPage(int pages);
  void navigateListButtons();
  void ensureVisible();
  int visibleRows() const;
  int listTop() const;
  // Top edge the detail block is drawn at, i.e. listTop() minus the page's own scroll.
  // Never above listTop() for the rows below it: maxDetailScroll() stops the page at the
  // block's own height, which is what keeps renderRows()' clip and rowTouch()'s
  // "y < top is not a row" on the geometry they were written for.
  int blockTop() const;
  int rowTop() const;
  int rowHeight() const;
  // Bottom edge of the content viewport: the theme's safe area, bounded by the panel
  // height the renderer reports (rule #8 -- never a literal 1216 here).
  int detailViewportBottom() const;
  int detailViewportHeight() const;
  // How far the detail page may scroll, and the clamps that keep it there.
  int maxDetailScroll() const;
  void clampDetailScroll();
  // One page of detail scroll. False when there was nothing to scroll, so the caller
  // can fall back to paging the rows.
  bool scrollDetailByPage(int pages);
  int detailPageSize() const;
  // Moves the page so the selected action row is inside the viewport. BookDetail's
  // equivalent of the row-window scroll every other list uses.
  void ensureDetailRowVisible();

  // ---- state ----
  State state_ = State::Disclaimer;
  State returnState_ = State::Menu;
  // One-shot request from the reader's menu. `pendingDeepLink*` is process-wide
  // because the reader creates this activity; `deepLinkAction_` is the copy this
  // instance consumes after openBook() reaches BookDetail.
  static DeepLinkAction pendingDeepLinkAction_;
  static MenuShortcut pendingMenuShortcut_;
  static char pendingDeepLinkNovelId_[16];
  static char pendingDeepLinkQuery_[32];
  static uint32_t pendingDeepLinkChapterId_;
  static bool pendingDeepLinkOpenChapter_;
  DeepLinkAction deepLinkAction_ = DeepLinkAction::None;
  uint32_t deepLinkChapterId_ = 0;
  bool deepLinkOpenChapter_ = false;
  bool deepLinkOpenBookDetail_ = false;
  int pendingDeepLinkOrdinal_ = -1;
  // Stays set until the action's own job finishes; then the activity closes with
  // a JjwxcReaderResult instead of entering a list screen.
  DeepLinkAction deepLinkReturnAction_ = DeepLinkAction::None;
  void finishDeepLink(DeepLinkAction action, bool completed, bool readable = false);

  // Raw content key for the finished EPUB, prepared on the UI task because
  // bookkey::write reaches the device secret (NVS). The packager job only uses
  // the bytes and the already-wrapped key file.
  uint8_t contentKey_[16] = {};
  std::string contentKeyPath_;
  // PSRAM-backed storage, like shelf_ and chapters_ below -- and the strings inside
  // them are PSRAM-backed too, which is the part that actually matters.
  //
  // These were originally moved to PSRAM for their SIZE: 135 books x (2 strings in Row
  // + 1 in rowIds_) is ~96 bytes of vector storage per row, and loading the shelf was
  // measured to cost 54 KB of internal RAM (free 86,551 -> 31,823). That reasoning
  // stopped one step short and concluded the string payloads could stay behind, on the
  // grounds that they were the smaller half. The device then proved otherwise:
  //
  //   [1718744] wolfSSL TLS closed: free=25815 largest=12276
  //   [1718745] job task stack (12288 B) not available; releasing the session and retrying once
  //   [1718745] xTaskCreatePinnedToCore failed twice; internal RAM is exhausted
  //
  // Twelve bytes short. Free internal RAM was not the problem -- 25 KB of it was
  // sitting right there -- the problem was that a 190-chapter list rebuild had cut it
  // into ~380 small string blocks, none of them 12,288 bytes long. And on the runs
  // where the stack did fit, it ate that block whole and the TLS handshake starved
  // instead: wolfSSL connects at largest=17396 and fails at largest=7156, after 62
  // seconds, which the UI can only call "网络错误".
  //
  // So the cost of these strings was never their bytes, it was their count. PsramString
  // keeps both out of internal RAM and leaves it in one piece.
  std::vector<Row, jjwxc::PsramAllocator<Row>> rows_;
  std::vector<jjwxc::PsramString, jjwxc::PsramAllocator<jjwxc::PsramString>> rowIds_;
  int selected_ = 0;
  int scroll_ = 0;
  std::string message_;
  // Optional bold line above message_; carries the name of the operation that
  // failed so fail() never has to concatenate translated fragments.
  std::string messageHeading_;
  std::string busyText_;
  // True while a network/SD job owns the screen. render() then draws the generic
  // busy page instead of the (stale) list underneath, and loop() accepts nothing
  // but cancel. Without it a request that takes seconds looks like a hang.
  bool busyScreen_ = false;
  // Draw the job over the launching page instead of replacing the screen. Set for
  // short SD-only work (cache deletion), where three full-screen transitions
  // (confirm, busy, success) were all the user ever saw of the action.
  bool inlineJob_ = false;
  // Millisecond clock for the busy page's elapsed line and its repaint cadence.
  uint32_t busyStartedMs_ = 0;
  uint32_t lastBusyTickMs_ = 0;
  // Cancel pill drawn by renderBusy()/renderProgress(), hit-tested by loop().
  mutable Rect busyCancelRect_{};
  // A short result message drawn over the page that remains after the job.
  std::string toast_;
  uint32_t toastUntilMs_ = 0;
  // The frame a state is entered on still carries the gesture that triggered the
  // transition, so loop() drops that frame whole.
  bool entryFrame_ = true;
  // Physical-button release barrier, armed from the held state rather than an
  // edge query (docs/engineering/ui-and-input.md). Touch is deliberately
  // excluded: it stays independent, and gating on it latches the barrier shut.
  bool waitForConfirmRelease_ = false;
  bool waitForBackRelease_ = false;
  // A newly entered screen gets one full waveform to clear the previous state's
  // ghosting; selection-only updates then use the fast LUT.
  bool fullRefreshPending_ = true;
  // Accept-button top edge recorded by renderDisclaimer(), so input hit-tests
  // the geometry that was actually drawn. -1 until the first render.
  mutable int disclaimerAcceptY_ = -1;
  // infoLineCount() walks infoLine() once per state change rather than once per
  // row per frame; -1 means "recompute". Reset by enter().
  mutable int infoLineCount_ = -1;

  OptionPopup optionPopup_;
  // Rule #13: the popup can close on a key edge, so the resumed screen gates the
  // inherited hold and consumes that release (same pattern as
  // WeReadActivity::loop()'s optionPopupClosing_).
  bool optionPopupClosing_ = false;
  ButtonNavigator buttonNavigator_;

  // ---- session ----
  jjwxc::Credentials cred_;
  std::string accountName_;
  std::string password_;
  std::string pendingCheckType_;
  std::string pendingDeviceCode_;
  // The login shortcut kept on the card, and what the user last allowed us to keep.
  // savedAccount_ is read by rebuildRows() and written only on the UI thread; the
  // login job publishes through jobAccountPersisted_ instead of touching it.
  JjwxcStore::SavedAccount savedAccount_;
  JjwxcStore::SaveScope saveScope_ = JjwxcStore::SaveScope::AccountOnly;
  std::atomic<bool> jobAccountPersisted_{false};
  // Set by the login job when the server answered 221003; pollJob() turns it
  // into a request for the emailed/SMSed code and retries.
  bool needDeviceCode_ = false;
  bool pendingPromptDeviceCode_ = false;
  // One code send per user-initiated login. The service can answer "supply a
  // verification code" again after we have supplied one -- 6018 is not documented
  // and its exact meaning is not confirmed -- and without this bound each answer
  // would email the user again and re-prompt, forever. Reset by startLogin(false),
  // i.e. by a fresh attempt that carries no code, which is the only thing that
  // legitimately starts a new sequence.
  bool deviceCodeSent_ = false;
  JjwxcHttpTransport transport_;

  // ---- WAP session ----
  // All three live in PSRAM. The jar is 5 KB and WapIo ~15 KB (the 12 KB URL
  // buffer is sized for the double-encoded writeComment GET), which is nothing next
  // to 6 MB of PSRAM and a lot next to internal RAM, where the largest free block has
  // measured 16 KB after Wi-Fi+TLS while the display DMA needs 45 KB contiguous.
  // Allocated lazily in ensureWapSession() because onEnter() can be reached on a C3
  // build with no PSRAM at all, where this app is compiled out anyway.
  jjwxc::CookieJar* jar_ = nullptr;
  jjwxc::WapIo* wapIo_ = nullptr;
  jjwxc::WapClient* wap_ = nullptr;

  bool ensureWapSession();
  void releaseWapSession();
  // Gates the one full-size "the WAP VIP chain worked" log line to the first success per
  // app entry. A 200-chapter download must not print 200 of them, but the first is the
  // only place the whole chain's sizes and cost are visible together.
  bool vipChainProven_ = false;
  // The WAP session, not Credentials, is what "logged in" means now: sid is 133
  // characters and Credentials::token is 48. Every UI gate that used to ask
  // cred_.valid() asks this instead.
  bool haveSession() const;
  // Unix seconds for cookie expiry, or 0 meaning "the clock is not set" -- which the
  // jar treats as "nothing has expired". Guarded against an unsynced RTC returning a
  // small non-zero value, because that would expire a 365-day sid on the spot and
  // read as a logout on every cold boot.
  static uint64_t cookieClock();

  uint8_t* scratch_ = nullptr;
  size_t scratchCap_ = 0;
  // True once this activity brought Wi-Fi up itself, so onExit() tears it down
  // again instead of leaving the radio on behind the user's back.
  bool wifiSessionActive_ = false;

  // ---- worker task ----
  std::function<void()> jobBody_;
  std::atomic<bool> jobDone_{false};
  std::atomic<bool> jobCancel_{false};
  bool jobRunning_ = false;
  TaskHandle_t jobTask_ = nullptr;
  // True when jobTask_ was created with xTaskCreatePinnedToCoreWithCaps, which puts the
  // stack in PSRAM and the TCB in internal RAM. Such a task MUST be deleted with
  // vTaskDeleteWithCaps(): plain vTaskDelete() treats the stack as caller-owned and
  // silently leaks it, 48 KB of PSRAM per job. Tracked rather than assumed because the
  // creation falls back to the ordinary API when PSRAM is unavailable.
  bool jobStackInPsram_ = false;
  jjwxc::Status jobStatus_ = jjwxc::Status::Ok;
  FailKind jobFailKind_ = FailKind::Protocol;
  std::string jobWhat_;
  State jobNextState_ = State::Menu;
  // Set by the streaming sinks on their first entry. Clearing the target vector
  // before the request instead would throw away the last good shelf/search when
  // the request fails, which is exactly what WeRead avoids by only swapping in a
  // complete result.
  bool jobReplacedTarget_ = false;
  // Set by a packaging failure, which is not a jjwxc::Status and so cannot ride
  // jobStatus_ without inventing a mapping.
  bool jobPackageFailed_ = false;
  // Set by cacheChapterRange when the requested range produced nothing readable --
  // every chapter in it is an "unpurchased"/"locked" placeholder. Handing the
  // reader a book then would drop the user into a document that does not contain
  // the chapter they asked to read, so the message page says so instead.
  bool jobRangeUnreadable_ = false;
  // The raw JJWXC business code behind the current failure, 0 when there was none.
  // statusFromCode()'s `default` arm folds every code it does not know into
  // Status::Server, and that catch-all has repeatedly hidden the real answer (code
  // 200 read as a failure, then 1013/1010 read as "the server is broken", each
  // costing a flashing cycle to find out). Showing the number costs one snprintf
  // and makes the next unknown code diagnosable from a photo of the screen.
  long jobDetailCode_ = 0;
  // The service's own words, when it sent any. Shown only where the mapped status
  // would otherwise be the catch-all "server error": that mapping is lossy, and the
  // sentence the server actually returned is the one thing that says what happened.
  // Written by the worker, read by fail() after pollJob() observed jobDone_.
  char jobServerMessage_[128] = {};
  // Whether the server still accepts this session, for the running cache job.
  //
  // A whole-book cache has to keep going past a chapter the account has not bought,
  // but it must NOT keep going past a dead token: the first leaves one placeholder,
  // the second leaves hundreds of them and tells the user to buy chapters they
  // already own. chapterContent reports both as code 1004 with an identical body,
  // and it serves a FREE chapter even to a dead token, so neither the status nor a
  // free-chapter retry can separate them (both verified live; see
  // jjwxc::probeTokenAlive). One `favorite` request per job can, and its verdict is
  // cached here so a book whose tail is all unpurchased VIP chapters costs one
  // probe, not one per chapter. Reset per job: a token that was alive an hour ago
  // may not be now. Worker task only, so no atomic.
  jjwxc::TokenLiveness tokenLiveness_ = jjwxc::TokenLiveness::Alive;
  bool tokenProbed_ = false;
  // The status an inconclusive probe returned, so the run reports the real cause
  // instead of a bare "sign in again".
  jjwxc::Status probeStatus_ = jjwxc::Status::Ok;

  // ---- data ----
  // PSRAM-backed: a long novel's chapter list is hundreds of KB, and internal
  // RAM has ~106 KB free after boot. These element types are trivially
  // copyable, which is what the allocator requires.
  std::vector<jjwxc::ShelfEntry, jjwxc::PsramAllocator<jjwxc::ShelfEntry>> shelf_;
  std::vector<jjwxc::SearchItem, jjwxc::PsramAllocator<jjwxc::SearchItem>> search_;
  uint32_t searchTotal_ = 0;
  std::vector<jjwxc::ChapterEntry, jjwxc::PsramAllocator<jjwxc::ChapterEntry>> chapters_;
  // Parallel to chapters_ and filled from the same sink: the JXT1 state byte per
  // chapter, so the list can say which chapters are already on the card without
  // a Storage.exists() per row per frame.
  std::vector<uint8_t, jjwxc::PsramAllocator<uint8_t>> chapterStates_;
  uint32_t chapterTotal_ = 0;
  uint32_t chapterVipStart_ = 0;
  std::vector<JjwxcStore::NovelRecord, jjwxc::PsramAllocator<JjwxcStore::NovelRecord>> books_;

  // ---- current book ----
  std::string novelId_;
  // Which list the book was opened from, so Back returns there. Guessing it from
  // "is the shelf non-empty" sent a user who arrived via Search back to the shelf
  // (or to the main menu), which is the kind of back-key surprise that makes a
  // state machine feel broken.
  State bookDetailOrigin_ = State::Menu;
  jjwxc::NovelInfo info_{};
  // The long blurb, `novelIntro`, unescaped. PSRAM-backed and therefore NOT a member
  // of NovelInfo: a measured blurb is 1,868 UTF-8 bytes and the cap below is 8 KB,
  // which as a fixed char array would sit in internal RAM for the whole life of the
  // activity -- exactly the allocation AGENTS.md §7 exists to prevent. Empty when the
  // service sent none, when the fetch was skipped (offline) or when it was refused.
  jjwxc::PsramString intro_;
  // The detail page's cover, opened and header-parsed by loadCoverThumb() when
  // BookDetail is entered and released again when it is left. Holding it across
  // frames is what keeps the render path allocation-free; Bitmap is non-copyable and
  // non-movable, so it lives behind a pointer.
  HalFile coverFile_;
  std::unique_ptr<Bitmap> coverBitmap_;
  // Cached by detailLayout(); invalidated by enter() and by anything that changes
  // the blurb or the cover.
  mutable DetailLayout detailLayout_;
  mutable bool detailLayoutValid_ = false;
  // Wrapped-line bookkeeping for the detail block, all of it PSRAM-backed and all of
  // it rebuilt by detailLayout() rather than per frame. This is what keeps measuring a
  // 1,900-byte blurb and a sixteen-line fact column out of the render path, and it is
  // why the two are byte ranges (jjwxc::WrapLine) instead of std::string lines: a
  // vector of strings would put every character back into the ~40 KB of internal RAM
  // this app has left (see the PsramString comment above).
  // factText_ owns the composed "label  value" bytes that factLines_ indexes into.
  mutable jjwxc::PsramString factText_;
  mutable std::vector<jjwxc::WrapLine, jjwxc::PsramAllocator<jjwxc::WrapLine>> factLines_;
  // Byte ranges of blurbText(), so drawing a scrolled page skips lines instead of
  // re-measuring every one of them from the start of the paragraph.
  mutable std::vector<jjwxc::WrapLine, jjwxc::PsramAllocator<jjwxc::WrapLine>> blurbLines_;
  // blurbLen() the cached ranges were built for. Part of the layout cache's validity:
  // a blurb that changed under the cache would otherwise be indexed by stale offsets.
  mutable size_t blurbLinesForLen_ = 0;
  // False = the blurb is clamped to what leaves the action rows on the first screen;
  // true = it takes its full wrapped height and the page scrolls through it. Reset by
  // enter(), so a book always opens collapsed.
  bool blurbExpanded_ = false;
  // Pixels the whole detail page (block AND rows) is scrolled up by. BookDetail is the
  // only state that uses it; blockTop() folds it into every Y the block, the rows and
  // the row hit-test are drawn from, so the two cannot drift apart.
  int detailScroll_ = 0;
  // Persisted metadata of the novel being viewed; also what finalBookPath() is
  // derived from, so the artefact's name cannot drift with a server-side rename.
  JjwxcStore::NovelRecord novel_{};
  // What toc.bin holds. Its `count` can exceed chapters_.size(): the in-memory
  // catalogue is capped at kMaxChapters while the index on the card is not.
  JjwxcStore::TocSummary tocSummary_{};
  // Closed range of 0-based toc ordinals. -1 means "not chosen yet", which is
  // what keeps an unset custom range from silently meaning chapter 1.
  int rangeFrom_ = -1;
  int rangeTo_ = -1;
  // Value the "first N chapters" preset was last confirmed with.
  int firstN_ = 20;

  // ---- download progress ----
  // Written by the worker task, read by render(): atomic so the progress bar
  // can never observe a torn value.
  std::atomic<size_t> dlDone_{0};
  std::atomic<size_t> dlTotal_{0};
  std::atomic<size_t> dlBytes_{0};
  std::atomic<uint8_t> dlPhase_{0};
  // Stage inside the current chapter (request, watermark font, decode, write).
  // The chapter counter alone only moves once per chapter, so a one-chapter run
  // used to sit at 0% until it jumped to 100%.
  std::atomic<uint8_t> dlStage_{0};
  std::string dlTitle_;
  // Finished EPUB, written by the worker and read only by pollJob() after it has
  // observed jobDone_ -- the same ordering jobStatus_/jobWhat_ already rely on.
  std::string dlPath_;
  // True while the running job hands the EPUB to the reader on success instead of
  // entering a list state.
  bool openAfterJob_ = false;
  // Which chapters the running job may touch.
  CacheMode cacheMode_ = CacheMode::Incremental;
  // Chapters the running job left alone: a recorded refusal it did not re-request,
  // or everything after the refusal that ended the run. Shown in the progress page
  // and the completion summary, because "done" alone would read as "cached".
  std::atomic<uint32_t> dlSkipped_{0};
  // Chapters that got a real body during the running job. Distinct from dlSkipped_
  // and from tocSummary_.readable, which counts the whole book rather than the run.
  std::atomic<uint32_t> jobNewlyCached_{0};
  // True while the running job is a cache run, so pollJob() ends it with the count
  // summary instead of silently re-entering the range page.
  bool jobWasCache_ = false;
  // Set when a run ended at a purchase refusal the App API path can actually have
  // caused (see jjwxc::refusalEndsRun) instead of reaching the end of the range,
  // plus where it ended, so the summary can say so.
  bool jobStoppedAtPurchase_ = false;
  uint32_t jobStoppedOrdinal_ = 0;
  // Spine index the reader should open at, or -1 to leave progress.bin alone.
  int jobTargetSpine_ = -1;
  // Toc ordinal the reader should open at, or -1 for "wherever progress.bin says".
  // Only a single-chapter run sets it; the worker turns it into jobTargetSpine_
  // once it knows how many packaged chapters sit ahead of it.
  int jobTargetOrdinal_ = -1;
  // Set by the refresh confirmation and consumed by whichever range preset the
  // user then picks; enter() drops it as soon as the range screen is left, so a
  // refresh can never leak into an unrelated later download.
  bool refreshPending_ = false;
  // True when this run only added chapters after the last one already in the
  // book, which is what makes it safe to carry the reader's progress across the
  // cache invalidation a repackage needs.
  bool appendOnlyRun_ = true;

  // ---- discovery / search ----
  // Book lists from channels, rankings and advanced search. Same PSRAM rule as
  // shelf_: a crowded channel page emits ~250 rows and the internal heap cannot
  // afford either the vector or its strings.
  std::vector<jjwxc::DiscoverEntry, jjwxc::PsramAllocator<jjwxc::DiscoverEntry>> discover_;
  uint32_t discoverPage_ = 1;
  uint32_t discoverMax_ = 0;
  // Exact href the page's own pager pointed at for the next page, or empty.
  std::string discoverNextUrl_;
  // URL the running discovery job was started with. A member rather than a
  // std::string captured by the job lambda: the lambda would then allocate its
  // own copy inside the worker task.
  std::string discoverRequestUrl_;
  // Header line: the source name (its translated label, so the row and the page
  // say the same thing).
  std::string discoverTitle_;
  // Where Back goes from DiscoverList: the source menu or the advanced-search
  // form. Both feed the same list state.
  State discoverListOrigin_ = State::Discover;
  // True when the running fetch appends to discover_ instead of replacing it.
  bool jobAppendTarget_ = false;
  std::string keyword_;
  // The site's t= parameter for the running search: 1 title, 2 author.
  uint8_t searchType_ = 1;
  // Unified search page: 0 title, 1 author, 2 advanced.
  uint8_t searchMode_ = 0;

  // Advanced search. advFinish_/advSort_ hold the SITE's own values (isfinish
  // 0/1/2, sortType from the table), so the URL is a direct rendering of the
  // form and a future value only needs a table entry.
  std::string advKeyword_;
  std::string advInclude_;
  std::string advExclude_;
  int advFinish_ = 0;
  int advSort_ = 0;
  // A tag name the site's dictionary does not know; shown after the job instead
  // of a generic protocol error.
  std::string jobTagMissing_;
  // Written by a mutation job, shown by pollJob() on the success path. A success
  // page that silently returns to a list reads as "nothing happened".
  std::string jobNotice_;
  // -1 leave the cached favourite state alone; 0 not favourited; 1 favourited.
  int jobFavoriteAfter_ = -1;
  // True when the mutation should re-fetch the class list before landing.
  bool jobReloadFavorites_ = false;
  bool jobDeletedNovel_ = false;

  // ---- paragraph comments ----
  std::vector<jjwxc::ParagraphComment, jjwxc::PsramAllocator<jjwxc::ParagraphComment>> paragraphs_;
  std::vector<jjwxc::CommentItem, jjwxc::PsramAllocator<jjwxc::CommentItem>> comments_;
  uint32_t commentsOrdinal_ = 0;
  uint32_t commentsParagraphId_ = 0;
  uint32_t commentTotal_ = 0;
  uint32_t commentOffset_ = 0;
  uint32_t lastCommentRequestMs_ = 0;

  // ---- purchase ----
  int pendingPurchaseOrdinal_ = -1;
  uint32_t pendingPrice_ = 0;
  std::string payPassword_;
  bool jobPurchaseConfirmPending_ = false;
  bool jobPurchaseAlready_ = false;
  bool jobPurchased_ = false;
  // Remembered payment password (device-wrapped). A manual password that led to a
  // successful purchase is handed to the UI task in this fixed buffer, saved
  // there (NVS/SD must not be touched from the PSRAM-stack worker), then wiped.
  bool purchaseUsedSavedPassword_ = false;
  bool purchaseAttemptInFlight_ = false;
  bool pendingPaySave_ = false;
  bool jobPurchaseWrongPassword_ = false;
  char lastPurchasePassword_[8] = {};
  void promptClearPayPassword();

  // ---- reading progress ----
  jjwxc::ReadingProgress cloudProgress_;
  bool cloudHasProgress_ = false;
  bool cloudOrdinalValid_ = false;
  uint32_t cloudOrdinal_ = 0;

  // ---- favourites ----
  std::vector<jjwxc::FavoriteClass, jjwxc::PsramAllocator<jjwxc::FavoriteClass>> favClasses_;
  // Current shelf filter. Empty means the plain /my/fav view (all favourites);
  // non-empty is passed as c=<id> together with fav_filter_action=1.
  std::string shelfClassId_;
  std::string shelfTitle_;
  // Whether this book is in the favourites, and whether we actually know. The
  // destructive action is hidden unless isFavorite_ is known to be true; it used
  // to be offered always and reported success on books that were never favourited.
  bool favoriteKnown_ = false;
  bool isFavorite_ = false;
  std::string favoriteClassId_;
  std::string favoriteClassName_;
  std::string jobFavoriteClassName_;

  // Last progress bucket the panel was asked to redraw, so a 300-chapter
  // download does not request a refresh per chapter (WeRead uses the same 5%
  // bucketing, WeReadActivity.cpp:102 and :885).
  uint8_t lastProgressBucket_ = 0;
  // Phase the panel was last asked to redraw for, so the switch from downloading to
  // packaging repaints at once instead of waiting for the next 5% bucket. Written
  // only by loop(), unlike the atomics the worker publishes.
  uint8_t lastProgressPhase_ = 0;
  // Same idea for the within-chapter stage: the bar and its label move when the
  // worker starts a new stage, not only when a whole chapter finishes.
  uint8_t lastProgressStage_ = 0;
  // millis() deadline until which auto-sleep stays blocked after a job ends.
  // main.cpp's inactivity clock keeps running while preventAutoSleep() holds the
  // timer off, so a download longer than the sleep timeout would otherwise sleep
  // the device the instant the result appeared.
  uint32_t sleepHoldoffUntil_ = 0;

  // One PSRAM region, sliced per job; no two jobs overlap. Raised from 384 KB because
  // the WAP VIP chain needs the page and its UTF-8 form in ONE buffer at once
  // (transcodeGbkInPlace is in-place only), plus the font, the decrypted fragment and
  // the prose. A measured chapter is 62 KB raw / 64 KB transcoded / 45.5 KB font /
  // 20.5 KB body / 16.5 KB text, so 384 KB would fit it with no room for a longer one.
  // PSRAM has ~6.1 MB free, so this costs nothing that matters.
  static constexpr size_t kScratchBytes = 1024 * 1024;
  // Accepted when the full region cannot be allocated. Still enough for a chapter
  // roughly twice the largest one measured.
  static constexpr size_t kScratchBytesFallback = 512 * 1024;
  // Slice layout for the VIP chain, in scratch order. The page slice is first and
  // largest because it has to hold raw + transcoded simultaneously.
  static constexpr size_t kWapPageSlice = 448 * 1024;
  static constexpr size_t kWapFontSlice = 80 * 1024;   // measured TTF is 45.5 KB
  static constexpr size_t kWapWorkSlice = 8 * 1024;    // cryptInfo Base64 decode + JSON
  static constexpr size_t kWapBodySlice = 192 * 1024;  // decrypted HTML fragment
  static constexpr size_t kWapTextSlice = 192 * 1024;  // final UTF-8 prose
  static constexpr size_t kWapMapSlice = 16 * 1024;    // PuaMap (256 x 12 = 3 KB)
  static constexpr size_t kWapVipScratchBytes =
      kWapPageSlice + kWapFontSlice + kWapWorkSlice + kWapBodySlice + kWapTextSlice + kWapMapSlice;
  // Carved out of the one PSRAM scratch region, which no two jobs share. The form is
  // the urlencoded login POST: fourteen fields, the two variable ones tripled at worst
  // by percent-encoding. The page is the response body -- the largest login page
  // measured is 33,701 bytes, and a VIP chapter page is bigger still, so this is sized
  // for both and 96 KB is under a quarter of the scratch.
  static constexpr size_t kWapFormBytes = 1024;
  static constexpr size_t kWapPageBytes = 96 * 1024;
  // How much of a WAP page may be COLLECTED. The rest of the scratch is transcode
  // headroom: transcodeGbkInPlace works in one buffer that must hold the raw page and
  // its UTF-8 form at once. A real shelf page is 45,838 bytes and a real VIP chapter
  // page 62,026 (only 2.7% of growth, since most of it is the ASCII Base64 payload),
  // so 176 KB is ~2.8x the largest page observed and leaves 208 KB to expand into.
  static constexpr size_t kWapPageRawCap = 176 * 1024;
  // The /assort tag dictionary is 227 KB raw, larger than the VIP-page budget
  // above, and it is transcoded in place. The one-megabyte scratch region has the
  // room; the only cost is that a discovery job cannot also hold a VIP chapter.
  static constexpr size_t kDiscoverPageRawCap = 320 * 1024;
  // The shelf pages itself at 30 books per page; kMaxRows is 300, so ten pages is the
  // real bound and twelve leaves room for the site to change the page size.
  static constexpr int kMaxShelfPages = 12;
  static constexpr size_t kMaxRows = 300;
  // 1500 x sizeof(ChapterEntry) is ~216 KB of PSRAM. The old 6000 wanted 864 KB
  // from internal RAM, which has ~106 KB free: reserve() would bad_alloc and,
  // under -fno-exceptions, abort() the device on any long novel.
  static constexpr size_t kMaxChapters = 1500;
  // Cap on the CATALOGUE STREAM, which is a different thing from kMaxChapters. The
  // in-memory mirror is bounded because it costs RAM; toc.bin is bounded only by
  // the card, because TocMerger writes each record straight out and keeps none of
  // them. Passing kMaxChapters here -- as this used to -- silently truncated the
  // index of any longer novel: CapSink reports a hit cap as a normal end of stream
  // (JjwxcProtocol.cpp's `cap.capped`), so a 4000-chapter book came back Ok with
  // 1500 records, "whole book" then meant "first 1500", and nothing logged it.
  // 20000 records is 2.3 MB of toc.bin, far past any real novel, and still bounds
  // a runaway response.
  static constexpr size_t kMaxTocEntries = 20000;
  // The "Downloaded" list is a NovelRecord (368 B) per entry in PSRAM. 96 novels
  // is 35 KB of PSRAM and far more than a card realistically holds; the row
  // renderer caps at kMaxRows anyway.
  static constexpr size_t kMaxLocalBooks = 96;
  // Slice of scratch_ handed to StoreOnlyZipWriter as its copy buffer. Packaging
  // runs after the last fetchChapter of the job, so the two never overlap and no
  // second buffer is allocated.
  static constexpr size_t kZipBufferBytes = 4096;
  // Depth passed to xTaskCreatePinnedToCoreWithCaps, which allocates the stack in PSRAM
  // (see startJob()). Both that allocation and the kernel scale the depth by
  // sizeof(StackType_t), so the effective stack is 4x this -- 48 KB of PSRAM, bought
  // deliberately: the deepest path here (sfnt walk + two AES passes + in-place GBK
  // transcode) has never had its true peak measured, and PSRAM is abundant while internal
  // RAM is what the TLS handshake starves on. The internal-RAM fallback below this keeps
  // the same number, where it means 12 KB as it always did.
  static constexpr uint32_t kJobStackBytes = 12 * 1024;
  // PSRAM task depth. xTaskCreatePinnedToCoreWithCaps multiplies the depth by
  // sizeof(StackType_t), so 24K words here is a 96 KB stack. The paragraph-comment
  // request adds a TLS handshake to www.jjwxc.net on top of the chapter-fetch frame;
  // at the old 48 KB the high-water mark was 56 bytes and the stack canary fired.
  // 96 KB leaves ample room for wolfSSL's deepest handshake path. The internal-RAM
  // fallback keeps 12 KB (it never runs this path on a PSRAM board).
  static constexpr uint32_t kJobStackPsramDepth = 24 * 1024;
  // Must exceed one chapter's worst-case blocking time, or a request that is merely
  // slow always reaches the forced-delete path -- and deleting a task mid-SD-write can
  // corrupt the filesystem.
  //
  // The bound this replaced (60 s, "must exceed the transport's 45 s timeout") rested on
  // an assumption the device disproved: `options.timeoutMs` reaches `client.setTimeout()`,
  // but `client.connect()` is not governed by it. A connect that was going to fail -- for
  // want of contiguous RAM, as it turned out -- blocked for 62 seconds:
  //
  //   [1740909] wolfSSL TLS new: host=m.jjwxc.net free=12067 largest=7156
  //   [1802983] [ERR] [HTTP] wolfSSL request connect failed
  //
  // A VIP chapter makes two such requests in sequence (page, then watermark font), so
  // 60 s could be exceeded by a single chapter with nothing wrong but a slow network.
  // 150 s covers two measured-worst-case connects with margin. The cost is that a cancel
  // during a genuinely stuck request blocks the UI for longer; that is the cheaper side
  // of the trade, because the alternative is a forced delete that can cost the card.
  static constexpr uint32_t kJobStopTimeoutMs = 150000;
  static constexpr uint32_t kChapterDelayMs = 1500;
  // How many consecutive chapters may be refused with a business code this library
  // does not know before the run gives up. An unknown code is treated as
  // per-chapter because the last two surprises (1013 "auto-subscription failed,
  // balance too low" and 1010 "already subscribed / order in flight") were exactly
  // that -- but a rate limit or a client rejection wearing an unknown number would
  // otherwise be stamped into every remaining chapter's placeholder. Three in a row
  // is well past a single odd chapter and well short of a book.
  static constexpr uint32_t kUnknownRefusalLimit = 3;
  // Progress buckets, matching WeRead's kChapterProgressBucketCount.
  static constexpr uint8_t kProgressBuckets = 20;
  // Bounded auto-sleep grace after a job ends. Long enough to read the result and
  // press a key, short enough that a device left alone still sleeps.
  static constexpr uint32_t kSleepHoldoffMs = 120000;
  // Covers are small; this only stops a mis-pointed URL from filling the card.
  static constexpr size_t kMaxCoverBytes = 512 * 1024;
  // The detail page's cover thumbnail is generated at exactly this size, so
  // drawBitmap() can blit it 1:1 into the slot: it scales a bitmap DOWN to fit but
  // never up (GfxRenderer.cpp:1887), and a second rescale of an already-rescaled
  // 2-bit image is what makes covers look muddy. 5:7 is the aspect the service's own
  // `_300_420` JPEG uses, and 200 px leaves the fact column room beside it on a
  // 684-px panel while still shrinking to fit a narrower one.
  static constexpr int kCoverThumbWidth = 200;
  static constexpr int kCoverThumbHeight = 280;
  // Below this the cover slot is dropped entirely and the facts go full width: a
  // thumbnail too small to recognise is worse than no thumbnail.
  static constexpr int kCoverMinWidth = 72;
  // Space between the cover and the fact column, and above the blurb heading. Both
  // are gaps between inert text and a picture, not between interactive controls, so
  // rule #14's 6 px minimum is satisfied with room to spare.
  static constexpr int kCoverGap = 16;
  static constexpr int kBlurbGap = 10;
  // Cap on the RAW `novelIntro` bytes accepted. The measured blurb is 1,868 UTF-8
  // bytes; 8 KB covers a long one with margin and bounds a pathological reply. The
  // allocation is PSRAM, so this costs nothing that matters.
  static constexpr size_t kMaxIntroBytes = 8192;
  // Action rows the COLLAPSED detail page guarantees to keep inside the first screen.
  // Two rather than "all of them": a wrapped fact column plus a cover already costs most
  // of a 1216-px panel, and reserving four to five rows left the blurb no lines at all
  // while still not fitting. Two keeps the expand/collapse row and the first action on
  // screen, which is what the page is for; the rest are one page gesture away. Note
  // this also makes the row count independent of the expand row itself -- BookDetail
  // always has at least two action rows, so min(rowCount, kDetailRowsReserve) is the
  // same with and without it, and rebuildRows() can therefore ask "would the collapsed
  // page hide something?" without the answer depending on the row that asks it.
  static constexpr int kDetailRowsReserve = 2;
  // Bound on the wrapped blurb lines one layout may hold. Only reached when expanded,
  // where the text itself ends the walk long before this; it exists so a pathological
  // reply cannot make the line list grow without limit.
  static constexpr int kMaxBlurbLines = 4096;
};
