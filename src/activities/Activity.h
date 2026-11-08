#pragma once
#include <Logging.h>
#include <Memory.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "activities/MainTab.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  // Debug/diagnostic name (CMD:REFRESHMAP); the activity's registered label.
  const char* debugName() const { return name.c_str(); }
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  // Last chance to queue activity-owned state before sleep events are drained.
  virtual void prepareForSleep() {}
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // One cancellable idle pass after a normal render; zero keeps the task asleep.
  virtual uint32_t idleRenderDelayMs() const { return 0; }
  virtual void renderIdle(uint32_t) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  // Exclusive storage activities suspend global controls and normal activity
  // transitions so no filesystem code races a raw SD-card owner.
  virtual bool requiresExclusiveStorageLoop() const { return false; }
  virtual bool isReaderActivity() const { return false; }
  virtual bool keepsBluetoothAlive() const { return isReaderActivity(); }
  virtual bool deferBluetoothStart() const { return false; }
  // Reading surfaces opt into output-polarity inversion; menus and overlays stay normal.
  virtual bool appliesNightMode() const { return false; }
  // Returns true when the activity schedules its own forced refresh.
  virtual bool handleForcedRefresh() { return false; }
  virtual bool isHomeActivity() const { return false; }
  virtual bool handleHomeGesture() { return false; }
  virtual MainTab mainTab() const { return MainTab::None; }
  virtual bool mainTabBackReturnsToTabs() const { return true; }
  virtual void selectMainTabContentEdge(MainTabContentEdge) {}
  bool usesMainTabBar() const;
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }
  // Debug-only serial hook; activities that own sheets override it.
  virtual void debugOpenPanel(int) {}
  virtual void debugCloseOverlay() {}
  virtual void debugTypeChar() {}
  // Serial-only: prepend synthetic SSIDs that exercise the text fallback chain
  // (covered and missing emoji) to the current Wi-Fi list.
  virtual void debugWifiDemo() {}
  // Serial-only hooks for the reading browser. Kept on the base so main.cpp can
  // drive the app without knowing its type.
  virtual void debugBrowserPage(int) {}
  virtual void debugBrowserLinks() {}
  virtual void debugBrowserBack() {}
  virtual void debugBrowserFollow(int) {}
  virtual void debugBrowserOpen(const char*) {}
  virtual void debugBrowserFixture() {}
  virtual void debugBrowserToggleBookmark() {}
  virtual void debugBrowserBookmarks() {}
  virtual void debugBrowserStatus() {}
  virtual void debugBrowserMenu() {}
  virtual void debugBrowserList(int) {}
  virtual void debugBrowserOpenPlace(int, int) {}
  // Opt-in band updates (GfxRenderer::setBandUpdates): only for 1-bit screens
  // whose repaint changes a small strip. Asked on every manager loop tick, so a
  // pushed or popped activity sets the mode for whatever is on top.
  virtual bool wantsBandUpdates() const { return false; }
  // Present through the 16-level canvas instead of a 1-bit push. Only for
  // screens whose content needs midtones (cover art); a following gray screen
  // then needs no GC16 promotion, so Home <-> Library no longer flashes.
  virtual bool presentsGrayscale() const { return false; }
  // Serial-only soak hook: moves the list ring like a NavNext/NavPrevious press.
  virtual void debugMoveRing(int) {}
  // Serial-only soak hook: performs the Confirm action at the current ring position.
  virtual void debugConfirm() {}
  // Serial-only soak hook: triggers the in-progress Cancel action.
  virtual void debugCancel() {}
  // Serial-only soak hook: performs one game move (0..3 = up/right/down/left).
  virtual void debugGameMove(int) {}
  virtual void debugSetTab(int) {}
  virtual void debugDismissHint() {}
  virtual void debugOpenJjwxcComment() {}
  virtual void debugOpenFakeJjwxcComment() {}
  virtual void debugOpenJjwxcChapterComments() {}
  // Serial-only probe for the comment panel's background prefetch: exercise the
  // real refresh/like paths without a touch screen.
  virtual void debugRefreshJjwxcComment() {}
  virtual void debugLikeJjwxcComment() {}
  virtual void debugOpenJjwxcCacheQueue() {}
  virtual void debugEnqueueJjwxcCacheQueue() {}
  virtual void debugEnqueueJjwxcCacheQueueRange(uint32_t from, uint32_t to) {}
  virtual void debugPauseJjwxcCacheQueue() {}
  virtual void debugClearJjwxcCacheQueue() {}
  virtual void debugResumeJjwxcCacheQueue() {}
  virtual void debugJjwxcCacheQueueStatus() {}
  virtual void debugPageTurn(bool) {}
  // Serial-only: open the reader's selection bar on the current page, so the
  // [look up][highlight][note] chips can be inspected without a long press.
  virtual void debugOpenPhraseSelection() {}
  virtual void debugDumpPage() {}
  virtual void debugOpenStressComment() {}
  virtual void debugOpenSearch() {}
  virtual void debugOpenSearchSample() {}
  virtual void debugRequestSnapshot() {}
  virtual void debugForceEmpty() {}
  virtual void debugSetContentsFilter(int) {}
  virtual void debugActivateContentsRow(int) {}
  // Serial screenshot hook: show the app's waiting surface for a few seconds.
  virtual void debugShowWaitDemo() {}
  // Serial screenshot hook: open the activity's inline confirmation.
  virtual void debugShowConfirmDemo() {}
  // Serial screenshot hook: show the app's short-error surface.
  virtual void debugShowMessageDemo() {}
  // Serial screenshot hook: open a scrollable option picker already scrolled to
  // its last entry, so the scrollbar's bottom end can be inspected on screen.
  virtual void debugShowPopupDemo() {}
  // Serial screenshot hook: run the library's manual rebuild (busy card inside
  // the page's own grayscale pass).
  virtual void debugRebuildLibrary() {}
  virtual void debugAreaTest(int) {}

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  template <typename T, typename... Args>
  bool startActivityForResultWith(ActivityResultHandler resultHandler, Args&&... args) {
    // The child is owned by ActivityManager across frames, so stack storage is
    // not possible; keep its one allocation fallible.
    auto activity = makeUniqueNoThrow<T>(renderer, mappedInput, std::forward<Args>(args)...);
    if (!activity) {
      LOG_ERR("ACT", "OOM: child activity (%u bytes)", static_cast<unsigned>(sizeof(T)));
      return false;
    }
    startActivityForResult(std::move(activity), std::move(resultHandler));
    return true;
  }

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  static void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);

 protected:
  MappedInputManager::Labels mainTabButtonLabels(const char* back, const char* confirm, bool canMove,
                                                 bool showTabDirections = true) const;
  bool showMainTabContentSelection() const;
  bool mainTabsAtBottom() const;
  bool hasMainTabStatusBar() const;
  MainTabLayout mainTabLayout() const;
  Rect pageContentRect() const;
  void drawPageHeader(const Rect& rect, const char* title, const char* subtitle = nullptr) const;

  enum class ListTouchResult : uint8_t {
    None,      // touch did not hit the list
    Consumed,  // touchdown moved the highlight (repaint already requested)
    Activated  // tap landed on a row: selectedIndex is updated, caller activates it
  };

  // Compatibility path for legacy theme lists: touchdown highlights, tap
  // activates, and vertical swipe changes a whole page without activation.
  // New screens use UiListActivity and its separate selection/viewport state.
  ListTouchResult handleListTouch(int& selectedIndex, int itemCount, int listTop, int listHeight, bool hasSubtitle);
};
