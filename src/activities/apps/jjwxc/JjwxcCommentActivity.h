#pragma once

#include <JjwxcComments.h>
#include <JjwxcCookies.h>
#include <JjwxcWapClient.h>
#include <Memory.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "JjwxcHttpTransport.h"
#include "JjwxcPsramAllocator.h"
#include "activities/Activity.h"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#endif

// Paragraph comments (段评) for one paragraph of a JJWXC chapter, opened from a
// bubble in the reader.
//
// Why this is its own activity rather than a reader overlay: it needs the Wi-Fi
// activity (the reader deliberately runs with the radio off), a scrollable list,
// and its own heap for the response body and decoded comments. All three are
// things the reader should not have to own between page turns.
//
// Offline behaviour: the first successful page is written to a per-paragraph
// sidecar (JjwxcCommentSidecar::Bodies). Later opens render it with the radio
// off; the header's Refresh action is the only path that intentionally goes back
// to the network, and it rewrites the sidecar when it succeeds.
//
// The endpoint is the PC-compat one the app already uses
// (lib/JjwxcApi/src/JjwxcComments.h documents the measured shapes):
//   GET https://www.jjwxc.net/app.jjwxc/Pc/comment/getCommentList?...
// It carries the WAP session as `token=urlencode(base64(sid))`, answers JSON
// (UTF-8 in practice, GBK accepted because the same server has returned both),
// and pages at `limit=20` with an `offset` counted in comments already shown.
//
// Requests run on this activity's own stack (not the reader's), which is why the
// response buffer and decoded items live in PSRAM: a page of 20 comments can be
// tens of KB, and internal RAM is what the TLS handshake needs.
class JjwxcCommentActivity final : public Activity {
 public:
  JjwxcCommentActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string novelId,
                       uint32_t chapterId, uint32_t paragraphId, uint16_t paragraphIndex);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class State : uint8_t {
    Loading,      // the first page has not arrived yet
    Ready,        // comments_ is populated (possibly empty after a load-more)
    Empty,        // the paragraph genuinely has no comments
    Failed,       // no session / no Wi-Fi / network or protocol error and no cache
  };

  // The panel has two lists. Replies replace the list view, not the activity, so
  // Back returns to the comments without losing the (already fetched) page.
  enum class View : uint8_t { Comments, Replies };

  // One worker task serves reads and writes. Writes are user-initiated only;
  // nothing here retries or queues one.
  enum class ActionKind : uint8_t {
    None,
    FetchPage,
    FetchReplies,
    LikeComment,
    LikeReply,
    PostComment,
    PostReply,
  };

  enum class WifiPurpose : uint8_t { FirstFetch, Refresh, Action };

  enum class HitKind : uint8_t { Like, Reply, NeedsRefresh };

  bool ensureSession();
  void releaseSession();
  // Loads the sidecar into comments_. Returns true when there is something to
  // show, in which case no radio is touched.
  bool loadCachedComments();
  // Writes comments_ to the sidecar. Best-effort: a failed write costs the
  // next open's offline view, never the current one.
  void persistComments();
  // `purpose` decides what runs when the radio is up: the first page, a refresh,
  // or the action the user just tapped.
  void beginWifi(WifiPurpose purpose);
  void onWifiResult(const ActivityResult& result);
  void fetchPage(uint32_t offset, bool append);
  void fetchReplies(uint32_t offset, bool append);
  // Runs on the activity task after the worker reports done. The worker fills
  // incoming_/incomingReplies_ and the result fields but never mutates
  // comments_/replies_/state_: a render can be in flight during a refresh, and
  // swapping a vector under it would be a data race.
  void applyFetchResult();
  void applyRepliesResult();
  void applyActionResult();
  void refresh();
  // The request runs on a dedicated 32 KB PSRAM-stacked task. It used to run on
  // the activity task, whose stack is not sized for a wolfSSL handshake: the
  // caching worker died at 56 bytes of headroom with a comparable call chain.
  void startWorker();
  void workerRun();
  void startFetch(uint32_t offset, bool append);
  void startRepliesFetch(bool append = false);
  void reapFetch();
  static void fetchTrampoline(void* param);
  void loadMore();
  bool hasMore() const;
  bool replyHasMore() const;
  void fail(const char* text);

  // User actions -----------------------------------------------------------
  bool prepareAction();
  void runActionWhenOnline();
  void startLikeAction(uint32_t index, bool reply);
  void performLike(bool reply);
  void performPostComment();
  void performPostReply();
  void showFeedback(const char* text);
  void showActionError();
  void openComposer(bool reply);
  void enterReplies(uint32_t commentIndex);
  void leaveReplies();
  bool handleTap(int x, int y);
  void addHit(HitKind kind, uint16_t index, int x, int y, int w, int h);

  void drawList(int top);
  void drawReplies(int top);
  void drawNotice(const char* text, const char* subtitle);
  void drawRefreshChip(int right, int y, int height);
  void drawActionChip(int right, int y, int height, const char* label, int& xOut, int& wOut);
  int maxScroll() const;
  int commentCardHeight(const jjwxc::CommentItem& comment, int textWidth) const;
  int replyCardHeight(const jjwxc::CommentReplyItem& reply, int textWidth) const;
  // Shared card body drawing. Returns the y just past the body.
  int drawCardBody(const char* author, const char* body, const char* ip, const char* date, int x, int y,
                   int textWidth, int bodyHeight, int metaHeight);

  static bool onCommentItem(void* ctx, const jjwxc::CommentItem& item, size_t index);
  static bool collectBody(void* ctx, const uint8_t* data, size_t len);

  std::string novelId_;
  uint32_t chapterId_ = 0;
  uint32_t paragraphId_ = 0;
  uint16_t paragraphIndex_ = 0;

  JjwxcHttpTransport transport_;
  jjwxc::CookieJar* jar_ = nullptr;
  jjwxc::WapIo* wapIo_ = nullptr;
  jjwxc::WapClient* wap_ = nullptr;
  memory::ByteBuffer response_;

  // PSRAM-backed: 20 items of ~628 bytes each is already 12 KB, and the list
  // keeps up to kMaxComments across pages. `incoming_` is the worker's staging
  // list: the UI keeps rendering the old page until a refresh parses cleanly, so
  // a malformed answer can never blank a perfectly good cached list.
  std::vector<jjwxc::CommentItem, jjwxc::PsramAllocator<jjwxc::CommentItem>> comments_;
  std::vector<jjwxc::CommentItem, jjwxc::PsramAllocator<jjwxc::CommentItem>> incoming_;
  std::vector<jjwxc::CommentReplyItem, jjwxc::PsramAllocator<jjwxc::CommentReplyItem>> replies_;
  std::vector<jjwxc::CommentReplyItem, jjwxc::PsramAllocator<jjwxc::CommentReplyItem>> incomingReplies_;
  static constexpr size_t kPageSize = 20;
  static constexpr size_t kReplyPageSize = 20;
  static constexpr size_t kMaxComments = 200;
  static constexpr size_t kMaxReplies = 200;

  View view_ = View::Comments;
  uint32_t commentsScrollY_ = 0;
  uint32_t repliesScrollY_ = 0;
  // The comment whose replies are on screen. Kept by index (not id) because the
  // top-level list is what the reply count and the reply composer report back to.
  uint32_t parentCommentIndex_ = 0;
  uint32_t replyTotal_ = 0;
  bool replyLoading_ = false;
  bool replyFailed_ = false;
  bool scrollRepliesToBottom_ = false;
  // Set after a keyboard child returns so its Confirm/Back release cannot be
  // read as a second action here (the same barrier JjwxcActivity uses).
  uint32_t inputBlockUntilMs_ = 0;
  const char* replyErrorText_ = nullptr;

  uint32_t commentTotal_ = 0;
  uint32_t scrollY_ = 0;
  int viewportHeight_ = 0;
  int contentHeight_ = 0;
  // Rect of the trailing "more comments" row in logical screen coordinates; the
  // loop uses the same numbers the render pass drew.
  int moreX_ = 0;
  int moreY_ = 0;
  int moreW_ = 0;
  int moreH_ = 0;
  bool moreVisible_ = false;
  // Toolbar chips, recorded by the render pass and consumed by the loop.
  int refreshX_ = 0;
  int refreshY_ = 0;
  int refreshW_ = 0;
  int refreshH_ = 0;
  int actionX_ = 0;
  int actionY_ = 0;
  int actionW_ = 0;
  int actionH_ = 0;

  // Per-card action chips. The draw pass and the tap pass use the same rects.
  struct HitTarget {
    HitKind kind;
    uint16_t index;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };
  static constexpr size_t kMaxHitTargets = 48;
  HitTarget hits_[kMaxHitTargets] = {};
  size_t hitCount_ = 0;

  State state_ = State::Loading;
  bool wifiBroughtUp_ = false;
  WifiPurpose wifiPurpose_ = WifiPurpose::FirstFetch;
  bool loadingMore_ = false;
  bool refreshing_ = false;
  bool moreFailed_ = false;
  // True when comments_ came from the sidecar rather than this session's network.
  bool showingCache_ = false;
  const char* errorText_ = nullptr;

  // A short line under the toolbar; unlike a toast it survives until the next
  // action, which on e-ink is more useful than a timed message nobody repaints.
  std::string feedback_;
  // The last composed text. Kept when a post fails so a long comment is not
  // lost to a TLS hiccup; cleared only after a successful post.
  std::string draftText_;
  bool draftIsReply_ = false;

  // Request worker. `fetchDone_` is the only cross-task handshake; the UI reads
  // the vectors/state only after observing it.
  TaskHandle_t fetchTask_ = nullptr;
  bool fetchTaskInPsram_ = false;
  std::atomic<bool> fetchDone_{false};
  ActionKind pendingAction_ = ActionKind::None;
  uint32_t pendingOffset_ = 0;
  bool pendingAppend_ = false;
  uint32_t actionCommentIndex_ = 0;
  bool actionIsReply_ = false;
  bool actionLike_ = false;
  bool actionOk_ = true;
  int actionCode_ = -1;
  bool actionCaptcha_ = false;
  char actionMessage_[160] = {};
  // Worker -> activity handoff. Written before fetchDone_ is published.
  bool fetchOk_ = true;
  const char* fetchError_ = nullptr;
  size_t fetchedReceived_ = 0;
  uint32_t fetchedTotal_ = 0;
  size_t fetchedReplies_ = 0;

  // URL/body scratch. writeComment carries the double-encoded body in the query;
  // 300 CJK characters alone expand past 8 KB, so this must be PSRAM and must
  // not sit on the 32 KB worker stack.
  memory::ByteBuffer urlBuf_;
  memory::ByteBuffer bodyBuf_;
};
