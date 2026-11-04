#include "WebBrowserActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <InflateReader.h>
#include <WiFi.h>
#include <WeReadStore.h>
#include <Memory.h>
#include <TxtEncoding.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#if !defined(SIMULATOR)
#include <esp_sntp.h>
#endif
#include <SecureHttpClient.h>
#include <WeReadHttpClient.h>
#include <WebCharset.h>
#include <WebUrl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <netdb.h>
#include <string_view>
#include <strings.h>
#include <utility>
#include <vector>

#include "BackgroundWork.h"
#include "CrossPointSettings.h"
#include "components/StatusCards.h"
#include "components/icons/listIcons.h"
#include "WifiCredentialStore.h"
#include "SdCardFontSystem.h"
#include "SystemFontPack.h"
#include "NetworkStartup.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiPowerSaveGuard.h"

namespace {

constexpr size_t kMaxBodyBytes = 1024 * 1024;       // 1 MiB of markup per page
constexpr size_t kBodyCapacity = 2 * 1024 * 1024;   // GBK -> UTF-8 expansion + headroom
constexpr int kMaxRedirects = 5;
constexpr int kRequestTimeoutMs = 60000;
constexpr int kFirstAttemptTimeoutMs = 90000;
constexpr uint32_t kJobStackPsramDepth = 24 * 1024;  // words -> 96 KB in PSRAM
constexpr uint32_t kJobStackBytes = 16 * 1024;       // internal fallback (kernel scales by 4)
constexpr uint32_t kJobStopTimeoutMs = 35000;
constexpr size_t kMaxHistory = 32;

constexpr const char* kUserAgent =
    "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile "
    "Safari/537.36";
constexpr const char* kAccept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
constexpr const char* kAcceptLanguage = "zh-CN,zh;q=0.9,en;q=0.6";

// 12pt GB2312 bitmap: the reader's default size, and its glyph advances are
// bitmap lookups. The vector UI face costs ~34 ms per whole-line measurement,
// which made a 200-line chapter take 8 s to paginate.
constexpr int kBodyFont = CJK_UI_12_FONT_ID;
constexpr int kSmallFont = SMALL_FONT_ID;
constexpr int kSidePadding = 28;
constexpr int kWrapSafety = 12;  // absorb kerning/rounding the model ignores
constexpr int kFooterHeight = 36;  // our own hint bar; CPY's metric can be 0

bool equalsNoCase(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

// Byte offset of the raw DEFLATE payload inside a gzip member, or 0 when the
// header is malformed. uzlib decodes raw DEFLATE; the gzip CRC/ISIZE trailer is
// ignored (we never reuse the bytes).
size_t gzipPayloadOffset(const uint8_t* data, const size_t length) {
  if (length < 18 || data[0] != 0x1F || data[1] != 0x8B || data[2] != 8) return 0;
  const uint8_t flags = data[3];
  size_t offset = 10;
  if (flags & 0x04) {  // FEXTRA
    if (offset + 2 > length) return 0;
    const size_t extra = static_cast<size_t>(data[offset]) | (static_cast<size_t>(data[offset + 1]) << 8);
    offset += 2 + extra;
  }
  if (flags & 0x08) {  // FNAME
    while (offset < length && data[offset] != 0) ++offset;
    ++offset;
  }
  if (flags & 0x10) {  // FCOMMENT
    while (offset < length && data[offset] != 0) ++offset;
    ++offset;
  }
  if (flags & 0x02) offset += 2;  // FHCRC
  if (offset >= length) return 0;
  return offset;
}

std::string formatVisitedAt(const uint32_t when) {
  if (when < 1735689600u) return "\xE2\x80\x94";  // em dash: the clock was never set
  time_t seconds = static_cast<time_t>(when);
  struct tm local{};
  localtime_r(&seconds, &local);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &local);
  return buffer;
}

std::string trimTo(const std::string& value, size_t maxBytes) {
  if (value.size() <= maxBytes) return value;
  size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
  return value.substr(0, end);
}

}  // namespace

char WebBrowserActivity::pendingOpenUrl_[512] = {};
bool WebBrowserActivity::pendingFixture_ = false;

void WebBrowserActivity::requestOpen(const char* url) {
  pendingOpenUrl_[0] = '\0';
  pendingFixture_ = false;
  if (url == nullptr || url[0] == '\0') return;
  snprintf(pendingOpenUrl_, sizeof(pendingOpenUrl_), "%s", url);
}

void WebBrowserActivity::requestFixture() {
  pendingOpenUrl_[0] = '\0';
  pendingFixture_ = true;
}

void WebBrowserActivity::debugBrowserPage(const int delta) { pageTurn(delta); }

void WebBrowserActivity::debugBrowserLinks() { openLinkPicker(); }

void WebBrowserActivity::debugBrowserBack() { goBack(); }

void WebBrowserActivity::loadFixtureDocument() {
  // A fetch may still be in flight (the page is being replaced by a sample); let
  // it unwind but never apply its result over the fixture.
  if (jobTask_ != nullptr && !jobDone_.load(std::memory_order_acquire)) {
    jobAbandoned_ = true;
    jobCancel_.store(true, std::memory_order_relaxed);
  }
  static constexpr char kFixture[] =
      "<html><head><meta charset=utf-8><title>\xE7\xA6\xBB\xE7\xBA\xBF\xE6\xA0\xB7\xE4\xBE\x8B</title>"
      "</head><body>"
      "<h2>\xE7\xAC\xAC\xE4\xB8\x80\xE7\xAB\xA0 \xE7\xA6\xBB\xE7\xBA\xBF\xE9\xAA\x8C\xE8\xAF\x81</h2>"
      "<p>\xE8\xBF\x99\xE6\x98\xAF\xE4\xB8\x80\xE6\xAE\xB5\xE7\x94\xA8\xE4\xBA\x8E\xE7\xA6\xBB\xE7\xBA\xBF\xE6\x8E\x92\xE7\x89\x88\xE9\xAA\x8C\xE8\xAF\x81\xE7\x9A\x84\xE4\xB8\xAD\xE6\x96\x87\xE6\xAD\xA3\xE6\x96\x87\xEF\xBC\x8C"
      "\xE5\x8C\x85\xE5\x90\xAB\xE5\xB8\xB8\xE8\xA7\x81\xE6\xA0\x87\xE7\x82\xB9\xE3\x80\x81\xE6\x95\xB0\xE5\xAD\x97\xE5\x92\x8C\xE8\x8B\xB1\xE6\x96\x87\xE6\xB7\xB7\xE6\x8E\x92\xE3\x80\x82"
      "Mixed English and 123 numbers wrap on the same page.</p>"
      "<p><a href=\"https://example.com/fixture-next\">\xE4\xB8\x8B\xE4\xB8\x80\xE7\xAB\xA0</a>"
      " <a href=\"https://example.com/fixture-toc\">\xE7\x9B\xAE\xE5\xBD\x95</a></p>"
      "<p>\xE7\xAC\xAC\xE4\xBA\x8C\xE6\xAE\xB5\xE3\x80\x82\xE8\xBF\x99\xE9\x87\x8C\xE9\xAA\x8C\xE8\xAF\x81\xE9\x95\xBF\xE6\xAE\xB5\xE8\x90\xBD\xE5\x9C\xA8\xE4\xB8\x8B\xE4\xB8\x80\xE9\xA1\xB5\xE3\x80\x82</p>"
      "</body></html>";
  webbrowser::ExtractOptions options;
  LOG_INF(kTag, "loading offline fixture");
  {
    const RenderLock lock;
    incomingDocument_.clear();
    if (!webbrowser::extractDocument(std::string_view(kFixture, sizeof(kFixture) - 1),
                                     "https://example.invalid/fixture", incomingDocument_, options)) {
      incomingDocument_.clear();
      return;
    }
    document_ = std::move(incomingDocument_);
    currentUrl_ = "https://example.invalid/fixture";
    pendingUrl_ = currentUrl_;
    pageIndex_ = 0;
    bodyFont_ = CJK_READER_SIZE_FONT_ID(SETTINGS.fontPointSize);
    if (renderer.getLineHeight(bodyFont_) <= 0) bodyFont_ = CJK_UI_12_FONT_ID;
    paginateDocument();
    state_ = pagination_.lines.empty() ? State::Error : State::Page;
    if (state_ == State::Error) errorText_ = tr(STR_BROWSER_ERR_EMPTY);
  }
  requestUpdate();
}

void WebBrowserActivity::debugBrowserToggleBookmark() {
  if (state_ != State::Page || currentUrl_.empty()) return;
  webbrowser::toggleBookmark(bookmarks_, currentPlace());
  bookmarksDirty_ = true;
  const bool saved = webbrowser::findBookmark(bookmarks_, currentUrl_) >= 0;
  LOG_INF(kTag, "bookmark %s: %s (%u total)", saved ? "added" : "removed", currentUrl_.c_str(),
          static_cast<unsigned>(bookmarks_.size()));
  showNotice(saved ? tr(STR_BROWSER_ADDED) : tr(STR_BROWSER_REMOVED));
}

void WebBrowserActivity::debugBrowserBookmarks() {
  LOG_INF(kTag, "bookmarks=%u", static_cast<unsigned>(bookmarks_.size()));
  for (size_t i = 0; i < bookmarks_.size(); ++i) {
    LOG_INF(kTag, "  [%u] %s | %s p%u", static_cast<unsigned>(i), bookmarks_[i].url.c_str(),
            bookmarks_[i].title.c_str(), static_cast<unsigned>(bookmarks_[i].page));
  }
}

void WebBrowserActivity::debugBrowserOpen(const char* url) {
  if (url == nullptr || url[0] == '\0') return;
  navigateTo(url, true);
}

void WebBrowserActivity::debugBrowserFollow(const int index) {
  std::vector<std::string> labels;
  std::vector<std::string> urls;
  gatherPageLinks(labels, urls);
  if (index < 0 || index >= static_cast<int>(urls.size())) {
    LOG_ERR(kTag, "follow: no link %d (page has %u)", index, static_cast<unsigned>(urls.size()));
    return;
  }
  LOG_INF(kTag, "follow %d: %s", index, urls[index].c_str());
  followLink(urls[index]);
}

void WebBrowserActivity::debugBrowserMenu() {
  if (state_ != State::Page) return;
  LOG_INF(kTag, "serial menu");
  openMenu();
}

void WebBrowserActivity::debugBrowserList(const int which) {
  if (state_ != State::Page && state_ != State::Home) return;
  LOG_INF(kTag, "serial list %s", which == 0 ? "bookmarks" : "history");
  enterList(which == 0 ? State::Bookmarks : State::History);
}

void WebBrowserActivity::debugBrowserOpenPlace(const int which, const int index) {
  auto& list = which == 0 ? bookmarks_ : visits_;
  if (index < 0 || index >= static_cast<int>(list.size())) {
    LOG_ERR(kTag, "open place: no %s[%d] (%u entries)", which == 0 ? "bookmark" : "history", index,
            static_cast<unsigned>(list.size()));
    return;
  }
  const webbrowser::Place place = list[static_cast<size_t>(index)];
  LOG_INF(kTag, "open place %s[%d]: %s p%u", which == 0 ? "bookmark" : "history", index, place.url.c_str(),
          static_cast<unsigned>(place.page));
  openPlace(place);
}

