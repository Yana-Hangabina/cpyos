#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <OpdsStream.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/PluginHttp.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

OpdsBookBrowserActivity::OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 OpdsServer server)
    : CatalogActivity("OpdsBookBrowser", renderer, mappedInput), server(std::move(server)) {}

void OpdsBookBrowserActivity::onEnter() {
  CatalogActivity::onEnter();
  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  releaseEntries();
  navigationHistory.clear();
  CatalogActivity::onExit();
}

void OpdsBookBrowserActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  const auto& entry = entries[index];
  entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
}

void OpdsBookBrowserActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str());
  if (!buildStatusScreen(screen, /*boldError=*/false, /*showDownloadTotal=*/true)) buildBrowsingScreen(screen);
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiScreen& screen) {
  if (entries.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}

void OpdsBookBrowserActivity::drawFooter() {
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING: {
      // Feeds open; books and the pager rows fetch, matching the plugin catalog.
      const char* confirmLabel = tr(STR_OPEN);
      if (!entries.empty()) {
        const bool onPager = (prevRowPresent && nav.selected == 0) ||
                             (nextRowPresent && nav.selected == static_cast<int>(entries.size()) - 1);
        if (onPager || entries[nav.selected].type == OpdsEntryType::BOOK) confirmLabel = tr(STR_FETCH);
      }
      const char* searchLabel = (!searchTemplate.empty() && nav.selected == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case State::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    fail(StrId::STR_NO_SERVER_URL);
    return;
  }

  std::string url = UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  // One body, either an Atom (1.x) feed or an OPDS 2.0 JSON document; OpdsFeed
  // sniffs the payload and picks the matching streaming parser.
  OpdsFeed feed;
  {
    OpdsParserStream stream{feed};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      fail(StrId::STR_FETCH_FEED_FAILED);
      return;
    }
  }

  if (feed.error()) {
    LOG_ERR("OPDS", "Could not parse feed (%s): %s", feed.isJson() ? "JSON" : "XML", url.c_str());
    fail(StrId::STR_PARSE_FEED_FAILED);
    return;
  }
  LOG_DBG("OPDS", "Feed parsed as %s", feed.isJson() ? "OPDS 2.0 JSON" : "OPDS 1.x Atom");

  // Everything the render task reads (entries, rows, state) changes under the
  // lock: a repaint started before this may still be drawing the previous feed.
  // Taken only for the mutation, never around the fetch itself.
  RenderLock lock;
  searchTemplate = feed.getSearchTemplate();
  const auto& nextUrl = feed.getNextPageUrl();
  const auto& prevUrl = feed.getPrevPageUrl();
  const bool feedTruncated = feed.truncated();
  // Reset selection before swapping in a potentially shorter feed.
  nav.reset();
  entries = feed.takeEntries();

  entries.reserve(entries.size() + (prevUrl.empty() ? 0 : 1) + (nextUrl.empty() ? 0 : 1));
  prevRowPresent = !prevUrl.empty();
  nextRowPresent = !nextUrl.empty();
  if (prevRowPresent) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (nextRowPresent) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }
  if (feedTruncated) {
    LOG_INF("OPDS", "Feed truncated to fit memory");
  }

  // Rows first: the render task reads rowItems as soon as the state is BROWSING.
  rebuildRowItems();
  state = entries.empty() ? State::ERROR : State::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  lock.unlock();
  requestUpdate();
}

// Derives rowItems from entries. Called whenever entries changes
// (fetchFeed()/releaseEntries()) so buildBrowsingScreen() reuses the cached
// rows on every repaint instead of rebuilding them per render.
void OpdsBookBrowserActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(entries.size());
  for (const auto& entry : entries) {
    fui::ListItem item;
    item.label = entry.title.c_str();
    // OPDS 2.0 rows may carry a description but no author; showing it beats an
    // empty subtitle, and 1.x entries simply have neither.
    if (entry.type == OpdsEntryType::BOOK) {
      if (!entry.author.empty())
        item.subtitle = entry.author.c_str();
      else if (!entry.summary.empty())
        item.subtitle = entry.summary.c_str();
    }
    if (entry.type == OpdsEntryType::NAVIGATION) item.value = ">";
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

// Callers outside onExit() (which already runs under the render lock) take the lock first: a repaint
// queued before this (e.g. by the press that opened a book) may still be drawing these rows. The lock
// is not re-entrant, so it cannot be taken here.
void OpdsBookBrowserActivity::releaseEntries() {
  // The app's interaction table holds row indices (and hit rects) for the old
  // entries; stop routing touches against it until the next render.
  closeRouting();
  std::vector<OpdsEntry>().swap(entries);
  std::vector<fui::ListItem>().swap(rowItems);
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  {
    RenderLock lock;
    releaseEntries();
  }
  startBrowse();
}

void OpdsBookBrowserActivity::onBackButton() {
  if (state == State::CHECK_WIFI || navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    {
      RenderLock lock;
      releaseEntries();
    }
    startBrowse();
  }
}

void OpdsBookBrowserActivity::startBrowse() {
  nav.reset();
  beginLoading();
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  beginDownload(book.title);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  // opdsDownloadFolder is already a null-terminated char[64]; use it directly —
  // no std::string copy. exists()/mkdir() take const char*.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    // exists()-guard first: mkdir's return-on-existing is unconfirmed, and every
    // existing caller checks exists() before mkdir. On real failure, fall back
    // to SD root so the download is never lost.
    LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  // downloadToFile() needs a std::string, and titles are unbounded (a fixed
  // char[] would truncate). Cold path (a multi-second download follows), so one
  // reserve'd, in-place-appended owning string is the right call.
  std::string filename;
  filename.reserve(96);
  if (haveFolder) filename += folder;
  filename += '/';
  filename += opdsBookFilename(book.author, book.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  // The selected book data is now copied into the download URL, filename, and
  // status line. Reclaim the catalog while TLS owns its record buffers; reload
  // the current feed when the transfer finishes.
  {
    RenderLock lock;  // a repaint may still be drawing these rows
    releaseEntries();
  }

  // downloadFile() (CatalogActivity) releases font caches and checks the TLS heap floor.
  const auto result = downloadFile(downloadUrl, filename, server.username, server.password);
  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    library::markLibraryIndexDirty();
  }
  finishDownload(result);
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = State::BROWSING;
    requestUpdate();
    return;
  }

  std::string url = searchTemplate;
  const std::string encoded = pluginhttp::urlEncodeQuery(query);
  // OPDS 1.x templates use {searchTerms}; OPDS 2.0 follows RFC 6570 and may use
  // {?query}/{&query} (or a bare {query}) instead.
  struct Placeholder {
    const char* token;
    std::string replacement;
  };
  std::string queryPrefix = url.find('?') == std::string::npos ? "?query=" : "&query=";
  const Placeholder placeholders[] = {
      {"{searchTerms}", encoded},
      {"{?query}", queryPrefix + encoded},
      {"{&query}", "&query=" + encoded},
      {"{query}", encoded},
  };
  for (const auto& placeholder : placeholders) {
    const size_t pos = url.find(placeholder.token);
    if (pos != std::string::npos) {
      url.replace(pos, strlen(placeholder.token), placeholder.replacement);
      break;
    }
  }

  navigationHistory.push_back(currentPath);
  currentPath = url;

  {
    RenderLock lock;
    releaseEntries();
  }
  startBrowse();
}
