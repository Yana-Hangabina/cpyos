#include "LibraryListActivity.h"

#include <Bitmap.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <PinyinDb.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "InxItemLayout.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/headerIcons.h"
#include "components/icons/inx_apps.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#ifdef BOARD_HAS_PSRAM
#include "activities/apps/jjwxc/JjwxcActivity.h"
#include "activities/apps/jjwxc/JjwxcUpdates.h"
#endif
#include "util/BookCoverLoader.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;
constexpr unsigned long LONG_PRESS_MS = 1000;
// CPY (refreshed UI frame 02): under the shared mast sit a 92px headline row (48px
// serif page title on the left, shelf count + quiet search/sync actions on the
// right) and an 80px source tab strip with real counts and a 4px underline
// under the active tab, closed by the design's 2px rule.
constexpr int16_t kCpyHeadlineHeight = UiHighDpiProfile::enabled ? 92 : 64;
constexpr int16_t kCpyTabsHeight = UiHighDpiProfile::enabled ? 80 : 60;
constexpr int16_t kCpyTabsRule = 2;
constexpr int16_t kCpyHeaderBandHeight = kCpyHeadlineHeight + kCpyTabsHeight + kCpyTabsRule;
constexpr int16_t kCpyTabGap = 24;
constexpr int16_t kCpyTabUnderline = 4;
constexpr int16_t kCpyTouchTarget = 64;  // quiet header actions keep a 64px hit
// Empty-shelf path rows (frame 15): 112px each, hairline separated.
constexpr int16_t kCpyEmptyPathHeight = UiHighDpiProfile::enabled ? 112 : 84;

// CPY cover grid (refreshed UI frame 02): 32px side padding, four columns, the
// covers at the mockup's 188:250 ratio, a single-line title under each cover
// and 22px of air before the next row.
constexpr int16_t kGridSidePad = 32;
constexpr int16_t kGridCellGap = 20;
constexpr int kGridCoverAspectW = 188;
constexpr int kGridCoverAspectH = 250;
constexpr int16_t kGridCoverRadius = 4;
constexpr int16_t kGridTitleGapTop = 8;
constexpr int16_t kGridRowGapBottom = 22;

// CJK-aware greedy wrap for the coverless placeholder. GfxRenderer's
// wrappedText only breaks on spaces, and a title like "脆皮鸭用户手册" is a
// single word to it — it truncated to "CrossM…" in the grid (sim review,
// 2026-10-16). Breaks per grapheme, trimming trailing spaces; when the line
// budget runs out the remainder gets the usual ellipsis.
std::vector<std::string> wrapCoverTitle(const GfxRenderer& renderer, const int fontId, const char* text,
                                        const int maxWidth, const int maxLines) {
  std::vector<std::string> lines;
  if (text == nullptr || maxWidth <= 0 || maxLines <= 0) return lines;
  std::string line;
  for (const char* p = text; *p != '\0';) {
    const auto c = static_cast<unsigned char>(*p);
    const int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    std::string next = line;
    next.append(p, static_cast<size_t>(len));
    if (!line.empty() && renderer.getTextWidth(fontId, next.c_str()) > maxWidth) {
      while (!line.empty() && line.back() == ' ') line.pop_back();
      lines.push_back(line);
      line.assign(p, static_cast<size_t>(len));
    } else {
      line.swap(next);
    }
    p += len;
  }
  if (!line.empty()) lines.push_back(line);
  if (static_cast<int>(lines.size()) > maxLines) {
    std::string rest;
    for (size_t i = static_cast<size_t>(maxLines) - 1; i < lines.size(); ++i) {
      if (!rest.empty()) rest += ' ';
      rest += lines[i];
    }
    lines.resize(static_cast<size_t>(maxLines) - 1);
    lines.push_back(renderer.truncatedText(fontId, rest.c_str(), maxWidth));
  }
  return lines;
}
constexpr int16_t kGridTopInset = 10;

// The serif page title: the mockups' ~48px Song face on high-density builds
// (the same font the mast uses); C3 keeps the legacy 18px face because the
// subset does not fit its flash budget. Only the high-density headline draws
// it, hence maybe-unused.
[[maybe_unused]] constexpr int serifTitleFontId() {
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  return NOTOSERIF_TITLE_FONT_ID;
#else
  return NOTOSERIF_18_FONT_ID;
#endif
}

// One line of book title under a grid cover: the body face when the board has
// the small-size budget, the compact face otherwise.
constexpr int gridTitleFontId() {
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  // 28 px CJK fallback: six to seven characters fit the 198 px column, which
  // is the design's 24 px title on a 188 px cover. UI_12 (32 px) showed four.
  return UI_10_FONT_ID;
#else
  return SMALL_FONT_ID;
#endif
}

// 1-bpp Mask1 icon blit (bit 1 = leave the pixel, bit 0 = ink), the freeink::
// Icon convention. The SDK's fui bitmaps cannot be drawn outside the fui
// renderer, and the theme's own copy is file-local, so the two quiet header
// actions carry this eight-line blitter.
void drawMaskIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y) {
  const int stride = (icon.w + 7) / 8;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* src = icon.bits + static_cast<size_t>(row) * stride;
    for (int col = 0; col < icon.w; ++col) {
      if (((src[col >> 3] >> (7 - (col & 7))) & 1U) == 0) renderer.drawPixel(x + col, y + row, true);
    }
  }
}

// Design's .seal: a white disc with a 2px black ring and one glyph. White fill
// first because it sits on cover art.
void drawSourceSeal(const GfxRenderer& renderer, const int x, const int y, const int size, const char* glyph) {
  if (glyph == nullptr || *glyph == '\0') return;
  renderer.fillRoundedRect(x, y, size, size, size / 2, Color::White);
  renderer.drawRoundedRect(x, y, size, size, 2, size / 2, true);
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int glyphW = renderer.getTextWidth(SMALL_FONT_ID, glyph, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, x + (size - glyphW) / 2, y + (size - lineH) / 2, glyph, true,
                    EpdFontFamily::BOLD);
}

// Source identity as one glyph. 2 = WeRead, 3 = JJWXC, anything else = local.
// The local badge has no single-glyph i18n key, so it takes the first
// character of STR_SOURCE_LOCAL ("本" / "L") rather than inventing one.
std::string sealGlyphFor(const uint8_t source) {
  if (source == 2) return tr(STR_SOURCE_BADGE_WEREAD);
  if (source == 3) return tr(STR_SOURCE_BADGE_JJWXC);
  const char* local = tr(STR_SOURCE_LOCAL);
  std::string glyph;
  if (local != nullptr && *local != '\0') {
    const auto c = static_cast<unsigned char>(*local);
    const size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    glyph.assign(local, len);
  }
  return glyph;
}

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
constexpr int TAB_SLOTS = AUTHOR_TAB + 1;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == TITLE_TAB) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab) {
  if (tab == TITLE_TAB) return tr(STR_LIBRARY_TAB_TITLE);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  return tr(STR_LIBRARY_TAB_RECENT);
}

// A query the Pinyin matcher can read: ASCII letters, digits, whitespace, or
// the two precomposed ü forms. Anything else (punctuation, other scripts)
// keeps the fold semantics alone, which is both the old behaviour and the
// matcher's own contract — it refuses queries it cannot spell.
bool looksLikePinyinQuery(const std::string& query) {
  bool sawUnit = false;
  for (size_t i = 0; i < query.size();) {
    const unsigned char c = static_cast<unsigned char>(query[i]);
    if (c < 0x80) {
      const bool unit = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      if (!unit && c != ' ' && c != '\t') return false;
      sawUnit |= unit;
      i++;
      continue;
    }
    // U+00FC ü / U+00DC Ü, the only non-ASCII letters the matcher accepts.
    if (c == 0xC3 && i + 1 < query.size() &&
        (static_cast<unsigned char>(query[i + 1]) == 0xBC || static_cast<unsigned char>(query[i + 1]) == 0x9C)) {
      sawUnit = true;
      i += 2;
      continue;
    }
    return false;
  }
  return sawUnit;
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {
  // Three short tab labels: a full-slot pill would stretch across a third of
  // the screen, so cap it at the label plus padding (slots stay put).
  tabPillMaxPad = 16;
  // Slot 0 would otherwise look "already showing entry 0" on the first frame
  // (the tables start zeroed), leaving its seal/progress never computed — and
  // a zero progress would read as a real "0 %".
  for (int i = 0; i < kGridCoverSlots; i++) {
    gridSlotEntry[i] = -1;
    gridSlotProgress[i] = 0xFF;
  }
}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);
  app.on(ACTION_CHIP, &LibraryListActivity::chipActionTrampoline, this);
  app.on(ACTION_SORT, &LibraryListActivity::sortActionTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Rebuild when the index is missing, invalid, or was built with the other
  // metadata mode. Otherwise entering the screen stays instant.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool rebuildNeeded = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                             index.header().metadataEnabled != readMetadata;
  if (rebuildNeeded) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    rebuildIndex();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  // The source tab counts are a property of the index, not of the sort: one
  // stamp per (re)open is what keeps them from being recomputed per frame.
  indexStamp++;
  resolvePinned();

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  Activity::onExit();
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (activeTabIndex != RECENT_TAB || hasFilter() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  pinnedTotal = static_cast<uint8_t>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (selectedEntry() < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (selectedEntry() >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(selectedEntry())].path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedEntry())));
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
      return;
    }
  }
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent shelf always opens the row options menu.
bool LibraryListActivity::deleteEligible() const { return !groupsCollapsed && (!query.empty() || !groupable()); }