void WebBrowserActivity::debugBrowserStatus() {
  LOG_INF(kTag, "status state=%d page=%u/%u blocks=%u links=%u text=%u bm=%u hist=%u stack=%u url=%.90s",
          static_cast<int>(state_), static_cast<unsigned>(pageIndex_ + 1),
          static_cast<unsigned>(pagination_.pages.size()), static_cast<unsigned>(document_.blocks.size()),
          static_cast<unsigned>(document_.links.size()), static_cast<unsigned>(document_.text.size()),
          static_cast<unsigned>(bookmarks_.size()), static_cast<unsigned>(visits_.size()),
          static_cast<unsigned>(history_.size()), currentUrl_.c_str());
}

void WebBrowserActivity::onEnter() {
  Activity::onEnter();
  // Pagination runs on the UI task (pollJob), outside render(): fix the logical
  // frame here too or it measures in whatever orientation the previous
  // activity left behind and the lines no longer fit the portrait screen.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  NetworkStartup::prepare(renderer);
  state_ = State::Home;
  errorText_.clear();
  pendingUrl_.clear();
  document_.clear();
  pagination_.clear();
  history_.clear();
  pageIndex_ = 0;
  optionPopupClosing_ = false;
  loadPlaces();
  entryFrame_ = true;
  waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
  waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
  if (pendingFixture_) {
    pendingFixture_ = false;
    loadFixtureDocument();
  } else if (pendingOpenUrl_[0] != '\0') {
    const std::string url = pendingOpenUrl_;
    pendingOpenUrl_[0] = '\0';
    navigateTo(url, true);
  }
}

void WebBrowserActivity::onExit() {
  // Leaving the app (Back on the first URL, Home from the menu, or the system
  // taking the foreground) still remembers where the reader stopped.
  if (state_ == State::Page && !currentUrl_.empty()) {
    webbrowser::Place place = currentPlace();
    place.page = static_cast<uint16_t>(pageIndex_);
    webbrowser::recordHistory(visits_, place);
    historyDirty_ = true;
  }
  if (historyDirty_) saveHistory();
  if (bookmarksDirty_) saveBookmarks();
  jobCancel_.store(true, std::memory_order_relaxed);
  const uint32_t deadline = millis() + kJobStopTimeoutMs;
  while (jobTask_ != nullptr && !jobDone_.load(std::memory_order_acquire) &&
         static_cast<int32_t>(deadline - millis()) > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (jobTask_ != nullptr && !jobDone_.load(std::memory_order_acquire)) {
    // TLS connect cannot be interrupted from here; deleting a task mid-call is
    // the least-bad option, and it must match the creation call or the stack
    // leaks. The request timeout bounds how long this can take in practice.
    LOG_ERR(kTag, "fetch did not stop in time; deleting it");
    reapJob();
  } else if (jobTask_ != nullptr) {
    reapJob();
  }
  // Drop TLS while the radio is still up (JjwxcActivity does the same): tearing
  // the Wi-Fi driver down first left wolfSSL cleaning up a dead socket and
  // panicked with InstrFetchProhibited on the next activity switch.
  if (wifiSessionActive_ && WiFi.getMode() != WIFI_MODE_NULL) {
    NetworkStartup::releaseRadio();
  }
  wifiSessionActive_ = false;
  resumeTimeSync();
  hasQueuedNav_ = false;
  queuedNavUrl_.clear();
  jobAbandoned_ = false;
  document_.clear();
  incomingDocument_.clear();
  pagination_.clear();
  history_.clear();
  Activity::onExit();
}

void WebBrowserActivity::prepareForSleep() { jobCancel_.store(true, std::memory_order_relaxed); }

void WebBrowserActivity::loop() {
  pollJob();

  if (hasQueuedNav_ && jobTask_ == nullptr) {
    const std::string url = queuedNavUrl_;
    const bool addHistory = queuedNavAddHistory_;
    hasQueuedNav_ = false;
    queuedNavUrl_.clear();
    if (addHistory) {
      const RenderLock lock;
      if (history_.empty() || std::string(history_.back().c_str()) != url) {
        history_.push_back(webbrowser::WebString(url.data(), url.size()));
        if (history_.size() > kMaxHistory) history_.erase(history_.begin());
      }
    }
    beginFetch(url);
  }

  if (cancelDeadlineMs_ != 0 && static_cast<int32_t>(millis() - cancelDeadlineMs_) >= 0) {
    cancelDeadlineMs_ = 0;
    abandonJob(/*toPage=*/true, nullptr);
  }
  if (state_ == State::Loading && jobTask_ != nullptr && loadingDeadlineMs_ != 0 &&
      static_cast<int32_t>(millis() - loadingDeadlineMs_) >= 0) {
    loadingDeadlineMs_ = 0;
    abandonJob(/*toPage=*/false, tr(STR_BROWSER_ERR_TIMEOUT));
  }
  if (historyDirty_) {
    historyDirty_ = false;
    saveHistory();
  }
  if (bookmarksDirty_) {
    bookmarksDirty_ = false;
    saveBookmarks();
  }
  if (state_ == State::Loading && jobTask_ != nullptr &&
      static_cast<int32_t>(millis() - lastLoadingTickMs_) >= 1000) {
    lastLoadingTickMs_ = millis();
    requestUpdate();
  }

  if (noticeUntilMs_ != 0 && static_cast<int32_t>(millis() - noticeUntilMs_) >= 0) {
    noticeUntilMs_ = 0;
    notice_.clear();
    requestUpdate();
  }

  if (entryFrame_) {
    entryFrame_ = false;
    return;
  }
  if (waitForBackRelease_ || waitForConfirmRelease_) {
    waitForBackRelease_ = waitForBackRelease_ && mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = waitForConfirmRelease_ && mappedInput.isPressed(MappedInputManager::Button::Confirm);
    return;
  }

  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) {
    optionPopupClosing_ = !optionPopup_.isActive();
    return;
  }
  if (optionPopupClosing_) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return;
    }
    optionPopupClosing_ = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }

  if (state_ == State::Loading) {
    int tx = 0;
    int ty = 0;
    if (loadingCancelRect_.width > 0 && mappedInput.wasScreenTapped(tx, ty) && tx >= loadingCancelRect_.x &&
        tx < loadingCancelRect_.x + loadingCancelRect_.width && ty >= loadingCancelRect_.y &&
        ty < loadingCancelRect_.y + loadingCancelRect_.height) {
      requestCancel();
    }
    return;
  }

  auto inside = [](const Rect& r, const int x, const int y) {
    return r.width > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
  };

  if (state_ == State::Home) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openAddressBar();
      return;
    }
    int tx = 0;
    int ty = 0;
    if (!mappedInput.wasScreenTapped(tx, ty)) return;
    if (inside(homeAddressRect_, tx, ty)) {
      openAddressBar();
      return;
    }
    if (inside(homeBookmarksRect_, tx, ty)) {
      enterList(State::Bookmarks);
      return;
    }
    if (inside(homeHistoryRect_, tx, ty)) {
      enterList(State::History);
      return;
    }
    for (size_t i = 0; i < homeRecentRects_.size() && i < visits_.size(); ++i) {
      if (inside(homeRecentRects_[i], tx, ty)) {
        openPlace(visits_[i]);
        return;
      }
    }
    return;
  }

  if (state_ == State::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!pendingUrl_.empty()) navigateTo(pendingUrl_, false);
      return;
    }
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      if (inside(errorRetryRect_, tx, ty)) {
        if (!pendingUrl_.empty()) navigateTo(pendingUrl_, false);
        return;
      }
      if (inside(errorBackRect_, tx, ty)) {
        goBack();
        return;
      }
    }
    return;
  }

  if (state_ == State::Bookmarks || state_ == State::History) {
    const auto& list = activeList();
    const int count = static_cast<int>(list.size());
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      openListMenu();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (count > 0 && listIndex_ > 0) {
        --listIndex_;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
      if (count > 0 && listIndex_ + 1 < count) {
        ++listIndex_;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (count > 0) openPlace(list[static_cast<size_t>(listIndex_)]);
      return;
    }
    int tx = 0;
    int ty = 0;
    if (!mappedInput.wasScreenTapped(tx, ty)) return;
    if (inside(listMenuRect_, tx, ty)) {
      openListMenu();
      return;
    }
    for (size_t i = 0; i < listTrashRects_.size(); ++i) {
      if (inside(listTrashRects_[i], tx, ty)) {
        confirmDeleteAt(static_cast<int>(i) + listScroll_);
        return;
      }
    }
    for (size_t i = 0; i < listRowRects_.size(); ++i) {
      if (inside(listRowRects_[i], tx, ty)) {
        const int index = static_cast<int>(i) + listScroll_;
        if (index < count) openPlace(list[static_cast<size_t>(index)]);
        return;
      }
    }
    return;
  }

  if (state_ != State::Page) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    pageTurn(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    pageTurn(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openMenu();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openLinkPicker();
    return;
  }

  int tx = 0;
  int ty = 0;
  if (!mappedInput.wasScreenTapped(tx, ty)) return;
  const Rect menuHit{pageMenuRect_.x - 8, pageMenuRect_.y - 10, pageMenuRect_.width + 16,
                     pageMenuRect_.height + 20};
  if (inside(menuHit, tx, ty)) {
    openMenu();
    return;
  }
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  for (const auto& hit : visibleLinks_) {
    if (tx >= hit.x && tx < hit.x + hit.width && ty >= hit.y && ty < hit.y + hit.height) {
      const auto& link = document_.links[hit.linkIndex];
      const std::string url(document_.hrefs.data() + link.hrefStart, link.hrefLength);
      followLink(url);
      return;
    }
  }
  // Taps in the bottom chrome band belong to that band, never to page turn.
  if (ty >= height - kFooterHeight - 60) return;
  if (tx < width * 22 / 100) {
    pageTurn(-1);
  } else if (tx > width * 78 / 100) {
    pageTurn(1);
  } else {
    openMenu();
  }
}

bool WebBrowserActivity::trySavedNetwork() {
  // Association and DHCP run in this thread; keep the CPU at 240 MHz for the
  // whole window (the idle governor otherwise drops to 80 MHz after ~3 s).
  HalPowerManager::Lock powerLock(/*quiet=*/true);
  WIFI_STORE.loadFromFile();
  const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential.has_value()) {
    LOG_INF(kTag, "no saved network to connect silently");
    return false;
  }
  LOG_INF(kTag, "connecting saved network %.32s", credential->ssid.c_str());
  // setMode, not WiFi.mode: it stops BLE and reclaims render memory first.
  // Silent-connecting without it left the wolfSSL handshake short of a
  // contiguous internal block (measured: largest 9.2 KB vs 12.2 KB).
  if (!NetworkStartup::setMode(renderer, WIFI_STA)) return false;
  WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  // 15 s: associations on this device have been seen to take 1.4 s warm and
  // longer after a cold radio start; the wait card stays under the 20 s slow
  // hint either way.
  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(deadline - millis()) > 0) {
    delay(100);
  }
  const bool connected = WiFi.status() == WL_CONNECTED;
  LOG_INF(kTag, "saved network connect %s", connected ? "ok" : "failed");
  if (connected) wifiSessionActive_ = true;
  return connected;
}

webbrowser::Place WebBrowserActivity::currentPlace() const {
  webbrowser::Place place;
  place.url = webbrowser::WebString(currentUrl_.data(), currentUrl_.size());
  place.title = document_.title.empty() ? place.url : document_.title;
  place.visitedAt = static_cast<uint32_t>(time(nullptr));
  place.page = static_cast<uint16_t>(pageIndex_);
  return place;
}

