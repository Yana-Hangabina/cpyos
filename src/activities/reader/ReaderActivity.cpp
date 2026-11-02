#include "ReaderActivity.h"
#include "activities/apps/jjwxc/JjwxcCacheService.h"

#include "EmojiFont.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Memory.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "CbzReaderActivity.h"
#if defined(CROSSPOINT_MUPDF)
#include "PdfReaderActivity.h"
#endif
#include "EpubReaderActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#if defined(CROSSPOINT_SYSTEM_FONT_PACK)
#include "Typography.h"
#endif
#include "XtcReaderActivity.h"
#include "util/PluginEvents.h"

ReaderActivity::ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::string bookPath, const bool allowFastInitialRefresh)
    : Activity(name, renderer, mappedInput), bookPath(std::move(bookPath)) {
  if (allowFastInitialRefresh) {
    const int refreshFrequency = SETTINGS.getRefreshFrequency();
    pagesUntilFullRefresh = refreshFrequency > 1 ? refreshFrequency : 2;
  }
}

std::unique_ptr<ReaderActivity> ReaderActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string path, const bool allowFastInitialRefresh) {
  // ActivityManager requires heap ownership; each branch allocates exactly one screen-lifetime object.
  std::unique_ptr<ReaderActivity> activity;
  if (FsHelpers::hasXtcExtension(path)) {
    activity = makeUniqueNoThrow<XtcReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else if (FsHelpers::hasCbzExtension(path)) {
    activity = makeUniqueNoThrow<CbzReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
#if defined(CROSSPOINT_MUPDF)
  } else if (FsHelpers::hasPdfExtension(path)) {
    activity = makeUniqueNoThrow<PdfReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
#endif
  } else {
    activity = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }

  if (!activity) {
    LOG_ERR("READER", "OOM: reader activity");
  }
  return activity;
}

void ReaderActivity::applyInitialOrientation() { ReaderUtils::applyOrientation(renderer, SETTINGS.orientation); }

void ReaderActivity::disableFastInitialRefresh() { pagesUntilFullRefresh = 0; }

void ReaderActivity::notePageTurn(const bool forward, const bool succeeded) {
  RenderLock lock(*this);
  readerSession.noteTurn(forward, succeeded);
}

void ReaderActivity::onEnter() {
  Activity::onEnter();
  // Reading has priority: the background paragraph-comment queue yields at its
  // next request and releases the radio/heap. The reader's own comment panel
  // prefetch is a separate, foreground path.
  jjwxc_cache_service::setReaderForeground(true);
  // The reader is where emoji matter; follow the persisted reader point size.
  // Reloading is a no-op unless the size changed since the last book.
  emoji_font::setPointSize(renderer, SETTINGS.fontPointSize);
#if defined(CROSSPOINT_SYSTEM_FONT_PACK)
  // Load the reader-size vector face (and any UI faces this activity needs)
  // before the first page is laid out; a no-op when the set is unchanged.
  typography::apply(renderer, SETTINGS.uiFontScale);
#endif
  // Fast turn is a 1-bit DU path. Keep its old 25% weight; the UI and normal
  // pages keep the 50% threshold that matches the reference firmware.
  GfxRenderer::setBwGlyphThreshold(SETTINGS.fastPageTurn ? 1 : 2);
  GfxRenderer::setBwGlyphThreshold4(SETTINGS.fastPageTurn ? 7 : 10);

  // Heap ledger for field crash reports: free vs largest block distinguishes a
  // leak (free falls) from fragmentation (free stable, largest collapses).
  LOG_INF("MEM", "reader enter: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  if (!Storage.exists(bookPath.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", bookPath.c_str());
    finish();
    return;
  }

  // Clear remembered book after opening it
  if (!APP_STATE.openEpubPath.empty()) {
    APP_STATE.openEpubPath.clear();
    APP_STATE.saveToFile();
  }

  sdFontSystem.ensureLoaded(renderer);
  applyInitialOrientation();

  if (!loadBook()) {
    if (!handleLoadFailure()) finish();
    return;
  }

  requestUpdate();
}