void LibraryListActivity::onRowLongPress(const int index) {
  if (isRecentSort(sortOrder)) {
    showRecentBookOptions(index);
  } else if (deleteEligible()) {
    promptDeleteBook(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(entry)].path;
    title = books[static_cast<size_t>(entry)].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 4 : 3, 0,
                   [this, path, title, isStoreRow](const int choice) {
                     swallowHeldReleases();
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         if (isStoreRow) {
                           promptRemoveRecentBook(path, title);
                         } else {
                           promptDeleteBookByPath(path, title);
                         }
                         break;
                       case 2:
                         if (isStoreRow)
                           promptDeleteBookByPath(path, title);
                         else
                           promptRebuildIndex();
                         break;
                       case 3:
                         if (isStoreRow) promptRebuildIndex();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  index.close();
  rebuildIndex();
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  resetAfterRebuild();
  requestUpdate(true);
}

void LibraryListActivity::syncShelfAndUpdates() {
  // Local first: the card is the source of truth for what exists, and a rescan
  // is what makes a freshly copied book appear. Only then ask the online side.
  promptRebuildIndex();
#ifdef BOARD_HAS_PSRAM
  if (!jjwxcUpdates::sessionPresent()) return;
  // The app's shelf walk is the only code that may hold a TLS session: it runs
  // on the PSRAM-stacked worker task and gives the heap back before returning
  // (JjwxcActivity::RefreshUpdates). Doing it here would put a handshake on the
  // render task, which is how a 60-second freeze happens when the AP is gone.
  const bool reopenIndex = index.isOpen();
  index.close();
  JjwxcActivity::requestDeepLink("", JjwxcActivity::DeepLinkAction::RefreshUpdates);
  const bool started = startActivityForResultWith<JjwxcActivity>([this, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    // The snapshot changed underneath us; the badges must re-read the card.
    jjwxcUpdates::invalidate();
    const auto* jjwxcResult = std::get_if<JjwxcReaderResult>(&result.data);
    const bool completed = jjwxcResult != nullptr && jjwxcResult->completed;
    LOG_INF("LIB", "JJWXC update check %s", completed ? "ok" : "failed");
    const char* options[] = {tr(STR_OK_BUTTON)};
    optionPopup.showMessage(tr(STR_LIBRARY),
                            completed ? tr(STR_JJWXC_UPDATES_CHECKED) : tr(STR_JJWXC_UPDATES_FAILED), options, 1, 0,
                            [this](int) { swallowHeldReleases(); });
    requestUpdate(true);
  });
  if (!started) {
    LOG_ERR("LIB", "OOM: JJWXC update check activity");
    JjwxcActivity::requestDeepLink("", JjwxcActivity::DeepLinkAction::None);
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
  }
#endif
}

void LibraryListActivity::resetAfterRebuild() {
  // Sort positions, group starts, and pinned rows all point into the old order.
  indexStamp++;
  applyFilter();
  resolvePinned();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        clearBookCache(path);
        if (!Storage.remove(path.c_str())) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::jjwxcSearchTrampoline(void* ctx, const char* query) {
  static_cast<LibraryListActivity*>(ctx)->openJjwxcSearch(query);
}

void LibraryListActivity::openJjwxcSearch(const char* query) {
#ifdef BOARD_HAS_PSRAM
  JjwxcActivity::requestSearchDeepLink(query);
  activityManager.goToJjwxc();
#endif
}

std::vector<std::string> LibraryListActivity::liveResultsTrampoline(void* ctx, const char* query) {
  return static_cast<LibraryListActivity*>(ctx)->liveResults(query);
}

std::vector<std::string> LibraryListActivity::liveResults(const char* query) {
  std::vector<std::string> out;
  if (query == nullptr || *query == '\0') return out;
  const std::string needle = library::fold(query);
  const pinyin::PinyinTables* pinyinTables =
      (looksLikePinyinQuery(query) && pinyin::PinyinDb::shared().loaded()) ? &pinyin::PinyinDb::shared().tables()
                                                                           : nullptr;
  const int total = static_cast<int>(index.bookCount());
  // Bound the preview: the keyboard calls this on each repaint, and a full
  // no-match scan of a 4,000-book index would be felt on every key.
  constexpr int kPreviewScanCap = 1000;
  const int scan = std::min(total, kPreviewScanCap);
  for (int row = 0; row < scan && out.size() < 6; ++row) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    bool hit = library::matchesQuery(std::string_view(record.fold, record.foldLen), needle);
    if (!hit && pinyinTables != nullptr) {
      hit = pinyinTables->match(record.fold, record.foldLen, query, strlen(query));
    }
    if (!hit) continue;
    std::string title;
    if (index.readTitle(record, title) && !title.empty()) out.push_back(title);
  }
  return out;
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // The Pinyin database (when the card carries one) is read once, here, so the
  // first keystroke never pays for it. A missing resource or no PSRAM is a
  // silent no-op and the search keeps its fold-only behaviour.
  pinyin::PinyinDb::shared().ensureLoaded();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  keyboard->setLiveResults(&LibraryListActivity::liveResultsTrampoline, this);
#ifdef BOARD_HAS_PSRAM
  keyboard->setJjwxcSearch(&LibraryListActivity::jjwxcSearchTrampoline, this);
#endif
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && filteredCount == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
      nav.selected = 0;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on the strip.
      nav.selected = 1;
    }
    nav.top = 0;
    requestUpdate();
  });
}

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
  if (index < 0 || index >= TAB_SLOTS) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  sortOrder = orderForTab(index, descendingTabs);
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  activeTabIndex = index;
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab()) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (debugEmpty_) return 0;
  if (hasFilter()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const { return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount(); }

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (hasFilter()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

bool LibraryListActivity::groupable() const { return !degraded && !isRecentSort(sortOrder) && bookRowCount() > 0; }

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  gridEpoch++;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  if (!hasFilter()) return;

  const std::string needle = library::fold(query);
  const bool chipOn = chipFilter != SourceChip::All;
  // A Latin query also runs the Pinyin matcher, so "dushu" reaches 读书 and "ds"
  // its initials. It is deliberately an extra channel, not a replacement: the
  // fold match above still answers English titles, and a matcher miss costs
  // only CPU — the per-book author read below stays exactly as before.
  const pinyin::PinyinTables* pinyinTables =
      (looksLikePinyinQuery(query) && pinyin::PinyinDb::shared().loaded()) ? &pinyin::PinyinDb::shared().tables()
                                                                           : nullptr;
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  std::string path;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (chipOn) {
      // Source predicate: the three named chips select disjoint folders —
      // /JJWXC/ (the app's export shelf), /WeRead/ (books WeRead fully
      // downloaded to the card) and 本地 (everything else the index picked up).
      path.clear();
      if (!index.readPath(record, path)) continue;
      const bool isJjwxc = path.rfind("/JJWXC/", 0) == 0;
      const bool isWeRead = path.rfind("/WeRead/", 0) == 0;
      switch (chipFilter) {
        case SourceChip::Jjwxc:
          if (!isJjwxc) continue;
          break;
        case SourceChip::WeRead:
          if (!isWeRead) continue;
          break;
        case SourceChip::Local:
          if (isJjwxc || isWeRead) continue;
          break;
        case SourceChip::All:
          break;
      }
      if (query.empty()) {
        matches[matchCount++] = static_cast<uint16_t>(row);
        continue;
      }
    }
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    if (pinyinTables != nullptr &&
        pinyinTables->match(record.fold, record.foldLen, query.c_str(), query.size())) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) &&
        (library::matchesQuery(library::fold(author), needle) ||
         (pinyinTables != nullptr && pinyinTables->match(author.c_str(), author.size(), query.c_str(), query.size())))) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (!tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->syncShelfAndUpdates();
}

void LibraryListActivity::chipActionTrampoline(const fui::ActionEvent& event, void* user) {
  static_cast<LibraryListActivity*>(user)->setSourceChip(event.value);
}

void LibraryListActivity::setSourceChip(const int value) {
  const auto chip = static_cast<SourceChip>(value);
  if (chip == chipFilter) return;
  chipFilter = chip;
  applyFilter();
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::sortActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptSortMenu();
}

void LibraryListActivity::promptSortMenu() {
  const char* options[] = {tr(STR_LIBRARY_TAB_RECENT), tr(STR_LIBRARY_TAB_TITLE), tr(STR_LIBRARY_TAB_AUTHOR),
                           tr(STR_SORT_REVERSE)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), tr(STR_LIBRARY), options, 4, activeTabIndex, [this](const int choice) {
    swallowHeldReleases();
    if (choice >= 0 && choice < TAB_SLOTS) {
      selectTab(choice, false);
    } else if (choice == 3) {
      selectTab(activeTabIndex, true);
    }
    requestUpdate();
  });
  // show() itself does not repaint; without this the menu stayed invisible
  // until the next unrelated update (caught in the sim).
  requestUpdate();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