namespace {
constexpr const char* kBookmarksPath = "/.crosspoint/browser/bookmarks.txt";
constexpr const char* kHistoryPath = "/.crosspoint/browser/history.txt";
constexpr size_t kPlacesFileCap = 64 * 1024;
}  // namespace

void WebBrowserActivity::loadPlaces() {
  bookmarks_.clear();
  visits_.clear();
  std::string raw;
  if (Storage.readFileToString("WBRS", kBookmarksPath, kPlacesFileCap, raw)) {
    webbrowser::deserializePlaces(raw, bookmarks_);
  }
  raw.clear();
  if (Storage.readFileToString("WBRS", kHistoryPath, kPlacesFileCap, raw)) {
    webbrowser::deserializePlaces(raw, visits_);
  }
  LOG_INF(kTag, "places loaded: bookmarks=%u history=%u", static_cast<unsigned>(bookmarks_.size()),
          static_cast<unsigned>(visits_.size()));
}

void WebBrowserActivity::saveBookmarks() {
  const webbrowser::WebString text = webbrowser::serializePlaces(bookmarks_);
  Storage.mkdir("/.crosspoint/browser");
  const std::string raw(text.c_str(), text.size());
  const std::string tmp = std::string(kBookmarksPath) + ".tmp";
  if (!Storage.writeFile(tmp.c_str(), String(raw.c_str()))) return;
  WeReadStore::atomicReplace(tmp, kBookmarksPath);
}

void WebBrowserActivity::saveHistory() {
  const webbrowser::WebString text = webbrowser::serializePlaces(visits_);
  Storage.mkdir("/.crosspoint/browser");
  const std::string raw(text.c_str(), text.size());
  const std::string tmp = std::string(kHistoryPath) + ".tmp";
  if (!Storage.writeFile(tmp.c_str(), String(raw.c_str()))) return;
  WeReadStore::atomicReplace(tmp, kHistoryPath);
}

void WebBrowserActivity::openAddressBar() {
  auto handler = [this](const ActivityResult& result) {
    entryFrame_ = true;
    waitForBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
    waitForConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (result.isCancelled) return;
    const auto* keyboard = std::get_if<KeyboardResult>(&result.data);
    if (keyboard == nullptr || keyboard->text.empty()) return;
    navigateTo(keyboard->text, true);
  };
  startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_BROWSER_ADDRESS_PROMPT), currentUrl_, 512,
                                                    InputType::Url);
}

void WebBrowserActivity::navigateTo(const std::string& input, const bool addHistory) {
  std::string normalized;
  bool upgraded = false;
  if (!webbrowser::normalizeHttpUrl(input, normalized, upgraded)) {
    showNotice(tr(STR_BROWSER_BAD_URL));
    return;
  }
  if (upgraded) {
    LOG_INF(kTag, "upgraded http:// to https:// (%.80s)", normalized.c_str());
    showNotice(tr(STR_BROWSER_UPGRADED));
  } else if (normalized.rfind("http://", 0) == 0) {
    LOG_INF(kTag, "plain HTTP (unencrypted): %.80s", normalized.c_str());
    showNotice(tr(STR_BROWSER_INSECURE_HTTP));
  }
  // Navigating away is leaving the current page: keep its last read page.
  if (state_ == State::Page && !currentUrl_.empty() && currentUrl_ != normalized) {
    updateHistoryPage();
  }
  // Arduino's NetworkManager clears the DNS cache on the first resolve after an
  // association. If SNTP has a DNS query pending, that callback runs on our
  // thread and udp_new_ip_type() asserts (lwIP core locking). Pause time sync
  // for the browsing session so no NTP lookup can be in flight while the
  // browser resolves hosts.
  suspendTimeSync();
  const bool busy = jobTask_ != nullptr && !jobDone_.load(std::memory_order_acquire);
  {
    // The render task reads currentUrl_/pendingUrl_ and the history is a PSRAM
    // container; publish both under the same lock the page is drawn with.
    const RenderLock lock;
    currentUrl_ = normalized;
    pendingUrl_ = normalized;
    if (addHistory && !busy) {
      if (history_.empty() || std::string(history_.back().c_str()) != normalized) {
        history_.push_back(webbrowser::WebString(normalized.data(), normalized.size()));
        if (history_.size() > kMaxHistory) history_.erase(history_.begin());
      }
    }
  }

  if (busy) {
    // Supersede the in-flight request instead of refusing with "busy". The old
    // worker is told to abort; the new URL starts the moment it has unwound, so
    // a cancel followed by another link never dead-ends.
    jobAbandoned_ = true;
    jobCancel_.store(true, std::memory_order_relaxed);
    hasQueuedNav_ = true;
    queuedNavUrl_ = normalized;
    queuedNavAddHistory_ = addHistory;
    cancelDeadlineMs_ = 0;
    const RenderLock lock;
    state_ = State::Loading;
    connectingWifi_ = false;
    loadingStartedMs_ = millis();
    lastLoadingTickMs_ = loadingStartedMs_;
    jobStage_.store(1, std::memory_order_relaxed);
    jobPermille_.store(-1, std::memory_order_relaxed);
    jobBytes_.store(0, std::memory_order_relaxed);
    jobTotal_.store(0, std::memory_order_relaxed);
    noticeUntilMs_ = 0;
    notice_.clear();
    requestUpdate();
    return;
  }

  if (WeReadHttpClient::networkReady()) {
    beginFetch(normalized);
    return;
  }

  // Connect to the last saved network without a sheet: the browser's job is to
  // show a page, not to make the user pick a network it already knows. A
  // failure falls back to the selection sheet below.
  {
    const RenderLock lock;
    state_ = State::Loading;
    pendingUrl_ = normalized;
    loadingStartedMs_ = millis();
    lastLoadingTickMs_ = loadingStartedMs_;
    connectingWifi_ = true;
    jobStage_.store(0, std::memory_order_relaxed);
  }
  requestUpdateAndWait();
  if (trySavedNetwork()) {
    connectingWifi_ = false;
    beginFetch(normalized);
    return;
  }
  connectingWifi_ = false;
  // We brought the radio up for this attempt; do not leave it on for an
  // association that failed.
  NetworkStartup::releaseRadio();
  // The browser never opens the Wi-Fi UI: one reason, one way out. The user
  // connects from Settings and comes back, and Back here returns to the page
  // that was already loaded (or exits when this was the first URL).
  state_ = State::Error;
  errorText_ = tr(STR_BROWSER_NO_WIFI);
  requestUpdate();
}

void WebBrowserActivity::jobTrampoline(void* param) {
  auto* self = static_cast<WebBrowserActivity*>(param);
  {
    // The standby overlay must not light-sleep while a TLS handshake and a
    // 1 MiB parse are in flight.
    background_work::Guard guard;
    self->runFetch(self->jobUrl_);
  }
  self->jobDone_.store(true, std::memory_order_release);
  vTaskSuspend(nullptr);
}