void ReaderActivity::rememberBookOnceRendered() {
  if (bookRemembered || !pageRendered.load(std::memory_order_acquire)) return;
  bookRemembered = true;
  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(bookPath, getBookTitle(), getBookAuthor(), getBookThumbBmpPath());
  const pluginevents::Var openVars[] = {{"book", bookPath.c_str()}};
  pluginevents::emit(pluginevents::Event::ReaderOpen, openVars, 1);
}

void ReaderActivity::onExit() {
  // Never leak the reader's fast-turn weight into menus or other activities.
  GfxRenderer::setBwGlyphThreshold(2);
  GfxRenderer::setBwGlyphThreshold4(10);
  Activity::onExit();
  // Resume only after the reader has released its fonts/page cache: the service
  // must not compete with the teardown for PSRAM or the radio.
  jjwxc_cache_service::setReaderForeground(false);

  // Keep rebuildable font buffers from pinning the heap between reading sessions.
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->releaseSdFontCaches();
  }

  LOG_INF("MEM", "reader exit: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // Flush BEFORE the ReaderExit event: the session's final progress must be
  // durable before a subscriber can act on the exit notification.
  flushReaderSession();

  if (pluginevents::anySubscriber(pluginevents::Event::ReaderExit)) {
    char percent[8];
    snprintf(percent, sizeof(percent), "%d", getScreenshotInfo().progressPercent);
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()}, {"percent", percent}};
    pluginevents::emit(pluginevents::Event::ReaderExit, vars, 2);
  }

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);
}

void ReaderActivity::prepareForSleep() { flushReaderSession(); }

void ReaderActivity::flushReaderSession() {
  if (!readerSession.isEmitWorthy() || !pluginevents::anySubscriber(pluginevents::Event::ReaderSession)) {
    readerSession.reset();
    return;
  }

  const std::string document = KOReaderDocumentId::calculate(bookPath);
  const bool validDocument =
      document.size() == 32 && std::all_of(document.begin(), document.end(), [](const unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
      });
  if (validDocument) {
    char startTime[24];
    char endTime[24];
    char duration[16];
    char startProgress[8];
    char endProgress[8];
    snprintf(startTime, sizeof(startTime), "%lld", static_cast<long long>(readerSession.startTime()));
    snprintf(endTime, sizeof(endTime), "%lld", static_cast<long long>(readerSession.endTime()));
    snprintf(duration, sizeof(duration), "%lu", static_cast<unsigned long>(readerSession.durationSeconds()));
    snprintf(startProgress, sizeof(startProgress), "%u", readerSession.startProgressBp());
    snprintf(endProgress, sizeof(endProgress), "%u", readerSession.endProgressBp());
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()},       {"document", document.c_str()},
                                      {"start_time", startTime},        {"end_time", endTime},
                                      {"duration_seconds", duration},   {"start_progress_bp", startProgress},
                                      {"end_progress_bp", endProgress}, {"progress_scale", "10000"}};
    pluginevents::emit(pluginevents::Event::ReaderSession, vars, 8);
  }
  readerSession.reset();
}

bool ReaderActivity::handleBackNavigation() {
  return ReaderUtils::handleBackNavigation(mappedInput, activityManager, bookPath.c_str(),
                                           {this, [](void* ctx) { static_cast<ReaderActivity*>(ctx)->onGoHome(); }});
}

void ReaderActivity::clearEndOfBookOptionsIfNeeded() {
  if (isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire)) return;

  RenderLock lock(*this);
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
}

bool ReaderActivity::endOfBookMenuActive() const {
  return isAtEndOfBook() && endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive();
}

bool ReaderActivity::handleEndOfBookMenu(const bool suppressConfirmRelease) {
  if (suppressConfirmRelease || !endOfBookMenuActive()) {
    return false;
  }

  std::string openPath;
  switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(openPath);
      return true;
    case EndOfBookOptions::Action::GoHome:
      onGoHome();
      return true;
    case EndOfBookOptions::Action::LastPage:
      onReturnFromEndOfBook();
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::Redraw:
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::None:
      return false;
  }

  return false;
}

bool ReaderActivity::handleEndOfBookPageTurn(const bool prevTriggered, const bool nextTriggered) {
  if (!isAtEndOfBook()) return false;

  if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive()) {
    return true;
  }
  if (nextTriggered) {
    onGoHome();
  } else if (prevTriggered) {
    onReturnFromEndOfBook();
    requestUpdate();
  }
  return true;
}