void LibraryListActivity::appendSourceSuffix(const int entry, std::string& author) {
  std::string path;
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(entry)].path;
  } else {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path)) return;
  }
  // Subtitle suffix naming the source folder, for both app shelves.
  const char* sourceTag = nullptr;
  if (path.rfind("/JJWXC/", 0) == 0) {
    sourceTag = I18N.get(StrId::STR_JJWXC_TITLE);
  } else if (path.rfind("/WeRead/", 0) == 0) {
    sourceTag = I18N.get(StrId::STR_WEREAD_TITLE);
  }
  if (sourceTag == nullptr) return;
  if (!author.empty()) author += " \xc2\xb7 ";
  author += sourceTag;

  // "· N new": the update snapshot was written by the app's online-shelf walk.
  // Absent snapshot (never synced, or added to the account on the phone just
  // now) means UNKNOWN, so nothing is appended -- saying "up to date" would be
  // a claim this device has no evidence for.
  uint32_t newChapters = 0;
#ifdef BOARD_HAS_PSRAM
  if (path.rfind("/JJWXC/", 0) == 0) newChapters = jjwxcUpdates::gapForPath(path.c_str()).newChapters;
#endif
  if (newChapters > 0) {
    char tail[48];
    snprintf(tail, sizeof(tail), I18N.get(StrId::STR_JJWXC_HAS_NEW_FMT),
             static_cast<unsigned>(std::min<uint32_t>(newChapters, 9999)));
    author += " \xc2\xb7 ";
    author += tail;
  }
}

bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    const auto& book = books[static_cast<size_t>(entry)];
    title = book.title;
    author = book.author;
    if (fileName) *fileName = book.path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  // The empty-source invitation: it is the only control on that screen, so both
  // Confirm and a tap on the pill open the app whose shelf is empty.
  if (emptySourceTitle() != nullptr) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openEmptySource();
      return true;
    }
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasScreenTapped(tapX, tapY) && emptySourceRect_.width > 0 &&
        tapX >= emptySourceRect_.x && tapX < emptySourceRect_.x + emptySourceRect_.width &&
        tapY >= emptySourceRect_.y && tapY < emptySourceRect_.y + emptySourceRect_.height) {
      openEmptySource();
      return true;
    }
  }

  // The three "put a first book here" rows of the empty shelf (frame 15). Their
  // rects are recorded by the render pass, the same rule as the invitation pill;
  // Confirm takes the first row so the screen is never a dead end on a keypad.
  if (emptyPathCount_ > 0) {
    int pathX = 0;
    int pathY = 0;
    if (mappedInput.wasScreenTapped(pathX, pathY)) {
      for (int i = 0; i < emptyPathCount_; i++) {
        const Rect& rect = emptyPathRects_[i];
        if (rect.width > 0 && pathX >= rect.x && pathX < rect.x + rect.width && pathY >= rect.y &&
            pathY < rect.y + rect.height) {
          openEmptyPath(i);
          return true;
        }
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openEmptyPath(0);
      return true;
    }
  }

  // Vertical swipe steps one screen, same travel as the held page keys. Kept on
  // moveRingTo(): routing it through ListNav::requestScroll() left the shelf
  // completely unresponsive on the device, and the reason is not call order —
  // syncToProps() applies scrollBy() AFTER the follow, so the scroll is not being
  // cancelled. What is left is the viewport measurement: scrollBy() clamps
  // top + delta into [0, count - visibleRows], so if visibleRows is measured larger
  // than the real page (grid rows vs item count) every scroll clamps to 0 and the
  // window never moves — which also explains why the highlight moved but the side
  // bar did not. That needs listVisibleRows()/scrollBy() read before changing this
  // again; until then the working gesture stays.
  // Vertical swipe steps one screen, same travel as the held page keys.
  // KNOWN LIMITATION: this moves the ring/selection (the shelf's window follows the
  // focused row), so pages turn but the side scroll bar and the visible window do not
  // scroll on their own. Routing it through ListNav does not work here: this custom
  // paging path never calls syncToProps(), so requestScroll()'s queue has no consumer
  // (visibleRows/drawnRows stay at their 1/0 initial values, confirmed on device), and
  // driving nav.top directly advances the counter while the shelf keeps rendering from
  // props.itemsWindowFirst — i.e. the window is not derived from nav.top at all.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int count = listCount();
    if (count > 0) {
      if (usesCoverGrid()) {
        // One page turn = one PAGE of rows, not one row: the first page already shows
        // the top rows in full, so stepping a single row left the picture looking
        // unchanged and needed a second swipe to reach the truncated row. Step by the
        // measured page height (gridPageRows_) and bring the ring to the new page's
        // first cell, so the counter and the picture move together.
        const int rows = (count + kGridCols - 1) / kGridCols;
        const int pageRows = std::max(1, gridPageRows_);
        const int maxTop = std::max(0, rows - pageRows);
        const int dir = swipe == MappedInputManager::SwipeDir::Up ? 1 : -1;
        const int next = std::clamp(gridTopRow + dir * pageRows, 0, maxTop);
        if (next != gridTopRow) {
          gridTopRow = next;
          moveRingTo(std::min(next * kGridCols, count - 1) + 1);
        }
      } else {
        const auto& nav = activeNav();
        if (swipe == MappedInputManager::SwipeDir::Up) {
          moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
        } else {
          moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
        }
      }
    }
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (isRecentSort(sortOrder)) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) promptDeleteBook(selectedEntry());
    } else if (!usesCoverGrid() && !groupsCollapsed && groupable()) {
      // Grouping is a list-mode concept; the grid stays flat.
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  auto& nav = activeNav();
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = tabsFocused();
  }
  buttonNavigator.onNextPress([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if ((!navigationStartedOnTabs || degraded) && count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // Search is an activation: defer it so holding Previous can still step tabs.
  buttonNavigator.onPreviousRelease([this] {
    if (navigationStartedOnTabs && tabsFocused() && !degraded) openSearch();
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count, &nav] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count, &nav] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  const bool grouped = !isRecentSort(sortOrder);

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the two-line
  // rows edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  std::string rowFile;
  rowFile.reserve(128);
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  // The window must follow the focused row. This custom paging path hands the SDK an
  // explicit window (props.itemsWindowFirst) and nothing derives it from the
  // selection, so a swipe moved the focus (page counter 1/8 -> 4/8) while the visible
  // window stayed put until the focus left it entirely — and the side scroll bar,
  // which is drawn from the page start, never moved at all. Keeping the focused row
  // at the top of the window is exactly what "step one screen" means here.
  const int visibleRows = std::max(1, static_cast<int>(nav.pageRows()));
  const int windowStart =
      std::clamp(static_cast<int>(nav.selected.load()), 0, std::max(0, count - visibleRows));
  props.topIndex = static_cast<uint16_t>(windowStart);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else {
      if (!rowTextFor(entry, title, author, &rowFile)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped) {
        appendSourceSuffix(entry, author);
        if (!author.empty()) item.subtitle = author.c_str();
      }
    }

    item.label = title.c_str();
    // Group headings stay bare; every book row gets its file-type icon.
    if (!groupsCollapsed && !rowFile.empty()) item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  const bool cpyChrome = UITheme::getInstance().usesCpyLayout();
  if (cpyChrome) {
    // refreshed UI frame 02: the shared mast (drawn by drawChrome) carries the page
    // label, duck, clock and battery. Below it this screen owns a headline row
    // (48px serif title, shelf count, quiet search/sync) and the source tab
    // strip. The fui header is not drawn at all: its bordered action buttons
    // are the pre-v5 idiom and the design's header actions are quiet glyphs.
    drawHeadline(screen);
    drawSourceTabs(screen);
    return;
  }

  const int16_t headerTop = static_cast<int16_t>(metrics.topPadding);
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.borderEdges = fui::EdgeBottom;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
  }
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
  }
  const auto frameRect = screen.frame().screen();
  const int16_t headerHeight = static_cast<int16_t>(metrics.headerHeight);
  fui::header(screen.frame(), fui::Rect{frameRect.x, headerTop, frameRect.width, headerHeight}, header);
}