void WebBrowserActivity::beginFetch(const std::string& url) {
  if (jobTask_ != nullptr) {
    // A cancelled request whose TLS connect is still unwinding; do not start a
    // second worker on top of it.
    if (jobDone_.load(std::memory_order_acquire)) {
      pollJob();
    } else {
      showNotice(tr(STR_BROWSER_BUSY));
      return;
    }
    if (jobTask_ != nullptr) return;
  }
  // A fresh association leaves the internal heap thin; reclaim the font caches
  // (TTF arenas included) before the handshake asks for its buffers.
  NetworkStartup::prepare(renderer);
  {
    const RenderLock lock;
    if (auto* fontCache = renderer.getFontCacheManager()) fontCache->releaseSdFontCaches();
  }
  jobUrl_ = url;
  jobDone_.store(false, std::memory_order_relaxed);
  jobCancel_.store(false, std::memory_order_relaxed);
  jobResult_ = JobResult::Network;
  jobHttpStatus_ = 0;
  {
    // Keep document_/pagination_ as they are: Back while loading must return to
    // the page the user was reading, not to the home screen.
    const RenderLock lock;
    state_ = State::Loading;
    loadingStartedMs_ = millis();
    lastLoadingTickMs_ = millis();
    jobStage_.store(1, std::memory_order_relaxed);
  }
  requestUpdate();

  BaseType_t ok = pdFAIL;
#ifdef WEBBROWSER_ALLOCATOR_HAS_HEAP_CAPS
  ok = xTaskCreatePinnedToCoreWithCaps(jobTrampoline, "webFetch", kJobStackPsramDepth, this, tskIDLE_PRIORITY,
                                       &jobTask_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  jobStackInPsram_ = ok == pdPASS;
  if (ok != pdPASS) LOG_ERR(kTag, "PSRAM job stack refused (%d); using internal RAM", static_cast<int>(ok));
#endif
  if (ok != pdPASS) {
    jobStackInPsram_ = false;
    ok = xTaskCreatePinnedToCore(jobTrampoline, "webFetch", kJobStackBytes, this, tskIDLE_PRIORITY, &jobTask_, 0);
  }
  if (ok != pdPASS) {
    jobTask_ = nullptr;
    state_ = State::Error;
    errorText_ = tr(STR_BROWSER_ERR_JOB);
    requestUpdate();
    return;
  }
  LOG_INF(kTag, "fetch task up: stack in %s, free=%u largest=%u", jobStackInPsram_ ? "PSRAM" : "INTERNAL RAM",
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void WebBrowserActivity::runFetch(std::string url) {
  jobResult_ = JobResult::Network;
  jobHttpStatus_ = 0;
  jobContentType_.clear();
  jobAbandoned_ = false;
  cancelDeadlineMs_ = 0;
  // Large pages on a slow link can legitimately take minutes; the HTTP client
  // still aborts after 45 s without a single byte, and Cancel is always live.
  loadingDeadlineMs_ = millis() + 240000;
  jobPermille_.store(-1, std::memory_order_relaxed);
  jobStage_.store(1, std::memory_order_relaxed);
  jobBytes_.store(0, std::memory_order_relaxed);
  jobTotal_.store(0, std::memory_order_relaxed);
  jobLastByteMs_.store(0, std::memory_order_relaxed);
  incomingDocument_.clear();

  bool bodyInPsram = false;
  uint8_t* body = nullptr;
#ifdef WEBBROWSER_ALLOCATOR_HAS_HEAP_CAPS
  body = static_cast<uint8_t*>(heap_caps_malloc(kBodyCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  bodyInPsram = body != nullptr;
#endif
  if (body == nullptr) body = static_cast<uint8_t*>(malloc(kBodyCapacity));
  if (body == nullptr) {
    LOG_ERR(kTag, "no %u-byte fetch buffer", static_cast<unsigned>(kBodyCapacity));
    jobResult_ = JobResult::Oversize;
    return;
  }

  size_t bodyLength = 0;
  bool truncated = false;
  std::string contentType;
  std::string contentEncoding;
  std::string location;

  {
    // A fresh connection per job, exactly like JjwxcHttpTransport::resetSession()
    // before startJob(); reusing a stale keep-alive is a separate failure mode.
    // Give lwIP/DHCP a moment after silent association; the JJWXC path always
    // had the Wi-Fi sheet in front of it.
    vTaskDelay(pdMS_TO_TICKS(1000));
    const WifiPowerSaveGuard powerGuard;
    // DNS, the TLS handshake and the body copy all run here; without this the
    // CPU drops to 80 MHz after a few idle seconds and a page that takes 1.5 s
    // on the host takes tens of seconds on the device (observed with 52shuku).
    HalPowerManager::Lock powerLock(/*quiet=*/true);
    for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
      if (jobCancel_.load(std::memory_order_relaxed)) {
        jobResult_ = JobResult::Canceled;
        break;
      }
      bodyLength = 0;
      truncated = false;
      contentType.clear();
      contentEncoding.clear();
      location.clear();
      jobPermille_.store(-1, std::memory_order_relaxed);
      jobBytes_.store(0, std::memory_order_relaxed);
      jobTotal_.store(0, std::memory_order_relaxed);
      jobLastByteMs_.store(0, std::memory_order_relaxed);

      // SecureHttpClient speaks both schemes: https through wolfSSL and plain
      // http through the WiFiClient transport. It also owns header framing
      // (Content-Length, chunked, close-delimited) and keep-alive, so the
      // browser no longer needs an HTTPS-only client.
      freeink::SecureHttpClient http;
      http.setInsecure();
      http.setTimeout(static_cast<uint32_t>(kFirstAttemptTimeoutMs));
      http.setUserAgent(kUserAgent);
      http.setFollowRedirects(0);
      // Close each request instead of keeping a socket alive: Cloudflare-fronted
      // hosts (52shuku) would otherwise leave a half-open connection that the
      // next read sees as closed, and a fresh request is cheaper than a tarpit.
      http.setReuse(false);
      if (!http.begin(url)) {
        LOG_ERR(kTag, "unfetchable URL %.70s", url.c_str());
        jobResult_ = JobResult::Http;
        break;
      }
      http.addHeader("Accept", kAccept);
      http.addHeader("Accept-Language", kAcceptLanguage);
      http.addHeader("Accept-Encoding", "gzip");

      uint32_t lastBodyLogMs = 0;
      const auto onData = [&](const uint8_t* data, size_t length) {
        if (jobCancel_.load(std::memory_order_relaxed)) return false;
        if (bodyLength + length > kMaxBodyBytes) {
          truncated = true;
          return false;
        }
        memcpy(body + bodyLength, data, length);
        bodyLength += length;
        const size_t total = http.getContentLength();
        if (bodyLength == length) {
          LOG_INF(kTag, "first body bytes after %u ms (total=%u)", static_cast<unsigned>(millis() - loadingStartedMs_),
                  static_cast<unsigned>(total));
        }
        jobBytes_.store(static_cast<uint32_t>(bodyLength), std::memory_order_relaxed);
        jobLastByteMs_.store(millis(), std::memory_order_relaxed);
        if (total > 0) {
          jobTotal_.store(static_cast<uint32_t>(total), std::memory_order_relaxed);
          const int permille = static_cast<int>((bodyLength * 1000) / total);
          jobPermille_.store(std::min(1000, permille), std::memory_order_relaxed);
        }
        if (millis() - lastBodyLogMs > 5000) {
          lastBodyLogMs = millis();
          LOG_INF(kTag, "body %u/%u after %u ms", static_cast<unsigned>(bodyLength), static_cast<unsigned>(total),
                  static_cast<unsigned>(millis() - loadingStartedMs_));
        }
        return true;
      };
      const auto shouldAbort = [&]() { return jobCancel_.load(std::memory_order_relaxed); };

      int status = -1;
      // The first connection right after association fails intermittently
      // while lwIP and the TLS buffers settle. One delayed retry over a fresh
      // connection is enough and bounded.
      for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
          LOG_INF(kTag, "retrying fetch after a %u ms pause (attempt %d)", 1500u, attempt + 1);
          vTaskDelay(pdMS_TO_TICKS(1500));
          http.end();
          bodyLength = 0;
          truncated = false;
          http.setTimeout(static_cast<uint32_t>(kRequestTimeoutMs));
        }
        LOG_INF(kTag, "GET %.80s attempt %d", url.c_str(), attempt + 1);
        const uint32_t attemptStartMs = millis();
        status = http.GET(onData, shouldAbort);
        LOG_INF(kTag, "GET attempt %d status=%d body=%u total=%u in %u ms", attempt + 1, status,
                static_cast<unsigned>(bodyLength), static_cast<unsigned>(http.getContentLength()),
                static_cast<unsigned>(millis() - attemptStartMs));
        if (jobCancel_.load(std::memory_order_relaxed)) break;
        if (status >= 0 && !http.callbackAborted()) break;
      }
      jobHttpStatus_ = status;
      if (jobCancel_.load(std::memory_order_relaxed)) {
        jobResult_ = JobResult::Canceled;
        break;
      }
      if (status < 0 || http.callbackAborted()) {
        // A body cap aborts the transfer through onData; that is a truncated
        // page, not a dead network. Everything received is still parsed.
        if (!(truncated && bodyLength > 0)) {
          LOG_ERR(kTag, "fetch failed status=%d url=%.70s", status, url.c_str());
          jobResult_ = JobResult::Network;
          break;
        }
      }
      contentType = http.getHeader("content-type");
      contentEncoding = http.getHeader("content-encoding");
      location = http.getHeader("location");
      if (status >= 200 && status < 300) {
        jobPermille_.store(1000, std::memory_order_relaxed);
        jobBytes_.store(static_cast<uint32_t>(bodyLength), std::memory_order_relaxed);
      }

      if (status >= 300 && status < 400 && !location.empty()) {
        std::string next;
        bool sameDocument = false;
        if (!webbrowser::resolveHref(url, location, next, sameDocument) || sameDocument || next.empty()) {
          jobResult_ = JobResult::Http;
          break;
        }
        LOG_INF(kTag, "redirect %d -> %.80s", status, next.c_str());
        url = std::move(next);
        continue;
      }
      if (status != 200) {
        LOG_ERR(kTag, "HTTP %d for %.70s", status, url.c_str());
        jobResult_ = JobResult::Http;
        break;
      }
      if (truncated) LOG_ERR(kTag, "page truncated at %u bytes", static_cast<unsigned>(bodyLength));
      jobStage_.store(2, std::memory_order_relaxed);

      // Followed links can point at binaries (an EPUB download, an image).
      // Parsing those as markup yields noise, so answer "not a page" instead.
      if (!contentType.empty()) {
        const bool textual = contentType.find("text/html") != std::string::npos ||
                             contentType.find("application/xhtml") != std::string::npos ||
                             contentType.find("text/plain") != std::string::npos ||
                             contentType.find("application/xml") != std::string::npos ||
                             contentType.find("text/xml") != std::string::npos;
        if (!textual) {
          LOG_ERR(kTag, "not a web page: %.60s", contentType.c_str());
          jobContentType_ = contentType;
          jobResult_ = JobResult::BadEncoding;
          break;
        }
      }

      const bool looksGzip = bodyLength >= 2 && body[0] == 0x1F && body[1] == 0x8B;
      const bool gzipHeader = contentEncoding.find("gzip") != std::string::npos;
      const bool deflateHeader = contentEncoding.find("deflate") != std::string::npos;
      if (looksGzip || gzipHeader || deflateHeader) {
        // Inflate in place without a second 2 MiB allocation: copy the
        // compressed payload to the second half of the buffer, then inflate
        // into the first half. Output is capped at kMaxBodyBytes, which also
        // keeps it from reaching the copied input.
        size_t payload = 0;
        if (looksGzip) {
          payload = gzipPayloadOffset(body, bodyLength);
        } else if (deflateHeader) {
          // "deflate" is ambiguous in the wild: zlib-wrapped (0x78 ..) or raw.
          payload = bodyLength >= 2 && (body[0] & 0x0F) == 8 && ((body[0] << 8) | body[1]) % 31 == 0 ? 2 : 0;
        }
        if (payload == 0 && gzipHeader) {
          // Some servers label an uncompressed body as gzip; trust the bytes,
          // not the header, and let the charset/HTML sniff decide.
          LOG_ERR(kTag, "gzip header missing (encoding=%s), treating as plain", contentEncoding.c_str());
        } else if (looksGzip && payload == 0) {
          LOG_ERR(kTag, "gzip header malformed: len=%u enc=%s head=%02x%02x", static_cast<unsigned>(bodyLength),
                  contentEncoding.c_str(), body[0], bodyLength > 1 ? body[1] : 0);
          jobResult_ = JobResult::BadEncoding;
          break;
        }
        if (payload != 0) {
          const size_t compressed = bodyLength - payload;
          memmove(body + kMaxBodyBytes, body + payload, compressed);
          InflateReader reader;
          if (!reader.init(false)) {
            jobResult_ = JobResult::BadEncoding;
            break;
          }
          reader.setSource(body + kMaxBodyBytes, compressed);
          size_t produced = 0;
          const InflateStatus status = reader.readAtMost(body, kMaxBodyBytes, &produced);
          if (status == InflateStatus::Error && produced == 0) {
            LOG_ERR(kTag, "inflate failed: %u bytes enc=%s", static_cast<unsigned>(compressed),
                    contentEncoding.c_str());
            jobResult_ = JobResult::BadEncoding;
            break;
          }
          bodyLength = produced;
          // A cut compressed stream (1 MiB body cap) or a full output buffer
          // still yielded readable text; show the start instead of an error.
          if (status != InflateStatus::Done) truncated = true;
          LOG_INF(kTag, "inflate %u -> %u bytes%s", static_cast<unsigned>(compressed),
                  static_cast<unsigned>(produced), truncated ? " (truncated)" : "");
        }
      }
      if (bodyLength == 0) {
        jobResult_ = JobResult::Empty;
        break;
      }

      jobContentType_ = contentType;
      const std::string charset = webbrowser::sniffCharset(
          std::string_view(reinterpret_cast<const char*>(body), std::min<size_t>(bodyLength, 4096)), contentType);
      txt_encoding::Encoding encoding = txt_encoding::detect(body, bodyLength, true);
      if (webbrowser::isGbkCharset(charset)) encoding = txt_encoding::Encoding::Gbk;
      if (encoding == txt_encoding::Encoding::Gbk || encoding == txt_encoding::Encoding::Utf16Le ||
          encoding == txt_encoding::Encoding::Utf16Be) {
        const auto transcoded =
            txt_encoding::transcodeInPlace(body, bodyLength, kBodyCapacity, true, encoding);
        if (transcoded.utf8Length > 0) bodyLength = transcoded.utf8Length;
      }

      webbrowser::ExtractOptions extractOptions;
      const bool extracted = webbrowser::extractDocument(
          std::string_view(reinterpret_cast<const char*>(body), bodyLength), url, incomingDocument_, extractOptions);
      if (!extracted || incomingDocument_.blocks.empty()) {
        jobResult_ = JobResult::Empty;
        break;
      }
      jobResult_ = JobResult::Ok;
      break;
    }
  }

  if (bodyInPsram) {
    heap_caps_free(body);
  } else {
    free(body);
  }
}

void WebBrowserActivity::reapJob() {
  if (jobTask_ == nullptr) return;
  if (jobStackInPsram_) {
    vTaskDeleteWithCaps(jobTask_);
  } else {
    vTaskDelete(jobTask_);
  }
  jobTask_ = nullptr;
  jobStackInPsram_ = false;
}

void WebBrowserActivity::pollJob() {
  if (jobTask_ == nullptr || !jobDone_.load(std::memory_order_acquire)) return;
  reapJob();
  const RenderLock lock;
  if (jobAbandoned_) {
    jobAbandoned_ = false;
    incomingDocument_.clear();
    return;
  }

  switch (jobResult_) {
    case JobResult::Ok:
      document_ = std::move(incomingDocument_);
      pageIndex_ = 0;
      paginateDocument();
      if (pagination_.lines.empty()) {
        state_ = State::Error;
        errorText_ = tr(STR_BROWSER_ERR_EMPTY);
        pendingRestore_ = false;
        pendingKeepPage_ = false;
        break;
      }
      if (pendingRestore_) {
        pageIndex_ = std::min<size_t>(pendingRestorePage_, pagination_.pages.size() - 1);
        pendingRestore_ = false;
      } else if (pendingKeepPage_) {
        const size_t wanted = pendingKeepPageIndex_;
        pageIndex_ = std::min<size_t>(wanted, pagination_.pages.size() - 1);
        if (wanted > 0 && pageIndex_ < wanted) showNotice(tr(STR_BROWSER_PAGE_MOVED));
        pendingKeepPage_ = false;
      }
      state_ = State::Page;
      if (!document_.text.empty() && currentUrl_.rfind("https://example.invalid", 0) != 0) {
        webbrowser::Place place = currentPlace();
        place.page = static_cast<uint16_t>(pageIndex_);
        webbrowser::recordHistory(visits_, place);
        historyDirty_ = true;  // SD write outside the render lock, in loop()
      }
      break;
    case JobResult::Canceled:
      pendingRestore_ = false;
      pendingKeepPage_ = false;
      state_ = document_.blocks.empty() ? State::Home : State::Page;
      break;
    case JobResult::Network:
      pendingRestore_ = false;
      pendingKeepPage_ = false;
      state_ = State::Error;
      errorText_ = WeReadHttpClient::networkReady() ? tr(STR_BROWSER_ERR_NETWORK) : tr(STR_BROWSER_NO_WIFI);
      break;
    case JobResult::Http: {
      state_ = State::Error;
      char buffer[96];
      snprintf(buffer, sizeof(buffer), "%s %d", tr(STR_BROWSER_ERR_HTTP), jobHttpStatus_);
      errorText_ = buffer;
      break;
    }
    case JobResult::Empty:
      state_ = State::Error;
      errorText_ = tr(STR_BROWSER_ERR_EMPTY);
      break;
    case JobResult::Oversize:
      state_ = State::Error;
      errorText_ = tr(STR_BROWSER_ERR_ENCODING);
      break;
    case JobResult::BadEncoding: {
      pendingRestore_ = false;
      pendingKeepPage_ = false;
      state_ = State::Error;
      const bool htmlType = jobContentType_.empty() || jobContentType_.find("text/html") != std::string::npos ||
                            jobContentType_.find("application/xhtml") != std::string::npos ||
                            jobContentType_.find("text/plain") != std::string::npos ||
                            jobContentType_.find("xml") != std::string::npos;
      if (!htmlType) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "%s (%s)", tr(STR_BROWSER_ERR_NOT_HTML), jobContentType_.c_str());
        errorText_ = buffer;
      } else {
        errorText_ = tr(STR_BROWSER_ERR_ENCODING);
      }
      break;
    }
  }
  LOG_INF(kTag, "fetch done result=%d http=%d blocks=%u bytes=%u", static_cast<int>(jobResult_), jobHttpStatus_,
          static_cast<unsigned>(jobResult_ == JobResult::Ok ? document_.blocks.size() : incomingDocument_.blocks.size()),
          static_cast<unsigned>(jobResult_ == JobResult::Ok ? document_.text.size() : incomingDocument_.text.size()));
  requestUpdate();
}