void ReaderActivity::loop() {
  rememberBookOnceRendered();
  clearEndOfBookOptionsIfNeeded();
  if (handleEndOfBookMenu()) return;
  if (handleFormatInput()) return;
  if (handleBackNavigation()) return;

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  // Tap-zone actions dispatch before any page-turn handling. In the default
  // LEGACY layout the detection never sets these, so this is inert there. The
  // end-of-book screen owns its taps.
  if (!isAtEndOfBook()) {
    if (touch.longPress && touch.longAction != tapzones::LONG_NONE) {
      handleZoneLongAction(touch.longAction);
      requestUpdate();
      return;
    }
    if (touch.action != tapzones::NONE) {
      // An unsupported action is swallowed on purpose: a zone the user assigned
      // must never fall through to a page turn.
      handleZoneShortAction(touch.action);
      requestUpdate();
      return;
    }
  }
  auto [prevTriggered, nextTriggered, fromMotion] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) return;
  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) return;

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool skip =
      !fromMotion && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP && heldMs >= ReaderUtils::SKIP_HOLD_MS;

  if (prevTriggered) {
    if (skip) {
      const bool succeeded = skipPages(-10);
      notePageTurn(false, succeeded);
    } else {
      const bool succeeded = pageTurn(false);
      notePageTurn(false, succeeded);
    }
  } else {
    if (skip) {
      // A skip is navigation, not reading: it never counts toward session dwell.
      const bool succeeded = skipPages(10);
      notePageTurn(false, succeeded);
    } else {
      const bool succeeded = pageTurn(true);
      notePageTurn(true, succeeded);
    }
  }
  requestUpdate();
}

bool ReaderActivity::handleZoneShortAction(const uint8_t action) {
  switch (action) {
    case tapzones::MENU:
      return openZoneMenu();
    case tapzones::GO_HOME:
      onGoHome();
      return true;
    case tapzones::REFRESH:
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      requestUpdate();
      return true;
    case tapzones::ROTATE_CW:
      rotateZoneOrientation(1);
      return true;
    case tapzones::ROTATE_CCW:
      rotateZoneOrientation(-1);
      return true;
    default:
      return false;
  }
}

bool ReaderActivity::handleZoneLongAction(const uint8_t action) {
  switch (action) {
    case tapzones::LONG_MENU:
      return openZoneMenu();
    case tapzones::LONG_GO_HOME:
      onGoHome();
      return true;
    case tapzones::LONG_REFRESH:
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      requestUpdate();
      return true;
    case tapzones::LONG_ROTATE_CW:
      rotateZoneOrientation(1);
      return true;
    case tapzones::LONG_ROTATE_CCW:
      rotateZoneOrientation(-1);
      return true;
    default:
      return false;
  }
}

void ReaderActivity::rotateZoneOrientation(const int delta) {
  const uint8_t next = static_cast<uint8_t>((SETTINGS.orientation + delta + CrossPointSettings::ORIENTATION_COUNT) %
                                            CrossPointSettings::ORIENTATION_COUNT);
  if (next == SETTINGS.orientation) return;
  SETTINGS.orientation = next;
  SETTINGS.saveToFile();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
}

void ReaderActivity::render(RenderLock&&) {
  if (isAtEndOfBook()) {
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) LOG_ERR("READER", "OOM: EndOfBookOptions");
    }
    renderer.clearScreen();
    if (endOfBookOptions) {
      endOfBookOptions->loadOnce(bookPath);
      // Release-publish AFTER loadOnce() so the main task's acquire load can't
      // observe an object whose names/selector are still being populated.
      endOfBookOptionsReady.store(true, std::memory_order_release);
      endOfBookOptions->render(renderer, mappedInput);
    }
    renderer.displayBuffer();
    onEndOfBookRendered();
    markPageRendered();
    readerSession.onRenderComplete(millis(), trustedtime::trustedNow(), getProgressBasisPoints());
    return;
  }

  renderBook();
  readerSession.onRenderComplete(millis(), trustedtime::trustedNow(), getProgressBasisPoints());
}

bool ReaderActivity::handleForcedRefresh() {
  {
    RenderLock lock(*this);
    pagesUntilFullRefresh = 1;
    forcedRefreshPending = true;
  }
  requestUpdate();
  return true;
}