// Frame 02's headline row: the page title in the mockups' 48px Song face, and
// on the right the shelf count plus the two actions the fui header used to
// carry (search, and the rebuild/JJWWXC-update sync). Quiet glyphs in a 64px
// hit box: no bordered buttons, no second copy of the clock/battery band.
void LibraryListActivity::drawHeadline(UiScreen& screen) {
  const Rect page = pageContentRect();
  // Screen-relative margins, the same origin the cover grid and the invitation
  // pill use, so the whole CPY shelf shares one left edge.
  const int left = kGridSidePad;
  const int right = renderer.getScreenWidth() - kGridSidePad;
  const int height = kCpyHeadlineHeight;
  const int y = page.y;

  const bool showActions = !degraded;
  constexpr int kIconSize = 32;
  // Right-aligned action cluster, laid out from the right edge inwards.
  int cursor = right;
  Rect syncHit{};
  Rect searchHit{};
  if (showActions) {
    if (mappedInput.hasTouch()) {
      syncHit = Rect{cursor - kCpyTouchTarget, y + (height - kCpyTouchTarget) / 2, kCpyTouchTarget, kCpyTouchTarget};
      cursor -= kCpyTouchTarget;
    }
    searchHit = Rect{cursor - kCpyTouchTarget, y + (height - kCpyTouchTarget) / 2, kCpyTouchTarget, kCpyTouchTarget};
    cursor -= kCpyTouchTarget;
  }

  // Shelf count ("全部 128 本" / "All 128"): the index's own book count, so it
  // is a fact even before the per-source pass has run. High-density builds put
  // it at the right, before the actions, under the serif page title; the
  // low-density mast already prints the page name, so the row leads with it
  // instead of saying the title twice.
  ensureShelfCounts();
  const int countTotal = shelfCounts_.valid ? static_cast<int>(shelfCounts_.total)
                                            : static_cast<int>(index.isOpen() ? index.bookCount() : 0);
  char countText[48] = {};
  snprintf(countText, sizeof(countText), I18N.get(StrId::STR_ALL_BOOKS_FMT),
           static_cast<unsigned>(countTotal));
  const int countY = y + (height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  const int countRight = (showActions ? cursor : right) - 22;
  const int countW = renderer.getTextWidth(UI_10_FONT_ID, countText);
  const int countX = UiHighDpiProfile::enabled ? countRight - countW : left;
  if (countX >= left && countX + countW <= countRight) {
    renderer.drawText(UI_10_FONT_ID, countX, countY, countText);
  } else {
    // No room beside a long query title: the title wins, the count yields.
    countText[0] = '\0';
  }

  if (showActions) {
    const int iconY = y + (height - kIconSize) / 2;
    drawMaskIcon(renderer, icon_search_32, searchHit.x + (searchHit.width - kIconSize) / 2, iconY);
    screen.frame().hit(fui::Rect{static_cast<int16_t>(searchHit.x), static_cast<int16_t>(searchHit.y),
                                 static_cast<int16_t>(searchHit.width), static_cast<int16_t>(searchHit.height)},
                       ACTION_SEARCH, 0, fui::InputTouch);
    if (syncHit.width > 0) {
      drawMaskIcon(renderer, icon_refresh_cw_32, syncHit.x + (syncHit.width - kIconSize) / 2, iconY);
      screen.frame().hit(fui::Rect{static_cast<int16_t>(syncHit.x), static_cast<int16_t>(syncHit.y),
                                   static_cast<int16_t>(syncHit.width), static_cast<int16_t>(syncHit.height)},
                         ACTION_REBUILD, 0, fui::InputTouch);
    }
  }

  // Page title (the search query while one is active, exactly as the mast
  // showed it). High-density only: the low-density mast already carries the
  // page label in this exact face.
  if constexpr (UiHighDpiProfile::enabled) {
    const char* title = headerTitle();
    if (title != nullptr && *title != '\0') {
      constexpr int titleFont = serifTitleFontId();
      const int lineH = renderer.getLineHeight(titleFont);
      const int titleRight = countText[0] != '\0' ? countX - 16 : right;
      const std::string shown =
          renderer.truncatedText(titleFont, title, std::max(1, titleRight - left), EpdFontFamily::BOLD);
      renderer.drawText(titleFont, left, y + (height - lineH) / 2, shown.c_str(), true, EpdFontFamily::BOLD);
    }
  }
}

// Frame 02's source tab strip: label + real count per source, 4px underline
// under the active one, the design's 1px hairline on top and 2px rule below.
// The sort order keeps a quiet text+triangle entry at the row's right end (it
// used to be a bordered capsule) so it does not vanish from the screen.
void LibraryListActivity::drawSourceTabs(UiScreen& screen) {
  const Rect page = pageContentRect();
  const int left = page.x + kGridSidePad;
  const int right = page.x + page.width - kGridSidePad;
  const int top = page.y + kCpyHeadlineHeight;
  const int height = kCpyTabsHeight;
  constexpr int font = UI_10_FONT_ID;
  const int lineH = renderer.getLineHeight(font);
  const int textY = top + (height - lineH) / 2 - 6;
  // The design hangs the 4px bar under the label with ~10px of air.
  const int underlineY = textY + lineH + 8;

  renderer.drawLine(left, top, right - 1, top, true);

  ensureShelfCounts();
  const bool countsKnown = shelfCounts_.valid;

  // Sort entry (quiet): current order + a drawn triangle so no glyph is needed.
  const char* sortLabel = tabLabelFor(activeTabIndex);
  constexpr int kArrow = 18;
  const int sortW = renderer.getTextWidth(font, sortLabel) + kArrow + 8;
  const int sortX = right - sortW;
  renderer.drawText(font, sortX, textY, sortLabel, true, EpdFontFamily::BOLD);
  {
    const int triX = sortX + sortW - kArrow + 4;
    const int triY = textY + lineH / 2;
    const int xs[3] = {triX, triX + 12, triX + 6};
    const int ys[3] = {triY - 4, triY - 4, triY + 5};
    renderer.fillPolygon(xs, ys, 3);
  }
  screen.frame().hit(fui::Rect{static_cast<int16_t>(sortX - 8), static_cast<int16_t>(top),
                               static_cast<int16_t>(sortW + 8), static_cast<int16_t>(height)},
                     ACTION_SORT, 0, fui::InputTouch);

  struct TabDef {
    StrId id;
    SourceChip value;
    uint16_t count;
  };
  const TabDef tabs[] = {
      {StrId::STR_SHELF_FILTER_ALL, SourceChip::All, shelfCounts_.total},
      {StrId::STR_SHELF_FILTER_LOCAL, SourceChip::Local, shelfCounts_.local},
      // Reuses the app's own title string — no new i18n key for a label that
      // already exists in all 35 languages.
      {StrId::STR_WEREAD_TITLE, SourceChip::WeRead, shelfCounts_.weread},
      {StrId::STR_SHELF_FILTER_JJWXC, SourceChip::Jjwxc, shelfCounts_.jjwxc},
  };

  const int tabsRight = sortX - 16;
  constexpr int kTabCount = static_cast<int>(sizeof(tabs) / sizeof(tabs[0]));
  // Measure first so the four tabs always fit between the margin and the sort
  // entry. Two fallbacks, cheapest first: the gap tightens, then the counts are
  // dropped. A tab is never dropped — that would take a filter away.
  int labelW[kTabCount] = {};
  int countW[kTabCount] = {};
  int totalW = 0;
  bool showCounts = countsKnown;
  for (int i = 0; i < kTabCount; i++) {
    labelW[i] = renderer.getTextWidth(font, I18N.get(tabs[i].id));
    totalW += labelW[i];
  }
  const int labelsOnlyW = totalW;
  if (showCounts) {
    for (int i = 0; i < kTabCount; i++) {
      char count[8];
      snprintf(count, sizeof(count), "%u", static_cast<unsigned>(tabs[i].count));
      countW[i] = 12 + renderer.getTextWidth(font, count);
      totalW += countW[i];
    }
  }
  constexpr int kMinGap = 12;
  const int room = tabsRight - left;
  if (totalW + (kTabCount - 1) * kMinGap > room && showCounts) {
    showCounts = false;
    totalW = labelsOnlyW;
    for (int i = 0; i < kTabCount; i++) countW[i] = 0;
  }
  int gap = kCpyTabGap;
  if (kTabCount > 1) {
    gap = std::clamp((room - totalW) / (kTabCount - 1), kMinGap, static_cast<int>(kCpyTabGap));
  }

  int x = left;
  for (int i = 0; i < kTabCount; i++) {
    const TabDef& tab = tabs[i];
    const char* label = I18N.get(tab.id);
    const int tabW = labelW[i] + countW[i];
    if (x + tabW > tabsRight) break;  // never run under the sort entry
    const bool selected = chipFilter == tab.value;
    renderer.drawText(font, x, textY, label, true, selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (countW[i] > 0) {
      char count[8];
      snprintf(count, sizeof(count), "%u", static_cast<unsigned>(tab.count));
      renderer.drawText(font, x + labelW[i] + 12, textY, count);
    }
    if (selected) {
      renderer.fillRect(x, underlineY, tabW, kCpyTabUnderline, true);
    }
    screen.frame().hit(fui::Rect{static_cast<int16_t>(x - 6), static_cast<int16_t>(top),
                                 static_cast<int16_t>(tabW + 12), static_cast<int16_t>(height)},
                       ACTION_CHIP, static_cast<int16_t>(tab.value), fui::InputTouch);
    x += tabW + gap;
  }

  // The 2px rule the design closes the strip with.
  renderer.fillRect(left, top + height, right - left, kCpyTabsRule, true);
}

// One pass over the index per generation. Counts are facts about the card:
// when the index is not readable, nothing is cached and the tabs draw their
// labels alone rather than a zero that would read as "you have none".
void LibraryListActivity::ensureShelfCounts() {
  if (shelfCounts_.valid && shelfCounts_.stamp == indexStamp) return;
  if (!index.isOpen()) return;

  ShelfCounts counts;
  counts.stamp = indexStamp;
  counts.total = index.bookCount();
  std::string path;
  library::ClixRecord record{};
  for (uint16_t ordinal = 0; ordinal < counts.total; ++ordinal) {
    if (!index.readRecord(ordinal, record)) break;  // ioFailed: keep what we counted
    path.clear();
    if (!index.readPath(record, path)) continue;
    if (path.rfind("/JJWXC/", 0) == 0) {
      counts.jjwxc++;
    } else if (path.rfind("/WeRead/", 0) == 0) {
      counts.weread++;
    } else {
      counts.local++;
    }
  }
  counts.valid = true;
  // The grid also shows recent books the index has not picked up yet
  // (bookRowCount() adds pinned - overlap). Counting only the index made the
  // tabs say "全部 7" next to a grid of eight covers. Count the same universe.
  if (activeTabIndex == RECENT_TAB && !hasFilter() && isDescending(sortOrder)) {
    const auto& books = RECENT_BOOKS.getBooks();
    const int indexed = static_cast<int>(counts.total);
    for (int i = 0; i < pinnedTotal && i < static_cast<int>(books.size()); i++) {
      if (pinnedAscRows[i] != 0xFFFF && pinnedAscRows[i] < indexed) continue;  // already counted
      counts.total++;
      const std::string& path = books[static_cast<size_t>(i)].path;
      if (path.rfind("/JJWXC/", 0) == 0) {
        counts.jjwxc++;
      } else if (path.rfind("/WeRead/", 0) == 0) {
        counts.weread++;
      } else {
        counts.local++;
      }
    }
  }
  shelfCounts_ = counts;
  LOG_DBG("LIB", "source counts: all=%u local=%u weread=%u jjwxc=%u", static_cast<unsigned>(counts.total),
          static_cast<unsigned>(counts.local), static_cast<unsigned>(counts.weread),
          static_cast<unsigned>(counts.jjwxc));
}

namespace {
memory::ByteBuffer g_libSnapshot;
size_t g_libSnapshotSize = 0;
bool g_libSnapshotRequested = false;
}  // namespace

void LibraryListActivity::debugRequestSnapshot() {
  g_libSnapshotRequested = true;
  requestUpdate();
}

const uint8_t* LibraryListActivity::debugSnapshotData(size_t& size) {
  size = g_libSnapshotSize;
  return g_libSnapshot.get();
}

void LibraryListActivity::debugClearSnapshot() {
  g_libSnapshot = {};
  g_libSnapshotSize = 0;
  g_libSnapshotRequested = false;
}

void LibraryListActivity::beforeDisplayHook() {
  // Empty shelves draw the illustration after the UI canvas; the empty branch
  // calls captureSnapshot() itself just before its own push.
  if (bookRowCount() == 0) return;
  captureSnapshot();
}

void LibraryListActivity::captureSnapshot() {
  if (!g_libSnapshotRequested) return;
  g_libSnapshotRequested = false;
  const size_t size = renderer.getBufferSize();
  g_libSnapshot = memory::makePsramByteBufferNoThrow(size);
  if (!g_libSnapshot) {
    g_libSnapshotSize = 0;
    return;
  }
  memcpy(g_libSnapshot.get(), renderer.getFrameBuffer(), size);
  g_libSnapshotSize = size;
}

void LibraryListActivity::drawChrome() {
  // CPY: the standard main-tab chrome (status strip, duck title band, tab
  // bar); the legacy fui header stays the title owner for INX and for the
  // pre-integration paths.
  if (!UITheme::getInstance().usesCpyLayout()) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The mast owns the page label; with the actions moved into the headline row
  // no right-hand reserve is needed any more — the mast itself truncates to
  // the clock/battery cluster.
  const char* shownTitle = headerSearchTitle.empty() ? tr(STR_LIBRARY) : headerSearchTitle.c_str();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, shownTitle);
}

bool LibraryListActivity::usesCoverGrid() const {
  return UITheme::getInstance().usesCpyLayout() &&
         SETTINGS.inxLibraryLayout == static_cast<uint8_t>(InxItemLayout::Icons);
}

// Frame 02's cover furniture: the source seal bottom-left, the reading
// progress bottom-right and the new-chapter badge top-right. Every piece is
// drawn only when the data behind it is known.
void LibraryListActivity::drawGridBadges(const int slot, const Rect& cover) const {
  if (slot < 0 || slot >= kGridCoverSlots) return;
  constexpr int font = SMALL_FONT_ID;
  const int lineH = renderer.getLineHeight(font);
  const int badgeGap = 6;

  // Bottom-left: which shelf this book came from, as a 28px seal.
  const uint8_t source = gridSlotSource[slot];
  if (source != 0) {
    constexpr int sealSize = 28;
    const std::string glyph = sealGlyphFor(source);
    drawSourceSeal(renderer, cover.x + badgeGap, cover.y + cover.height - sealSize - badgeGap, sealSize,
                   glyph.c_str());
  }

  // Bottom-right: reading progress. 0xFF means the stats store has no record
  // for this path, which draws NOTHING — "0 %" would be a claim we cannot make.
  if (gridSlotProgress[slot] != 0xFF) {
    char pct[12];
    snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(gridSlotProgress[slot]));
    const int w = renderer.getTextWidth(font, pct) + 12;
    const int h = lineH + 6;
    const int x = cover.x + cover.width - w - badgeGap;
    const int y = cover.y + cover.height - h - badgeGap;
    renderer.fillRoundedRect(x, y, w, h, 4, Color::White);
    renderer.drawRoundedRect(x, y, w, h, 1, 4, true);
    renderer.drawText(font, x + 6, y + 3, pct);
  }

  // Top-right: how many chapters the card is behind, as the design's "+N 章".
  // Only when the snapshot says so; an unknown state draws nothing (never a
  // misleading "0"). The label is ellipsised so it cannot leave the cover.
  if (gridSlotNew[slot] > 0) {
    char chapters[40];
    snprintf(chapters, sizeof(chapters), I18N.get(StrId::STR_JJWXC_CHAPTERS_COUNT),
             static_cast<unsigned long>(gridSlotNew[slot]));
    char badge[48];
    snprintf(badge, sizeof(badge), "+%s", chapters);
    const std::string shown = renderer.truncatedText(font, badge, std::max(1, cover.width - 28));
    const int w = renderer.getTextWidth(font, shown.c_str()) + 16;
    const int h = lineH + 6;
    const int x = cover.x + cover.width - w - badgeGap;
    renderer.fillRoundedRect(x, cover.y + badgeGap, w, h, 6, Color::Black);
    renderer.drawText(font, x + 8, cover.y + badgeGap + 3, shown.c_str(), false);
  }
}