void WebBrowserActivity::paginateDocument() {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = safe.y + metrics.headerHeight + 10;
  const int contentBottom = safe.y + safe.height - kFooterHeight - 60;
  const int contentWidth = safe.width - kSidePadding * 2 - kWrapSafety;

  // One face for the whole document. getTextWidth and drawText then resolve the
  // same face for every line, so measuring per glyph cannot disagree with the
  // line the renderer actually draws.
  const uint32_t fontStartMs = millis();
  // Follow the reader's size and use its vector face so the 16-level canvas has
  // real glyph coverage; fall back to the built-in 12pt bitmap when the pack did
  // not register that size.
  const int readerFace = CJK_READER_SIZE_FONT_ID(SETTINGS.fontPointSize);
  bodyFont_ = renderer.getLineHeight(readerFace) > 0 ? readerFace : CJK_UI_12_FONT_ID;
  LOG_INF(kTag, "body face %d (%dpt requested)", bodyFont_, static_cast<int>(SETTINGS.fontPointSize));
  const uint32_t fontDoneMs = millis();

  webbrowser::PagerSpec spec;
  spec.contentWidth = std::max(40, contentWidth);
  spec.pageHeight = std::max(40, contentBottom - contentTop);
  spec.lineHeight = renderer.getLineHeight(bodyFont_);
  spec.blockGap = std::max(4, spec.lineHeight / 2);
  spec.headingGap = spec.lineHeight;
  spec.measure = &WebBrowserActivity::measureThunk;
  spec.spaceAdvance = &WebBrowserActivity::spaceThunk;
  spec.measureContext = this;
  const uint32_t wrapStartMs = millis();
  webbrowser::paginate(document_, spec, pagination_);
  const uint32_t wrapDoneMs = millis();

  LOG_INF(kTag,
          "pager: safe=%d,%d %dx%d content=%d pageH=%d lineH=%d lines=%u pages=%u font=%d tFont=%u tWrap=%u",
          safe.x, safe.y, safe.width, safe.height, spec.contentWidth, spec.pageHeight, spec.lineHeight,
          static_cast<unsigned>(pagination_.lines.size()), static_cast<unsigned>(pagination_.pages.size()),
          bodyFont_, static_cast<unsigned>(fontDoneMs - fontStartMs), static_cast<unsigned>(wrapDoneMs - wrapStartMs));
}

bool WebBrowserActivity::currentPageBookmarked() const {
  return !currentUrl_.empty() && webbrowser::findBookmark(bookmarks_, currentUrl_) >= 0;
}

webbrowser::WebVector<webbrowser::Place>& WebBrowserActivity::activeList() {
  return state_ == State::Bookmarks ? bookmarks_ : visits_;
}

const webbrowser::WebVector<webbrowser::Place>& WebBrowserActivity::activeList() const {
  return state_ == State::Bookmarks ? bookmarks_ : visits_;
}

void WebBrowserActivity::openMenu() {
  if (state_ != State::Page) return;
  std::vector<std::string> labels;
  labels.push_back(tr(STR_BROWSER_ADDRESS_ITEM));
  labels.push_back(tr(STR_BROWSER_RELOAD));
  labels.push_back(currentPageBookmarked() ? tr(STR_BROWSER_REMOVE_BOOKMARK) : tr(STR_BROWSER_ADD_BOOKMARK));
  labels.push_back(tr(STR_BROWSER_BOOKMARKS_TITLE));
  labels.push_back(tr(STR_BROWSER_HISTORY_TITLE));
  labels.push_back(tr(STR_BROWSER_HOME));
  std::vector<const char*> options;
  for (const auto& label : labels) options.push_back(label.c_str());
  optionPopup_.show(tr(STR_BROWSER_MENU), options.data(), static_cast<int>(options.size()), 0,
                    [this](const int index) {
                      switch (index) {
                        case 0: openAddressBar(); break;
                        case 1: reloadCurrentPage(); break;
                        case 2: {
                          const bool had = currentPageBookmarked();
                          webbrowser::toggleBookmark(bookmarks_, currentPlace());
                          bookmarksDirty_ = true;
                          showNotice(had ? tr(STR_BROWSER_REMOVED) : tr(STR_BROWSER_ADDED));
                          requestUpdate();
                          break;
                        }
                        case 3: enterList(State::Bookmarks); break;
                        case 4: enterList(State::History); break;
                        case 5: goHomeState(); break;
                        default: break;
                      }
                    });
  requestUpdate();
}

void WebBrowserActivity::enterList(const State list) {
  listReturnState_ = (state_ == State::Home) ? State::Home : State::Page;
  listIndex_ = 0;
  listScroll_ = 0;
  state_ = list;
  requestUpdate();
}

void WebBrowserActivity::openPlace(const webbrowser::Place& place) {
  if (place.url.empty()) return;
  pendingRestore_ = true;
  pendingRestorePage_ = place.page;
  navigateTo(std::string(place.url.c_str()), true);
}

void WebBrowserActivity::updateHistoryPage() {
  if (currentUrl_.empty() || state_ != State::Page) return;
  webbrowser::Place place = currentPlace();
  place.page = static_cast<uint16_t>(pageIndex_);
  webbrowser::recordHistory(visits_, place);
  historyDirty_ = true;
}

void WebBrowserActivity::reloadCurrentPage() {
  if (currentUrl_.empty()) return;
  pendingKeepPage_ = true;
  pendingKeepPageIndex_ = static_cast<uint16_t>(pageIndex_);
  navigateTo(currentUrl_, false);
}

void WebBrowserActivity::goHomeState() {
  updateHistoryPage();
  state_ = State::Home;
  requestUpdate();
}

void WebBrowserActivity::openListMenu() {
  const char* labels[] = {tr(STR_DELETE), tr(STR_BROWSER_CLEAR), tr(STR_CANCEL)};
  optionPopup_.show(tr(STR_BROWSER_MENU), labels, 3, 2, [this](const int index) {
    if (index == 0) {
      confirmDeleteAt(listIndex_);
    } else if (index == 1) {
      confirmClearList();
    }
  });
  requestUpdate();
}

void WebBrowserActivity::confirmDeleteAt(const int index) {
  auto& list = activeList();
  if (index < 0 || index >= static_cast<int>(list.size())) return;
  const char* labels[] = {tr(STR_DELETE), tr(STR_CANCEL)};
  optionPopup_.show(tr(STR_BROWSER_DELETE_ENTRY), labels, 2, 1, [this, index](const int choice) {
    if (choice != 0) return;
    auto& current = activeList();
    if (index < 0 || index >= static_cast<int>(current.size())) return;
    current.erase(current.begin() + index);
    if (state_ == State::History) {
      historyDirty_ = true;
    } else {
      bookmarksDirty_ = true;
    }
    const int count = static_cast<int>(current.size());
    listIndex_ = std::max(0, std::min(listIndex_, count - 1));
    showNotice(tr(STR_BROWSER_DELETED));
    requestUpdate();
  });
  requestUpdate();
}

void WebBrowserActivity::confirmClearList() {
  const char* labels[] = {tr(STR_BROWSER_CLEAR), tr(STR_CANCEL)};
  optionPopup_.show(tr(STR_BROWSER_CLEAR_ALL), labels, 2, 1, [this](const int choice) {
    if (choice != 0) return;
    webbrowser::clearPlaces(activeList());
    if (state_ == State::History) {
      historyDirty_ = true;
    } else {
      bookmarksDirty_ = true;
    }
    listIndex_ = 0;
    listScroll_ = 0;
    showNotice(tr(STR_BROWSER_CLEARED));
    requestUpdate();
  });
  requestUpdate();
}