void LibraryListActivity::buildCoverGrid(UiScreen& screen) {
  const int count = bookRowCount();
  if (count <= 0) return;

  const int screenWidth = renderer.getScreenWidth();
  const int cellWidth = (screenWidth - 2 * kGridSidePad - (kGridCols - 1) * kGridCellGap) / kGridCols;
  const int coverWidth = cellWidth;
  // The mockup's 188:250 cover, scaled to the four-column cell. Runtime (not a
  // constant) because the panel width differs between boards; the thumbnail
  // cache is keyed by height, so each profile keeps its own set.
  const int coverHeight = std::max(1, cellWidth * kGridCoverAspectH / kGridCoverAspectW);
  constexpr int titleFont = gridTitleFontId();
  const int titleLineH = renderer.getLineHeight(titleFont);
  const int rowPitch = coverHeight + kGridTitleGapTop + titleLineH + kGridRowGapBottom;

  const fui::Rect body = screen.body();
  const GfxRenderer::ClipScope gridClip(renderer, body.x, body.y, body.width, body.height);
  const int gridTop = body.y + kGridTopInset;
  const int rows = (count + kGridCols - 1) / kGridCols;
  // Page size counts COMPLETE rows only — that is what the viewport can scroll by.
  // The row that does not fit is still DRAWN (loop below goes one row further) and
  // clipped at the body edge, so it peeks above the tab bar instead of vanishing; a
  // swipe can then bring that row fully into view (maxTopRow is rows - visibleRows,
  // so the partial row is reachable). Rounding this up instead made the page "fit"
  // and left maxTopRow at 0, i.e. nothing to scroll.
  const int available = static_cast<int>(body.height) - kGridTopInset;
  const int visibleRows = std::max(1, available / rowPitch);

  // Whole rows only: a half-cover peeking under the footer read as a broken row
  // (device feedback 2026-10-26). The design shows two complete rows and a quiet
  // footer strip; paging moves by visibleRows and the last page is bottom-aligned.
  const int maxTopRow = std::max(0, rows - visibleRows);
  gridPageRows_ = visibleRows;
  // Keep the focused cell on screen for key navigation, without overriding the
  // swipe pager: only scroll when the selection actually left the window.
  const int selRow = selectedEntry() / kGridCols;
  if (selRow < gridTopRow) gridTopRow = selRow;
  if (selRow >= gridTopRow + visibleRows) gridTopRow = selRow - visibleRows + 1;
  gridTopRow = std::clamp(gridTopRow, 0, maxTopRow);

  const bool showSelection = showMainTabContentSelection();
  const int selected = selectedEntry();
  // Remember what each slot showed last frame: a slot whose entry is
  // unchanged keeps its cover state; anything else restarts at Unchecked, so
  // scrolling re-prewarms the covers that scroll into view.
  int16_t prevSlotEntry[kGridCoverSlots];
  for (int i = 0; i < kGridCoverSlots; i++) {
    prevSlotEntry[i] = gridSlotEntry[i];
    gridSlotEntry[i] = -1;
  }

  // Clip VERTICALLY to the body only — clipping horizontally to body.width cut the
  // side scroll bar (drawn at x=0..screenWidth) right off the screen.
  const GfxRenderer::ClipScope clip(renderer, 0, body.y, screenWidth, body.height);
  for (int row = 0; row < visibleRows; row++) {
    const int rowIndex = gridTopRow + row;
    if (rowIndex >= rows) break;
    const int rowY = gridTop + row * rowPitch;
    std::string rowTitles[kGridCols];
    // A row that does not fill the grid keeps its cells left-aligned; the
    // vacant positions carry no decoration at all.
    const int rowCols = std::min(kGridCols, count - rowIndex * kGridCols);

    for (int col = 0; col < rowCols; col++) {
      const int entry = rowIndex * kGridCols + col;
      const int cellX = kGridSidePad + col * (cellWidth + kGridCellGap);
      const Rect cover{cellX, rowY, coverWidth, coverHeight};

      std::string author;
      rowTextFor(entry, rowTitles[col], author);

      const int slot = row * kGridCols + col;
      if (slot < kGridCoverSlots) {
        if (prevSlotEntry[slot] != entry || gridSlotEpoch[slot] != gridEpoch) {
          gridCovers[slot] = GridCover::Unchecked;
          gridCoverPaths[slot].clear();
          // Source seal, "N new" and progress. Computed here, with the cover
          // slot, so a repaint costs nothing and only a new entry/epoch
          // touches the card. No path -> no source -> no seal.
          gridSlotSource[slot] = 0;
          gridSlotNew[slot] = 0;
          gridSlotProgress[slot] = 0xFF;
          std::string badgePath;
          if (slotBookPath(entry, badgePath)) {
            if (badgePath.rfind("/WeRead/", 0) == 0) {
              gridSlotSource[slot] = 2;
            } else if (badgePath.rfind("/JJWXC/", 0) == 0) {
              gridSlotSource[slot] = 3;
#ifdef BOARD_HAS_PSRAM
              const auto gap = jjwxcUpdates::gapForPath(badgePath.c_str());
              gridSlotNew[slot] = static_cast<uint16_t>(std::min<uint32_t>(gap.newChapters, 999));
#endif
            } else {
              gridSlotSource[slot] = 1;  // local
            }
            const ReadingBookStats* stats = READING_STATS.findBook(badgePath);
            if (stats != nullptr) gridSlotProgress[slot] = stats->lastProgressPercent;
          }
        }
        gridSlotEntry[slot] = static_cast<int16_t>(entry);
        gridSlotEpoch[slot] = gridEpoch;
        // Cached thumbnails resolve inline so a sort/filter switch redraws
        // covers in the SAME frame when the cache already exists (device
        // feedback: the switch looked delayed); generation stays with the
        // per-frame prewarm below.
        if (gridCovers[slot] == GridCover::Unchecked) {
          std::string bookPath;
          if (slotBookPath(entry, bookPath)) {
            const std::string cached = BookCoverLoader::cachedThumbnail(bookPath, coverHeight);
            if (!cached.empty()) {
              gridCoverPaths[slot] = cached;
              gridCovers[slot] = GridCover::Ready;
            }
          }
        }
      }

      bool drewCover = false;
      if (slot < kGridCoverSlots && gridCovers[slot] == GridCover::Ready && !gridCoverPaths[slot].empty()) {
        HalFile file;
        if (Storage.openFileForRead("LIB", gridCoverPaths[slot], file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
            drewCover = renderer.drawBitmapCropToFill(bitmap, cover.x, cover.y, cover.width, cover.height);
          }
        }
        if (!drewCover) {
          gridCovers[slot] = GridCover::Missing;
          gridCoverPaths[slot].clear();
        }
      }
      if (!drewCover) {
        // No cover yet: the framed placeholder naming the book horizontally.
        // The vertical spine + inner title reading is retired (user feedback,
        // 2026-10-16): it duplicated the title line below, and on the one real
        // coverless book (the bundled guide's EPUB ships no cover image) it
        // read as a stuck artifact rather than a placeholder.
        renderer.drawRoundedRect(cover.x, cover.y, cover.width, cover.height, 1, kGridCoverRadius, true);
        const auto lines = wrapCoverTitle(renderer, SMALL_FONT_ID, rowTitles[col].c_str(),
                                          std::max(1, cover.width - 28), 4);
        const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
        int ty = cover.y + (cover.height - static_cast<int>(lines.size()) * lineH) / 2;
        for (const auto& line : lines) {
          const int w = renderer.getTextWidth(SMALL_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
          renderer.drawText(SMALL_FONT_ID, cover.x + (cover.width - w) / 2, ty, line.c_str(), true,
                            EpdFontFamily::BOLD);
          ty += lineH;
        }
      }
      if (slot < kGridCoverSlots) drawGridBadges(slot, cover);
      if (showSelection && entry == selected) {
        renderer.drawRoundedRect(cover.x - 3, cover.y - 3, cover.width + 6, cover.height + 6, 3,
                                 kGridCoverRadius + 3, true);
      }

      // Title: one line, left-aligned under the cover (frame 02), ellipsised.
      const int titleY = rowY + coverHeight + kGridTitleGapTop;
      const std::string shown =
          renderer.truncatedText(titleFont, rowTitles[col].c_str(), std::max(1, cellWidth - 4));
      renderer.drawText(titleFont, cellX, titleY, shown.c_str());

      // Cover and title are one cell: both open the book, both take the hold.
      screen.frame().hit(
          fui::Rect{static_cast<int16_t>(cellX), static_cast<int16_t>(rowY), static_cast<int16_t>(cellWidth),
                    static_cast<int16_t>(coverHeight + kGridTitleGapTop + titleLineH)},
          ACTION_ROW, static_cast<int16_t>(entry), fui::InputTouch | fui::InputLongPress);
    }
  }

  GUI.drawSideScrollBar(renderer, Rect{0, gridTop, screenWidth, static_cast<int>(body.height) - kGridTopInset}, count,
                        gridTopRow * kGridCols, visibleRows * kGridCols);

  // Device feedback 2026-10-26: two complete rows left a blank bay above the
  // position readout. The design closes the grid with a quiet footer strip
  // (rule + the sort entry on the right), so the bay reads as a band instead of
  // leftover padding. Registered here, where the frame is still being built —
  // drawFooter() runs after that lock.
  // The footer strip sits one pixel below the clipped content band, so it must
  // be drawn after the grid clip is lifted (the guard below restores the
  // original rect on scope exit regardless).
  renderer.clearClipRect();
  if (UITheme::getInstance().usesCpyLayout()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    // Close the grid immediately below its last complete row (the design's
    // footer strip) instead of at the body edge, which left a blank bay.
    const int drawnRows = std::min(visibleRows, rows - gridTopRow);
    const int lastRowBottom = gridTop + drawnRows * rowPitch - kGridRowGapBottom;
    const int footerTop = std::min(lastRowBottom + 11, body.y + body.height);
    const int left = body.x + kGridSidePad;
    const int right = body.x + body.width - kGridSidePad;
    renderer.drawLine(left, footerTop, right - 1, footerTop, true);
    const int textY = footerTop + 8;

    // The position readout moved here with the strip; drawing it in drawFooter
    // (below the tab band's reserved line) would now float in empty space.
    const int listTotal = listCount();
    if (listTotal > 0) {
      char buf[32];
      const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
      snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, listTotal);
      renderer.drawText(SMALL_FONT_ID, left, textY, buf, true);
    }

    constexpr int font = SMALL_FONT_ID;
    const char* sortLabel = tabLabelFor(activeTabIndex);
    constexpr int kArrow = 16;
    const int sortW = renderer.getTextWidth(font, sortLabel) + kArrow + 8;
    const int sortX = right - sortW;
    renderer.drawText(font, sortX, textY, sortLabel, true, EpdFontFamily::BOLD);
    const int triX = sortX + sortW - kArrow + 4;
    const int triY = textY + renderer.getLineHeight(font) / 2;
    const int xs[3] = {triX, triX + 10, triX + 5};
    const int ys[3] = {triY - 4, triY - 4, triY + 4};
    renderer.fillPolygon(xs, ys, 3);
    screen.frame().hit(fui::Rect{static_cast<int16_t>(sortX - 10), static_cast<int16_t>(footerTop),
                                 static_cast<int16_t>(sortW + 10),
                                 static_cast<int16_t>(renderer.getLineHeight(font) + metrics.verticalSpacing)},
                       ACTION_SORT, 0, fui::InputTouch);
  }
}