void WebBrowserActivity::drawBookmarkMark(const int x, const int y, const int width, const int height) {
  renderer.fillRect(x, y, width, height, true);
  // V notch at the bottom, cut with background-coloured rows.
  const int notch = height / 4;
  for (int row = 0; row < notch; ++row) {
    const int half = (width / 2) * (notch - row) / (notch + 1);
    if (half <= 0) continue;
    renderer.fillRect(x + width / 2 - half, y + height - 1 - row, half * 2, 1, false);
  }
}

void WebBrowserActivity::goBack() {
  if (state_ == State::Loading) {
    requestCancel();
    return;
  }
  if (state_ == State::Bookmarks || state_ == State::History) {
    state_ = listReturnState_;
    requestUpdate();
    return;
  }
  if (state_ == State::Error) {
    if (document_.blocks.empty()) {
      // A failed first navigation returns to the browser's own start page, not
      // straight out to the system home screen.
      goHomeState();
      return;
    }
    if (!document_.blocks.empty()) {
      // The failed navigation replaced currentUrl_ while the document on screen
      // is still the previous page. Put the URL back too, and drop the failed
      // entry so Back from that page does not retry it.
      const std::string docUrl(document_.url.c_str());
      if (!docUrl.empty() && docUrl != currentUrl_) {
        if (!history_.empty() && std::string(history_.back().c_str()) == currentUrl_) history_.pop_back();
        currentUrl_ = docUrl;
        pendingUrl_ = docUrl;
      }
      state_ = State::Page;
      requestUpdate();
      return;
    }
  }
  if (state_ == State::Home) {
    finish();
    return;
  }
  if (state_ != State::Page) {
    finish();
    return;
  }

  // Reading: Back is page -> URL -> exit, never a jump to Home.
  if (pageIndex_ > 0) {
    pageTurn(-1);
    return;
  }
  updateHistoryPage();
  if (history_.size() > 1) {
    history_.pop_back();
    const std::string previous(history_.back().c_str());
    pendingRestore_ = true;
    pendingRestorePage_ = 0;
    for (const auto& visit : visits_) {
      if (std::string(visit.url.c_str()) == previous) {
        pendingRestorePage_ = visit.page;
        break;
      }
    }
    navigateTo(previous, false);
    return;
  }
  // Page 1 of the first URL: back to the browser's start page. Back on that
  // page exits the app.
  goHomeState();
}

void WebBrowserActivity::suspendTimeSync() {
  if (timeSyncSuspended_) return;
  timeSyncSuspended_ = true;
  halClock.setAutoSyncEnabled(false);
#if !defined(SIMULATOR)
  if (esp_sntp_enabled()) esp_sntp_stop();
#endif
  LOG_INF(kTag, "time sync paused for the browsing session");
}

void WebBrowserActivity::resumeTimeSync() {
  if (!timeSyncSuspended_) return;
  timeSyncSuspended_ = false;
  halClock.setAutoSyncEnabled(SETTINGS.clockAutoSync != 0);
  LOG_INF(kTag, "time sync restored (auto=%d)", static_cast<int>(SETTINGS.clockAutoSync));
}

void WebBrowserActivity::requestCancel() {
  hasQueuedNav_ = false;
  queuedNavUrl_.clear();
  if (jobTask_ == nullptr || jobDone_.load(std::memory_order_acquire)) return;
  jobCancel_.store(true, std::memory_order_relaxed);
  // TLS connect cannot be aborted; give it a moment, then let the UI go while
  // the worker unwinds in the background.
  cancelDeadlineMs_ = millis() + 3000;
  showNotice(tr(STR_BROWSER_CANCELING));
}

void WebBrowserActivity::abandonJob(const bool toPage, const char* message) {
  if (jobTask_ == nullptr || jobDone_.load(std::memory_order_acquire)) return;
  jobAbandoned_ = true;
  jobCancel_.store(true, std::memory_order_relaxed);
  const RenderLock lock;
  if (toPage) {
    state_ = document_.blocks.empty() ? State::Home : State::Page;
  } else {
    state_ = State::Error;
    errorText_ = message != nullptr ? message : "";
  }
  requestUpdate();
}

void WebBrowserActivity::gatherPageLinks(std::vector<std::string>& labels, std::vector<std::string>& urls) const {
  labels.clear();
  urls.clear();
  if (pagination_.pages.empty() || pageIndex_ >= pagination_.pages.size()) return;
  const auto& page = pagination_.pages[pageIndex_];
  const uint32_t first = page.firstLine;
  const uint32_t last = first + page.lineCount;
  const uint32_t pageStart = pagination_.lines[first].textStart;
  const auto& lastLine = pagination_.lines[last - 1];
  const uint32_t pageEnd = lastLine.textStart + lastLine.textLength;

  for (size_t i = 0; i < document_.links.size() && labels.size() < 8; ++i) {
    const auto& link = document_.links[i];
    if (link.textStart >= pageEnd || link.textStart + link.textLength <= pageStart) continue;
    const std::string href(document_.hrefs.data() + link.hrefStart, link.hrefLength);
    if (std::find(urls.begin(), urls.end(), href) != urls.end()) continue;
    std::string label(document_.text.data() + link.textStart, link.textLength);
    label = trimTo(label, 60);
    if (label.empty()) label = href;
    labels.push_back(std::move(label));
    urls.push_back(href);
  }
}

void WebBrowserActivity::openLinkPicker() {
  std::vector<std::string> labels;
  std::vector<std::string> urls;
  gatherPageLinks(labels, urls);
  if (labels.empty()) {
    showNotice(tr(STR_BROWSER_NO_LINKS));
    return;
  }

  // Ordinal prefix, like the product spec: the list is in reading order and the
  // number tells the user which link they are choosing without a cursor.
  std::vector<std::string> numbered;
  numbered.reserve(labels.size());
  for (size_t i = 0; i < labels.size(); ++i) {
    numbered.push_back(std::to_string(i + 1) + " " + labels[i]);
  }
  std::vector<const char*> options;
  options.reserve(numbered.size());
  for (const auto& label : numbered) options.push_back(label.c_str());
  auto captured = urls;
  optionPopup_.show(tr(STR_BROWSER_LINKS), options.data(), static_cast<int>(options.size()), 0,
                    [this, captured](int index) {
                      if (index >= 0 && index < static_cast<int>(captured.size())) followLink(captured[index]);
                    });
  requestUpdate();
}

void WebBrowserActivity::followLink(const std::string& url) {
  navigateTo(url, true);
}

void WebBrowserActivity::pageTurn(const int delta) {
  if (state_ != State::Page || pagination_.pages.empty()) return;
  const int target = static_cast<int>(pageIndex_) + delta;
  if (target < 0) {
    showNotice(tr(STR_BROWSER_FIRST_PAGE));
    return;
  }
  if (target >= static_cast<int>(pagination_.pages.size())) {
    showNotice(tr(STR_BROWSER_LAST_PAGE));
    return;
  }
  pageIndex_ = static_cast<size_t>(target);
  ++turnsSinceHistory_;
  if (turnsSinceHistory_ >= 10) {
    turnsSinceHistory_ = 0;
    updateHistoryPage();
  }
  requestUpdate();
}

void WebBrowserActivity::showNotice(const char* message) {
  const RenderLock lock;
  notice_ = message ? message : "";
  noticeUntilMs_ = millis() + 2500;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void WebBrowserActivity::render(RenderLock&&) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  // A reading page goes through the 16-level canvas so the vector face keeps
  // its antialiasing; chrome/footer/icons ride along and are thresholded.
  const bool gray16 = state_ == State::Page && renderer.beginGrayscale16();
  if (!gray16) renderer.clearScreen();

  switch (state_) {
    case State::Home:
      renderHome();
      break;
    case State::Loading: {
      // The wait card floats over whatever the user was reading, so the screen
      // never goes blank and the context is preserved. The header keeps the
      // loaded page's own URL, not the one that is being fetched.
      const std::string loadedUrl(document_.url.c_str());
      if (!document_.blocks.empty()) {
        renderPage(&loadedUrl, false);
      } else {
        renderHome(false);
      }
      renderLoading();
      break;
    }
    case State::Page:
      renderPage();
      break;
    case State::Error:
      renderError();
      break;
    case State::Bookmarks:
    case State::History:
      renderList();
      break;
  }

  if (noticeUntilMs_ != 0 && !notice_.empty()) {
    const int width = renderer.getScreenWidth();
    const int height = renderer.getScreenHeight();
    const int textWidth = renderer.getTextWidth(kSmallFont, notice_.c_str());
    const int pillWidth = std::min(width - 24, textWidth + 32);
    const int x = (width - pillWidth) / 2;
    const int y = height - kFooterHeight - 52;
    renderer.fillRoundedRect(x, y, pillWidth, 40, 20, Color::Black);
    renderer.drawText(kSmallFont, x + 16, y + (40 - renderer.getLineHeight(kSmallFont)) / 2, notice_.c_str(),
                      false);
  }

  if (optionPopup_.isActive()) optionPopup_.render(renderer);
  if (gray16) {
    if (renderer.commitGrayscale16(false)) return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::Normal);
}

void WebBrowserActivity::drawShell(const char* title, const char* subtitle, const bool backButton) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const Rect header{safe.x, safe.y, safe.width, metrics.headerHeight};
  GUI.drawHeader(renderer, header, title, nullptr, backButton);
  if (subtitle != nullptr && subtitle[0] != '\0') {
    char clipped[96];
    snprintf(clipped, sizeof(clipped), "%s", subtitle);
    renderer.drawText(kSmallFont, safe.x + metrics.contentSidePadding,
                      header.y + header.height - renderer.getLineHeight(kSmallFont), clipped, true);
  }
}