bool LibraryListActivity::slotBookPath(const int entry, std::string& path) {
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    path = books[static_cast<size_t>(entry)].path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path)) return false;
  return true;
}

bool LibraryListActivity::prepareNextGridCover() {
  // Runs after the frame is on the panel: cache probes and coverless books
  // are cheap and all go in one sweep; a book that actually PARSES stops the
  // pass (one generation per frame -- the InxRecent prewarm pattern).
  //
  // Only an ON-SCREEN slot becoming ready earns a panel push (visibleReady).
  // The off-screen prefetch below generates thumbnail files silently and never
  // pumps the panel itself: walking the whole index one push per book made
  // opening the library scan the panel once per cover (200 books = 200 pushes,
  // a GC16 flash every 6th) for thumbnails the user could not even see. The
  // prefetch now piggybacks on renders the visible slots or the user already
  // cause, so it stays warm ahead of scrolling without monopolising the panel.
  bool visibleReady = false;
  // Thumbnails are cached per requested height, so the prewarm must ask for the
  // same cover height the grid draws (four-column geometry, screen width).
  const int coverHeight = std::max(
      1, ((renderer.getScreenWidth() - 2 * kGridSidePad - (kGridCols - 1) * kGridCellGap) / kGridCols) *
             kGridCoverAspectH / kGridCoverAspectW);
  for (int slot = 0; slot < kGridCoverSlots; slot++) {
    if (gridCovers[slot] != GridCover::Unchecked || gridSlotEntry[slot] < 0) continue;
    const int entry = gridSlotEntry[slot];
    std::string path;
    if (!slotBookPath(entry, path)) {
      gridCovers[slot] = GridCover::Unavailable;
      continue;
    }
    bool generated = false;
    std::string thumb;
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      thumb = BookCoverLoader::ensureThumbnail(path, coverHeight, &generated);
    }
    if (thumb.empty()) {
      gridCovers[slot] = GridCover::Unavailable;
      continue;
    }
    gridCoverPaths[slot] = thumb;
    gridCovers[slot] = GridCover::Ready;
    visibleReady = true;
    if (generated) break;
  }

  // Phase 2, off-screen prefetch: keep walking the WHOLE index one cover per
  // frame, so a later sort or scroll finds its thumbnails already cached
  // (device feedback: switching sort re-generated covers with a visible
  // delay). Probes of cached entries are header reads and walk in one pass;
  // the first uncached book generates and stops the pass. Unlike the visible
  // slots this does NOT requestUpdate -- it advances on renders the user or the
  // on-screen slots already schedule, so it warms the cache without turning
  // each off-screen thumbnail into a full-panel scan. Ends for the session once
  // every entry has been seen; an index/filter change (epoch) restarts it.
  if (gridPrefetchEpoch != gridEpoch) {
    gridPrefetchEpoch = gridEpoch;
    gridPrefetchCursor = 0;
    gridPrefetchDone = false;
  }
  const int total = bookRowCount();
  while (!gridPrefetchDone && gridPrefetchCursor < total) {
    std::string path;
    if (!slotBookPath(gridPrefetchCursor, path)) {
      gridPrefetchCursor++;
      continue;
    }
    const int entry = gridPrefetchCursor;
    gridPrefetchCursor++;
    bool visibleSlot = false;
    for (int i = 0; i < kGridCoverSlots; i++) {
      if (gridSlotEntry[i] == entry) {
        visibleSlot = true;  // its own pass owns it
        break;
      }
    }
    if (visibleSlot) continue;
    bool generated = false;
    std::string thumb;
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      thumb = BookCoverLoader::ensureThumbnail(path, coverHeight, &generated);
    }
    if (generated) break;  // one off-screen generation per pass; no panel push
  }
  if (gridPrefetchCursor >= total) gridPrefetchDone = true;

  if (visibleReady) requestUpdate();
  return true;
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing);
  buildHeader(screen);
  if (UITheme::getInstance().usesCpyLayout()) {
    // CPY: the mast, the headline row and the source tabs take the top band
    // (drawn by drawChrome and drawHeadline/drawSourceTabs). The list stops
    // above the position readout, which sits in the last line above the tab
    // bar. Button hints only exist on boards without touch, where they still
    // occupy the bottom slot.
    const bool hintsVisible = !mappedInput.hasTouch() && SETTINGS.showButtonHints;
    const Rect page = pageContentRect();
    // Two readout lines: the readout itself plus the air the design puts between
    // the last grid row and the footer rule. Reserving one line let the readout
    // sit inside the content band, so "1/8 本" printed on top of a book title.
    screen.setContentMarginFromScreen(
        fui::Insets{static_cast<int16_t>(page.y + kCpyHeaderBandHeight),
                    static_cast<int16_t>(renderer.getScreenWidth() - page.x - page.width),
                    static_cast<int16_t>(renderer.getScreenHeight() - page.y - page.height + readoutReserved * 2 +
                                         (hintsVisible ? metrics.buttonHintsHeight : 0)),
                    static_cast<int16_t>(page.x)});
  } else {
    screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                  static_cast<int16_t>(metrics.buttonHintsHeight + readoutReserved), 0});
  }

  if (!degraded) buildTabBar(screen);
  if (bookRowCount() == 0) {
    // The placeholder is painted by render() after the frame: the theme's
    // drawEmptyState adds the CPY duck backdrop under the message, which a
    // plain centred-text component cannot do.
    return;
  }
  if (usesCoverGrid()) {
    buildCoverGrid(screen);
    return;
  }
  buildRows(screen);
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = listCount();
  if (count <= 0) return;
  // The CPY cover grid draws its readout inside the footer strip, directly
  // under the last row (see buildCoverGrid).
  if (UITheme::getInstance().usesCpyLayout() && usesCoverGrid() && bookRowCount() > 0) return;

  char buf[32];
  const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  // In the CPY footer band the right edge belongs to the sort entry, so the
  // readout anchors to the grid's left margin there (design footer strip).
  const bool cpyFooter = UITheme::getInstance().usesCpyLayout() && usesCoverGrid() && bookRowCount() > 0;
  const int x = cpyFooter ? pageContentRect().x + kGridSidePad
                          : renderer.getScreenWidth() - width - SIDE_PADDING;
  // CPY: the tab bar owns the screen bottom, so the readout moves up into
  // the line the content margins reserve above it.
  const Rect page = pageContentRect();
  const int y = UITheme::getInstance().usesCpyLayout()
                    ? page.y + page.height - renderer.getLineHeight(SMALL_FONT_ID) * 2 - metrics.verticalSpacing
                    : renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_LIBRARY);
}