void WebBrowserActivity::drawHints(const char* back, const char* confirm, const char* previous,
                                   const char* next) {
  // Touch boards hide the button-hint row entirely (BaseTheme::buttonHintsVisible);
  // their actions live in on-screen pills. Button-only boards still get the
  // standard four-slot hint bar instead of a hand-rolled line of text.
  const auto labels = mappedInput.mapLabels(back, confirm, previous, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WebBrowserActivity::drawPageCounter() {
  if (state_ != State::Page || pagination_.pages.empty()) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int y = safe.y + safe.height - kFooterHeight + 6;
  char page[32];
  snprintf(page, sizeof(page), "%u / %u", static_cast<unsigned>(pageIndex_ + 1),
           static_cast<unsigned>(pagination_.pages.size()));
  const int width = renderer.getTextWidth(kSmallFont, page);
  renderer.drawText(kSmallFont, safe.x + safe.width - metrics.contentSidePadding - width, y, page, true);
}

void WebBrowserActivity::drawListMenuPill(const Rect& safe) {
  constexpr int kBtnW = 120;
  constexpr int kBtnH = 48;
  listMenuRect_ = Rect{safe.x + safe.width - kSidePadding - kBtnW,
                       safe.y + safe.height - kFooterHeight - kBtnH - 6, kBtnW, kBtnH};
  GUI.drawActionButton(renderer, listMenuRect_, tr(STR_BROWSER_HINT_MENU), false);
}

void WebBrowserActivity::renderHome(const bool withFooter) {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawShell(tr(STR_BROWSER_TITLE), nullptr);

  constexpr int kUi = UI_10_FONT_ID;
  const int x = safe.x + kSidePadding;
  const int width = safe.width - kSidePadding * 2;
  const int lineHeight = renderer.getLineHeight(kUi);
  int y = safe.y + metrics.headerHeight + 24;

  homeAddressRect_ = Rect{x, y, width, 64};
  renderer.drawRoundedRect(x, y, width, 64, 2, 32, true);
  InxAppIcons::draw(renderer, UIIcon::Browser, x + 14, y + 16, 1, false);
  const std::string addressText =
      currentUrl_.empty() ? tr(STR_BROWSER_ADDRESS_PROMPT) : webbrowser::hostOf(currentUrl_);
  const std::string clippedAddress = renderer.truncatedText(kUi, addressText.c_str(), width - 84);
  renderer.drawText(kUi, x + 56, y + (64 - lineHeight) / 2, clippedAddress.c_str(), true);
  y += 64 + 20;

  constexpr int kRowH = 88;
  GUI.drawCardFrame(renderer, Rect{x, y, width, kRowH * 2}, false);
  homeBookmarksRect_ = Rect{x, y, width, kRowH};
  homeHistoryRect_ = Rect{x, y + kRowH, width, kRowH};
  const auto drawCardRow = [&](const int rowY, const char* label, const size_t count) {
    renderer.drawText(kUi, x + 24, rowY + (kRowH - lineHeight) / 2, label, true);
    char value[32];
    snprintf(value, sizeof(value), "%u >", static_cast<unsigned>(count));
    const int valueWidth = renderer.getTextWidth(kUi, value);
    renderer.drawText(kUi, x + width - 24 - valueWidth, rowY + (kRowH - lineHeight) / 2, value, true);
  };
  drawCardRow(y, tr(STR_BROWSER_BOOKMARKS), bookmarks_.size());
  renderer.drawLine(x + 24, y + kRowH, x + width - 24, y + kRowH, true);
  drawCardRow(y + kRowH, tr(STR_BROWSER_HISTORY), visits_.size());
  y += kRowH * 2 + 24;

  renderer.drawText(kSmallFont, x, y, tr(STR_BROWSER_HOME_RECENT), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(kSmallFont) + 8;

  homeRecentRects_.clear();
  const int bottom = safe.y + safe.height - kFooterHeight - 4;
  constexpr int kRecentRowH = 100;
  if (visits_.empty()) {
    renderer.drawCenteredText(kSmallFont, y + 32, tr(STR_BROWSER_EMPTY_HISTORY));
  } else {
    const int maxRows = std::max(0, (bottom - y) / kRecentRowH);
    const int rows = std::min<int>({static_cast<int>(visits_.size()), maxRows, 5});
    for (int i = 0; i < rows; ++i) {
      const auto& place = visits_[static_cast<size_t>(i)];
      const Rect row{x, y, width, kRecentRowH};
      homeRecentRects_.push_back(row);
      const std::string title = place.title.empty() ? std::string(place.url.c_str()) : std::string(place.title.c_str());
      const std::string clipped = renderer.truncatedText(kUi, title.c_str(), width - 16);
      renderer.drawText(kUi, x, y + 16, clipped.c_str(), true);
      char subtitle[128];
      snprintf(subtitle, sizeof(subtitle), "%s  ·  %s %u  ·  %s", webbrowser::hostOf(std::string(place.url.c_str())).c_str(),
               tr(STR_BROWSER_PAGE_WORD), static_cast<unsigned>(place.page) + 1,
               formatVisitedAt(place.visitedAt).c_str());
      renderer.drawText(kSmallFont, x, y + 16 + lineHeight + 6, subtitle, true);
      renderer.drawLine(x, y + kRecentRowH - 1, x + width, y + kRecentRowH - 1, true);
      y += kRecentRowH;
    }
  }

  if (withFooter) drawHints(tr(STR_BACK), tr(STR_OPEN), nullptr, nullptr);
}

void WebBrowserActivity::renderLoading() {
  const std::string host = webbrowser::hostOf(pendingUrl_);
  const bool wifi = connectingWifi_;
  const uint8_t stage = jobStage_.load(std::memory_order_relaxed);
  const unsigned received = jobBytes_.load(std::memory_order_relaxed);
  const unsigned total = jobTotal_.load(std::memory_order_relaxed);
  const uint32_t elapsedMs = millis() - loadingStartedMs_;

  // One continuous bar for the whole navigation. The transport does not expose
  // handshake progress, so the stages are weighted by time and then corrected
  // by real bytes as soon as they arrive; every term is monotonic, so the bar
  // never runs backwards or swims in place.
  int permille = 0;
  const char* title = tr(STR_BROWSER_LOADING);
  char detail[80];
  if (wifi) {
    title = tr(STR_BROWSER_CONNECTING_WIFI);
    permille = 100 + std::min<int>(150, static_cast<int>(elapsedMs / 100));  // 10-25 %
    snprintf(detail, sizeof(detail), "%u s", static_cast<unsigned>(elapsedMs / 1000));
  } else if (stage >= 2) {
    permille = 950;
    snprintf(detail, sizeof(detail), "%s", tr(STR_BROWSER_PARSING));
  } else {
    const int base = 250 + std::min<int>(200, static_cast<int>(elapsedMs / 200));  // 25-45 %
    if (received > 0 && total > 0) {
      permille = 450 + static_cast<int>(std::min<uint64_t>(500, static_cast<uint64_t>(received) * 500 / total));
    } else if (received > 0) {
      permille = std::max(base, 450 + std::min<int>(450, static_cast<int>(received / 8192)));
    } else {
      permille = base;
    }
    if (received > 0 && total > 0) {
      const double percent = permille / 10.0;
      if (percent < 10.0) {
        snprintf(detail, sizeof(detail), "%.1f%%  %.1f / %.1f MB", percent, received / 1048576.0,
                 total / 1048576.0);
      } else {
        snprintf(detail, sizeof(detail), "%.0f%%  %.1f / %.1f MB", percent, received / 1048576.0,
                 total / 1048576.0);
      }
    } else if (received > 0) {
      const uint32_t lastByteMs = jobLastByteMs_.load(std::memory_order_relaxed);
      const bool stalled = lastByteMs != 0 && millis() - lastByteMs > 2500;
      if (received < 1024) {
        snprintf(detail, sizeof(detail), stalled ? "%u B · %s" : "%u B", received,
                 stalled ? tr(STR_BROWSER_WAITING_SHORT) : "");
      } else {
        snprintf(detail, sizeof(detail), stalled ? "%.1f KB · %s" : "%.1f KB", received / 1024.0,
                 stalled ? tr(STR_BROWSER_WAITING_SHORT) : "");
      }
    } else if (elapsedMs > 20000) {
      snprintf(detail, sizeof(detail), "%s", tr(STR_BROWSER_SLOW));
    } else if (elapsedMs > 4000) {
      snprintf(detail, sizeof(detail), "%s", tr(STR_BROWSER_WAITING_SERVER));
    } else {
      snprintf(detail, sizeof(detail), "%u s", static_cast<unsigned>(elapsedMs / 1000));
    }
  }
  permille = std::max(0, std::min(1000, permille));
  const auto layout = status_card::paintWait(renderer, title, host.empty() ? nullptr : host.c_str(), detail, permille,
                                             /*withCancel=*/true, elapsedMs);
  loadingCancelRect_ = layout.cancel;
}

void WebBrowserActivity::renderError() {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  if (!document_.blocks.empty() && !pagination_.pages.empty()) {
    // A document is already on screen: float the reason over it instead of
    // throwing the reader back to a blank page. Back closes the card.
    const std::string docUrl(document_.url.c_str());
    renderPage(&docUrl, false);
    status_card::paintMessage(renderer, tr(STR_BROWSER_ERROR_TITLE), errorText_.c_str());
  } else {
    const std::string host = webbrowser::hostOf(currentUrl_);
    drawShell(tr(STR_BROWSER_ERROR_TITLE), host.c_str());
    const Rect body{safe.x + 48, safe.y + 220, safe.width - 96, 260};
    UITheme::drawCenteredWrappedText(renderer, body, kBodyFont, errorText_.c_str(), 4);
  }
  // On-screen actions: a primary Retry and a secondary Back, in the same
  // bordered pill style every other CPY screen uses. They sit above the
  // button-hint band so touch boards can act without a physical key.
  constexpr int kBtnH = 56;
  constexpr int kRetryW = 200;
  constexpr int kBackW = 140;
  constexpr int kGap = 20;
  const int rowY = safe.y + safe.height - kFooterHeight - kBtnH - 8;
  const int rowX = safe.x + (safe.width - (kRetryW + kGap + kBackW)) / 2;
  errorRetryRect_ = Rect{rowX, rowY, kRetryW, kBtnH};
  errorBackRect_ = Rect{rowX + kRetryW + kGap, rowY, kBackW, kBtnH};
  GUI.drawActionButton(renderer, errorRetryRect_, tr(STR_RETRY), true);
  GUI.drawActionButton(renderer, errorBackRect_, tr(STR_BACK), false);
  drawHints(tr(STR_BACK), tr(STR_RETRY), nullptr, nullptr);
}

void WebBrowserActivity::renderList() {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& list = activeList();
  const bool bookmarks = state_ == State::Bookmarks;
  char header[64];
  const StrId titleId = bookmarks ? StrId::STR_BROWSER_BOOKMARKS_TITLE : StrId::STR_BROWSER_HISTORY_TITLE;
  snprintf(header, sizeof(header), "%s  ·  %u", I18N.get(titleId), static_cast<unsigned>(list.size()));
  drawShell(header, nullptr);

  constexpr int kUi = UI_10_FONT_ID;
  constexpr int kRowH = 112;
  const int x = safe.x + kSidePadding;
  const int width = safe.width - kSidePadding * 2;
  const int top = safe.y + metrics.headerHeight + 12;
  const int bottom = safe.y + safe.height - kFooterHeight - 60;
  const int lineHeight = renderer.getLineHeight(kUi);
  listRowRects_.clear();
  listTrashRects_.clear();

  renderer.drawLine(x, top, x + width, top, true);
  if (list.empty()) {
    const StrId emptyId = bookmarks ? StrId::STR_BROWSER_EMPTY_BOOKMARKS : StrId::STR_BROWSER_EMPTY_HISTORY;
    renderer.drawCenteredText(kSmallFont, top + 70, I18N.get(emptyId));
    if (bookmarks) renderer.drawCenteredText(kSmallFont, top + 110, tr(STR_BROWSER_EMPTY_HINT));
    drawListMenuPill(safe);
    drawHints(tr(STR_BACK), tr(STR_OPEN), nullptr, nullptr);
    return;
  }

  const int visible = std::max(1, (bottom - top) / kRowH);
  const int count = static_cast<int>(list.size());
  listIndex_ = std::max(0, std::min(listIndex_, count - 1));
  if (listIndex_ < listScroll_) listScroll_ = listIndex_;
  if (listIndex_ >= listScroll_ + visible) listScroll_ = listIndex_ - visible + 1;
  listScroll_ = std::max(0, std::min(listScroll_, std::max(0, count - visible)));

  const int last = std::min(count, listScroll_ + visible);
  for (int i = listScroll_; i < last; ++i) {
    const auto& place = list[static_cast<size_t>(i)];
    const int y = top + (i - listScroll_) * kRowH;
    const bool selected = i == listIndex_;
    if (selected) renderer.fillRect(x - 10, y + kRowH / 2 - 24, 5, 48, true);

    const std::string title = place.title.empty() ? std::string(place.url.c_str()) : std::string(place.title.c_str());
    const std::string clipped = renderer.truncatedText(kUi, title.c_str(), width - 150);
    renderer.drawText(kUi, x, y + 18, clipped.c_str(), true,
                      selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    char subtitle[160];
    snprintf(subtitle, sizeof(subtitle), "%s  ·  %s %u  ·  %s",
             webbrowser::hostOf(std::string(place.url.c_str())).c_str(), tr(STR_BROWSER_PAGE_WORD),
             static_cast<unsigned>(place.page) + 1, formatVisitedAt(place.visitedAt).c_str());
    renderer.drawText(kSmallFont, x, y + 18 + lineHeight + 6, subtitle, true);

    const Rect row{x, y, width, kRowH};
    listRowRects_.push_back(row);
    const Rect trash{x + width - 64, y + (kRowH - 56) / 2, 56, 56};
    listTrashRects_.push_back(trash);
    renderer.drawRoundedRect(trash.x, trash.y, trash.width, trash.height, 2, 16, true);
    renderer.drawIcon(icon_trash_2_24_bits, trash.x + 16, trash.y + 16, 24);
    renderer.drawLine(x, y + kRowH - 1, x + width, y + kRowH - 1, true);
  }

  drawListMenuPill(safe);
  drawHints(tr(STR_BACK), tr(STR_OPEN), nullptr, nullptr);
}

void WebBrowserActivity::renderPage(const std::string* urlOverride, const bool withFooter) {
  if (pagination_.pages.empty()) return;
  if (pageIndex_ >= pagination_.pages.size()) pageIndex_ = pagination_.pages.size() - 1;

  const std::string& effectiveUrl = urlOverride != nullptr ? *urlOverride : currentUrl_;
  const std::string title = document_.title.empty() ? webbrowser::hostOf(effectiveUrl)
                                                    : std::string(document_.title.c_str());
  const std::string host = webbrowser::hostOf(effectiveUrl);
  drawShell(title.c_str(), host.c_str());

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = safe.y + metrics.headerHeight + 10;
  const int contentBottom = safe.y + safe.height - kFooterHeight - 60;
  const int contentLeft = safe.x + kSidePadding;
  const int lineHeight = renderer.getLineHeight(bodyFont_);

  visibleLinks_.clear();
  const auto& page = pagination_.pages[pageIndex_];
  {
    const GfxRenderer::ClipScope clip(renderer, safe.x, contentTop, safe.width, contentBottom - contentTop);
    char lineBuffer[384];
    for (uint32_t i = 0; i < page.lineCount; ++i) {
    const auto& line = pagination_.lines[page.firstLine + i];
    const int y = contentTop + static_cast<int>(line.y - page.yOffset);
    if (y + lineHeight < contentTop || y > contentBottom) continue;

    size_t length = line.textLength;
    if (length >= sizeof(lineBuffer)) {
      length = sizeof(lineBuffer) - 1;
      while (length > 0 &&
             (static_cast<unsigned char>(document_.text.data()[line.textStart + length]) & 0xC0) == 0x80) {
        --length;
      }
    }
    memcpy(lineBuffer, document_.text.data() + line.textStart, length);
    lineBuffer[length] = '\0';
    const auto style = (line.flags & webbrowser::LineHeading) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(bodyFont_, contentLeft, y, lineBuffer, true, style);

    // Underline the link segments that fall on this line and remember their hit
    // rectangles. The underline is the only visual affordance: the page has no
    // cursor, and touch/Confirm both route through the same rects.
    const uint32_t lineStart = line.textStart;
    const uint32_t lineEnd = line.textStart + line.textLength;
    for (size_t linkIndex = 0; linkIndex < document_.links.size(); ++linkIndex) {
      const auto& link = document_.links[linkIndex];
      if (link.textStart >= lineEnd || link.textStart + link.textLength <= lineStart) continue;
      const uint32_t begin = std::max(lineStart, link.textStart);
      const uint32_t end = std::min(lineEnd, link.textStart + link.textLength);
      if (begin >= end) continue;
      const int x0 = contentLeft + measureRange(document_.text, lineStart, begin - lineStart, false);
      const int x1 = x0 + measureRange(document_.text, begin, end - begin, false);
      renderer.drawLine(x0, y + lineHeight - 2, x1, y + lineHeight - 2, true);
      LinkHit hit;
      // Six pixels of slack below the line and a few at the sides: a fingertip
      // aiming at the underline used to fall through to the page-turn zones.
      hit.x = std::max(0, x0 - 4);
      hit.y = y;
      hit.width = std::max(1, x1 - x0 + 8);
      hit.height = lineHeight + 6;
      hit.linkIndex = static_cast<uint32_t>(linkIndex);
      visibleLinks_.push_back(hit);
      }
    }
  }

  if (!effectiveUrl.empty() && webbrowser::findBookmark(bookmarks_, effectiveUrl) >= 0) {
    constexpr int kMarkW = 24;
    constexpr int kMarkH = 32;
    const int markX = safe.x + safe.width - kSidePadding - kMarkW;
    renderer.fillRect(markX - 4, contentTop - 2, kMarkW + 8, kMarkH + 4, false);
    drawBookmarkMark(markX, contentTop, kMarkW, kMarkH);
  }

  if (withFooter) {
    // The visible page-turn affordance is the underlined links plus the edge
    // tap zones; the bottom bar only carries the menu pill and the counter.
    constexpr int kBtnH = 50;
    pageMenuRect_ = Rect{safe.x + kSidePadding, safe.y + safe.height - kFooterHeight - kBtnH - 10, 120, kBtnH};
    GUI.drawActionButton(renderer, pageMenuRect_, tr(STR_BROWSER_HINT_MENU), false);
    drawPageCounter();
    drawHints(tr(STR_BACK), tr(STR_BROWSER_HINT_LINKS), tr(STR_BROWSER_HINT_TURN), tr(STR_BROWSER_HINT_TURN));
  }
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

int WebBrowserActivity::measureThunk(void* context, const char* text, const size_t length, const bool bold) {
  return static_cast<WebBrowserActivity*>(context)->measureTextWidth(text, length, bold);
}

int WebBrowserActivity::spaceThunk(void* context, const uint32_t /*left*/, const uint32_t /*right*/,
                                   const bool bold) {
  return static_cast<WebBrowserActivity*>(context)->spaceWidthInternal(bold);
}

int WebBrowserActivity::measureTextWidth(const char* utf8, const size_t length, const bool bold) {
  uint32_t cp = 0;
  size_t bytes = 0;
  const unsigned char lead = static_cast<unsigned char>(utf8[0]);
  if (lead < 0x80) {
    cp = lead;
    bytes = 1;
  } else if ((lead & 0xE0) == 0xC0 && length >= 2) {
    cp = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[1]) & 0x3Fu);
    bytes = 2;
  } else if ((lead & 0xF0) == 0xE0 && length >= 3) {
    cp = ((lead & 0x0Fu) << 12) | ((static_cast<unsigned char>(utf8[1]) & 0x3Fu) << 6) |
         (static_cast<unsigned char>(utf8[2]) & 0x3Fu);
    bytes = 3;
  } else if ((lead & 0xF8) == 0xF0 && length >= 4) {
    cp = ((lead & 0x07u) << 18) | ((static_cast<unsigned char>(utf8[1]) & 0x3Fu) << 12) |
         ((static_cast<unsigned char>(utf8[2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(utf8[3]) & 0x3Fu);
    bytes = 4;
  } else {
    cp = 0xFFFD;
    bytes = 1;
  }
  if (bytes == length) return glyphWidthInternal(cp, bold);
  // Whole strings only reach here from the opt-in verifier.
  char buffer[512];
  size_t copy = length;
  if (copy >= sizeof(buffer)) {
    copy = sizeof(buffer) - 1;
    while (copy > 0 && (static_cast<unsigned char>(utf8[copy]) & 0xC0) == 0x80) --copy;
  }
  memcpy(buffer, utf8, copy);
  buffer[copy] = '\0';
  return renderer.getTextWidth(bodyFont_, buffer, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

int WebBrowserActivity::measureCodepoint(const uint32_t cp, const bool bold) {
  char buffer[8];
  size_t length = 0;
  if (cp < 0x80) {
    buffer[length++] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    buffer[length++] = static_cast<char>(0xC0 | (cp >> 6));
    buffer[length++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    buffer[length++] = static_cast<char>(0xE0 | (cp >> 12));
    buffer[length++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buffer[length++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    buffer[length++] = static_cast<char>(0xF0 | (cp >> 18));
    buffer[length++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    buffer[length++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buffer[length++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  buffer[length] = '\0';
  return renderer.getTextWidth(bodyFont_, buffer, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

int WebBrowserActivity::spaceWidthInternal(const bool bold) {
  const int style = bold ? 1 : 0;
  if (!spaceReady_[style]) {
    spaceWidth_[style] = renderer.getSpaceWidth(bodyFont_, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    spaceReady_[style] = true;
  }
  return spaceWidth_[style];
}

int WebBrowserActivity::glyphWidthInternal(const uint32_t cp, const bool bold) {
  if (cp == ' ') return spaceWidthInternal(bold);
  if (cachedFont_ != bodyFont_) {
    for (int style = 0; style < 2; ++style) {
      for (int i = 0; i < 95; ++i) asciiReady_[style][i] = false;
      wideReady_[style] = false;
      spaceReady_[style] = false;
      glyphMemoCount_[style] = 0;
    }
    cachedFont_ = bodyFont_;
  }
  const int style = bold ? 1 : 0;
  if (cp >= 0x20 && cp <= 0x7E) {
    const int index = static_cast<int>(cp) - 0x20;
    if (!asciiReady_[style][index]) {
      asciiWidth_[style][index] = measureCodepoint(cp, bold);
      asciiReady_[style][index] = true;
    }
    return asciiWidth_[style][index];
  }
  const bool wide = (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                    (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x3040 && cp <= 0x30FF) ||
                    (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xFF00 && cp <= 0xFFEF);
  if (wide) {
    if (!wideReady_[style]) {
      // Average five ideographs so integer rounding does not bias every line.
      const char* sample = "\xE4\xB8\xAD\xE5\x8D\x8E\xE4\xBA\xBA\xE6\xB0\x91\xE5\x85\xB1";
      wideAdvance_[style] = (renderer.getTextWidth(bodyFont_, sample,
                                                  bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR) +
                             2) /
                            5;
      wideReady_[style] = true;
    }
    return wideAdvance_[style];
  }
  for (int i = 0; i < glyphMemoCount_[style]; ++i) {
    if (glyphMemo_[style][i].cp == cp) return glyphMemo_[style][i].width;
  }
  const int width = measureCodepoint(cp, bold);
  if (glyphMemoCount_[style] < kGlyphMemoSize) {
    glyphMemo_[style][glyphMemoCount_[style]++] = GlyphMemo{cp, width};
  }
  return width;
}

int WebBrowserActivity::measureRange(const webbrowser::WebString& text, const uint32_t start,
                                     const uint32_t length, const bool bold) {
  char buffer[512];
  uint32_t copy = length;
  if (copy >= sizeof(buffer)) {
    copy = sizeof(buffer) - 1;
    while (copy > 0 && (static_cast<unsigned char>(text.data()[start + copy]) & 0xC0) == 0x80) --copy;
  }
  if (copy == 0) return 0;
  memcpy(buffer, text.data() + start, copy);
  buffer[copy] = '\0';
  return renderer.getTextWidth(bodyFont_, buffer, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}