void LibraryListActivity::drawHoldHelp() const {
  // Touch devices used to skip this line entirely, so the only route to the
  // row menu (rename / move / delete / sort-reverse) was invisible: the hint
  // was written for the keypad and silently dropped for the touch panel that
  // this hardware actually has. Long-press works there too, so show it.
  if (groupsCollapsed) return;
  const char* help = nullptr;
  if (tabsFocused() && !degraded)
    help = tr(STR_LIBRARY_HOLD_SORT);
  else if (!tabsFocused() && isRecentSort(sortOrder) && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // recent rows: hold opens the row menu
  else if (!tabsFocused() && deleteEligible() && listCount() > 0)
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  else if (!tabsFocused() && groupable())
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const Rect page = pageContentRect();
  const int y = UITheme::getInstance().usesCpyLayout()
                    ? page.y + page.height - lineHeight * 2 - metrics.verticalSpacing
                    : renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer, Rect{SIDE_PADDING, y, renderer.getScreenWidth() / 2 - SIDE_PADDING, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
const char* LibraryListActivity::emptySourceTitle() const {
  if (bookRowCount() != 0) return nullptr;
  // A search with no hits is about the query, not about the source: offering
  // "open WeRead" there would answer a question the user did not ask.
  if (!query.empty()) return nullptr;
  if (chipFilter == SourceChip::WeRead) return tr(STR_WEREAD_TITLE);
  if (chipFilter == SourceChip::Jjwxc) return tr(STR_JJWXC_TITLE);
  return nullptr;
}

const char* LibraryListActivity::emptyStateMessage() const {
  if (filterFailed) return tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
  const char* sourceTitle = emptySourceTitle();
  if (sourceTitle != nullptr) {
    char text[160];
    snprintf(text, sizeof(text), I18N.get(StrId::STR_LIBRARY_EMPTY_SOURCE_FMT), sourceTitle);
    emptySourceMessage_ = text;
    return emptySourceMessage_.c_str();
  }
  if (!hasFilter()) return tr(STR_LIBRARY_EMPTY);
  return tr(STR_LIBRARY_NO_RESULTS);
}

void LibraryListActivity::drawEmptySourceAction(const Rect& band) {
  emptySourceRect_ = Rect{};
  const char* sourceTitle = emptySourceTitle();
  if (sourceTitle == nullptr) return;
  char label[160];
  snprintf(label, sizeof(label), I18N.get(StrId::STR_LIBRARY_OPEN_SOURCE_FMT), sourceTitle);

  constexpr int kPadX = 36;
  constexpr int kHeight = 64;
  const int width = renderer.getTextWidth(UI_10_FONT_ID, label) + 2 * kPadX;
  const int x = (renderer.getScreenWidth() - width) / 2;
  // Bottom of the band: the empty message keeps its centred position above it.
  int y = band.y + band.height - kHeight - 24;
  if (y < band.y + kHeight) y = band.y + band.height - kHeight - 8;
  const int radius = kHeight / 2;
  renderer.fillRoundedRect(x, y, width, kHeight, radius, Color::Black);
  renderer.drawText(UI_10_FONT_ID, x + kPadX, y + (kHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2, label, false);
  emptySourceRect_ = Rect{x, y, width, kHeight};
}

void LibraryListActivity::openEmptySource() {
  if (emptySourceTitle() == nullptr) return;
  app.clearTapFlash();
#ifdef ENABLE_CHINESE_VERSION
  if (chipFilter == SourceChip::WeRead) {
    activityManager.goToWeRead();
    return;
  }
#ifdef BOARD_HAS_PSRAM
  if (chipFilter == SourceChip::Jjwxc) activityManager.goToJjwxc();
#endif
#endif
}

// Frame 15 is the *shelf* empty state: no books at all, or a source filter that
// matched none. A search with no hits answers the query, not "add a book", so
// it keeps the plain message.
bool LibraryListActivity::showEmptyPaths() const { return bookRowCount() == 0 && query.empty(); }

int LibraryListActivity::emptyPathRowCount() const {
  int rows = 1;  // transfer from a computer: always compiled in
#ifdef ENABLE_CHINESE_VERSION
  rows++;  // WeRead
#ifdef BOARD_HAS_PSRAM
  rows++;  // JJWXC needs both the Chinese build and the PSRAM heap
#endif
#endif
  return rows;
}

// The three concrete paths of frame 15, with their real destinations. Each
// row's description is the precondition/steps the firmware can actually keep:
// the app descriptions are the same strings the source cards use, and the
// JJWXC login hint is the real flow (account, password, code).
void LibraryListActivity::drawEmptyPaths(const Rect& band) {
  emptyPathCount_ = 0;
  struct PathRow {
    uint8_t kind;
    StrId title;
    StrId detail;
  };
  PathRow rows[kEmptyPathKinds];
  int n = 0;
  rows[n++] = {0, StrId::STR_FILE_TRANSFER, StrId::STR_FIND_LOCAL_DESC};
#ifdef ENABLE_CHINESE_VERSION
  rows[n++] = {1, StrId::STR_WEREAD_TITLE, StrId::STR_WEREAD_SCAN_LOGIN};
#ifdef BOARD_HAS_PSRAM
  rows[n++] = {2, StrId::STR_JJWXC_TITLE, StrId::STR_JJWXC_LOGIN_HINT};
#endif
#endif

  const int rowH = kCpyEmptyPathHeight;
  const int left = band.x + kGridSidePad;
  const int right = band.x + band.width - kGridSidePad;
  constexpr int iconSize = 32;
  constexpr int titleFont = UI_12_FONT_ID;
  constexpr int detailFont = UI_10_FONT_ID;
  const int titleLine = renderer.getLineHeight(titleFont);
  const int detailLine = renderer.getLineHeight(detailFont);

  int y = band.y + band.height - rowH * n;
  renderer.drawLine(left, y, right - 1, y, true);
  for (int i = 0; i < n; i++) {
    const int iconY = y + (rowH - iconSize) / 2;
    if (rows[i].kind == 0) {
      // The transfer glyph, the same mark the home dashboard uses for the web
      // upload page.
      InxAppIcons::drawModern(renderer, UIIcon::Transfer, left, iconY, iconSize, false);
    } else {
      // The app sources wear the seal the covers carry, so the row and the
      // books it will add read as one thing.
      const char* glyph = rows[i].kind == 1 ? tr(STR_SOURCE_BADGE_WEREAD) : tr(STR_SOURCE_BADGE_JJWXC);
      drawSourceSeal(renderer, left, iconY, iconSize, glyph);
    }

    const int chevX = right - 14;
    const int chevY = y + rowH / 2;
    renderer.drawLine(chevX, chevY - 6, chevX + 6, chevY, true);
    renderer.drawLine(chevX + 6, chevY, chevX, chevY + 6, true);

    const int textX = left + iconSize + 16;
    const int textRight = chevX - 16;
    const int textY = y + (rowH - titleLine - 4 - detailLine) / 2;
    renderer.drawText(titleFont, textX, textY, I18N.get(rows[i].title), true, EpdFontFamily::BOLD);
    const std::string detail =
        renderer.truncatedText(detailFont, I18N.get(rows[i].detail), std::max(1, textRight - textX));
    renderer.drawText(detailFont, textX, textY + titleLine + 4, detail.c_str());

    y += rowH;
    renderer.drawLine(left, y, right - 1, y, true);
    emptyPathRects_[i] = Rect{band.x, y - rowH, band.width, rowH};
    emptyPathKind_[i] = rows[i].kind;
    emptyPathCount_ = i + 1;
  }
}

void LibraryListActivity::openEmptyPath(const int index) {
  if (index < 0 || index >= emptyPathCount_) return;
  app.clearTapFlash();
  switch (emptyPathKind_[index]) {
    case 0:
      activityManager.goToFileTransfer();
      return;
#ifdef ENABLE_CHINESE_VERSION
    case 1:
      activityManager.goToWeRead();
      return;
#ifdef BOARD_HAS_PSRAM
    case 2:
      activityManager.goToJjwxc();
      return;
#endif
#endif
    default:
      return;
  }
}

void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  // Hit rects belong to one frame: a stale row must never take a tap after the
  // shelf or the empty state changed underneath it.
  emptyPathCount_ = 0;
  emptySourceRect_ = Rect{};
  UiTabListActivity::render(std::move(lock));
  // Grid covers prewarm one per frame, after the frame is already on screen.
  if (bookRowCount() > 0 && usesCoverGrid() && prepareNextGridCover()) return;
  // Empty state: the fui frame drew the chrome and left the content band
  // blank, so the backdrop + message can land on top and refresh once more.
  // The band matches the fui content margins (header above, readout + hints
  // below) so both themes put the message where the old component had it.
  if (bookRowCount() == 0 && !optionPopup.isActive()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    Rect band = pageContentRect();
    if (UITheme::getInstance().usesCpyLayout()) {
      // Same band the fui content margins carve out: below the headline and
      // the source tabs, above the position readout line over the tab bar.
      band.y += kCpyHeaderBandHeight;
      band.height = std::max(0, band.height - kCpyHeaderBandHeight -
                                    renderer.getLineHeight(SMALL_FONT_ID) - metrics.verticalSpacing);
    } else {
      band.y += metrics.topPadding + metrics.headerHeight;
      band.height = std::max(0, band.height - metrics.topPadding - metrics.headerHeight -
                                    metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID) -
                                    metrics.verticalSpacing);
    }
    // The invitation pill and the three path rows own the bottom of the band,
    // so the illustration + message are laid out in what is left instead of
    // being centred behind them.
    emptyPathCount_ = 0;
    const bool pill = emptySourceTitle() != nullptr;
    const int pillReserve = pill ? 88 : 0;
    const bool paths = showEmptyPaths();
    const int pathRows = paths ? emptyPathRowCount() * kCpyEmptyPathHeight : 0;
    const bool drawPaths = paths && band.height >= pathRows + 200;
    Rect messageBand = band;
    messageBand.height = std::max(0, band.height - (drawPaths ? pathRows : 0) - pillReserve);
    GUI.drawEmptyState(renderer, messageBand, emptyStateMessage());
    if (pill) {
      Rect pillBand = band;
      pillBand.y = band.y + messageBand.height;
      pillBand.height = pillReserve;
      drawEmptySourceAction(pillBand);
    }
    if (drawPaths) drawEmptyPaths(band);
    captureSnapshot();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void LibraryListActivity::drawFooter() {
  drawPositionReadout();
  // The empty shelf already owns the bottom band with its three import paths;
  // a "long-press to sort" hint there overlapped the third path's detail line.
  // The 3-column grid also has a partial trailing row, so the same hint ran
  // under the third cover; the CPY design has no such footer line.
  if (bookRowCount() > 0 && !UITheme::getInstance().usesCpyLayout()) drawHoldHelp();

  const bool backGoesHome = tabsFocused() && !groupsCollapsed && query.empty();
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  const bool canSearch = tabsFocused() && !degraded;
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tr(STR_TOGGLE) : confirmLabel,
                                            canSearch ? tr(STR_SEARCH) : tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
