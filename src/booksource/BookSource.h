#pragma once

// Public boundary for optional, separately maintained book sources.
//
// The core firmware never contains a source's protocol, storage or UI
// implementation: it only knows this interface, the generic value types below,
// and the registration point in BookSource.cpp. A source is linked in as its
// own component and registers itself there; without it every entry point
// degrades to the "no source" behaviour (the feature simply does not appear).
//
// Everything in this header is deliberately source-agnostic: no host names, no
// credentials, no endpoint paths, no wire formats.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Activity;
class ActivityResult;
class Epub;
class GfxRenderer;
class MappedInputManager;
class Page;

namespace booksource {

enum class Id : uint8_t { None = 0, Jjwxc = 1 };

// Chapter availability as stored by a source. Names are generic on purpose:
// "unpurchased" applies to any paid-content source, "app only" to any chapter
// the source refuses to serve outside its own client.
enum class TocState : uint8_t {
  Cached = 0,
  Missing = 1,
  Unpurchased = 2,
  Locked = 3,
  AppOnly = 4,
};

struct TocMeta {
  uint32_t chapterId = 0;
  uint8_t state = 0;
  uint8_t isVip = 0;
};

// How many chapters the source has that the packaged book does not.
struct UpdateGap {
  bool known = false;
  uint32_t newChapters = 0;
};

// One paragraph's comment count, in the same shape the reader's page cache
// serialises. Kept here so the reader can size its PSRAM table without knowing
// which source produced it.
struct CommentCount {
  uint32_t paragraphId = 0;
  uint16_t total = 0;
};

// Explicit values: the reader indexes its theme's icon enums with these, so a
// reorder here would silently change every Contents row.
enum class RowBadge : uint8_t { None = 0, Free = 1, Vip = 2, VipYen = 3, Lock = 4 };
enum class RowAction : uint8_t { None = 0, Download = 1, Refresh = 2, Delete = 3 };

struct RowTail {
  RowBadge badge = RowBadge::None;
  RowAction action = RowAction::None;
};

// Extra entries a source contributes to the reader menu.
enum class MenuAction : uint8_t {
  UpdateCurrentChapter,
  UpdateChapterList,
  BookDetail,
  CloudProgress,
};

// What the source's own app should do when opened as a child of the reader or
// of another core screen.
enum class AppAction : uint8_t {
  None = 0,
  Open,
  UpdateCurrentChapter,
  UpdateChapterList,
  OpenBookDetail,
  CacheChapter,
  PurchaseChapter,
  RefreshUpdates,
};

struct AppLink {
  AppAction action = AppAction::None;
  std::string bookId;
  uint32_t chapterId = 0;
  bool openChapter = false;
};

// Returned by the source app when it closes itself.
struct AppResult {
  AppAction action = AppAction::None;
  bool completed = false;
  uint32_t chapterId = 0;
  bool readable = false;
  bool openChapter = false;
};

enum class JobBadge : uint8_t { None = 0, Working, Paused, Done };

// Services the reader offers to a source's per-book support object. Everything
// here is generic reader behaviour; the source's support object decides when to
// call it.
class ReaderHost {
 public:
  virtual ~ReaderHost() = default;
  virtual GfxRenderer& renderer() = 0;
  virtual Epub* epub() = 0;
  virtual int currentSpine() const = 0;
  virtual int currentTocIndex() const = 0;
  virtual bool atLastPageOfBook() const = 0;
  virtual bool usesToolbarMenu() const = 0;
  // Free the section's internal-RAM blocks, drop the page cache and close any
  // open sheet before a network-bound child activity starts.
  virtual void prepareForChild() = 0;
  // Restore the user's orientation and reload an SD font the child unloaded.
  virtual void restoreAfterChild() = 0;
  // Rebuild the Epub after the source repackaged the file (title changes move
  // the artefact). Returns false when the book cannot be reopened.
  virtual bool reopenAfterExternalChange() = 0;
  virtual void openContentsPanel(int focusTocIndex) = 0;
  virtual void selectChapterFromMenu() = 0;
  virtual void failAndFinish() = 0;
  virtual void requestUpdate() = 0;
  virtual bool startChild(std::unique_ptr<Activity> child, std::function<void(const ActivityResult&)> handler) = 0;
  virtual void removeDerivedCaches() = 0;
  virtual void invalidateChapterState() = 0;
};

// Per-opened-book support object. Created by ISource::newReaderSupport and owned
// by the reader; attach() decides whether this book belongs to the source.
class ReaderSupport {
 public:
  virtual ~ReaderSupport() = default;
  virtual bool attach(ReaderHost& host, const std::string& epubPath) = 0;
  virtual void detach() = 0;
  // The reader finished loading its TOC; the source may now map its own
  // catalogue onto it (and must disable per-chapter chrome on a mismatch).
  virtual void onTocLoaded(int tocCount) = 0;
  virtual void onSpineChanged(int spineIndex) = 0;
  virtual void onBookClosed() = 0;

  // Rendering. Bubbles are laid out once per rendered page and painted in every
  // pass, exactly like the footnote card.
  virtual void layoutBubbles(const Page& page, int fontId, int top, int right, int bottom, int left) = 0;
  virtual void drawBubbles() const = 0;
  // Page chrome drawn after the body: the placeholder action button and the
  // "more chapters" pill. Both record their hit rectangles internally.
  virtual void drawChrome(int fontId, int bottomMargin) = 0;
  // Returns true when the tap hit source chrome and must not page-turn.
  virtual bool handleTap(int x, int y) = 0;

  // Contents panel.
  virtual bool filterable() const = 0;
  virtual bool rowVisible(int tocIndex) const = 0;
  virtual bool numberedTitles() const = 0;
  virtual RowTail tailFor(int tocIndex) const = 0;
  virtual void activateRow(int tocIndex) = 0;

  // Menu.
  virtual void appendMenuActions(std::vector<MenuAction>& out) const = 0;
  virtual void runMenuAction(MenuAction action) = 0;
  virtual void openUpdateList(bool returnToContents) = 0;

  // Debug hooks used by the serial console.
  virtual void debugOpenComment(bool fake) = 0;
};

// The registered source (nullptr when no private component is compiled in).
class ISource {
 public:
  virtual ~ISource() = default;
  virtual Id id() const = 0;

  virtual std::unique_ptr<ReaderSupport> newReaderSupport() = 0;
  // The pending AppLink is consumed by this call.
  virtual std::unique_ptr<Activity> newAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) = 0;
  virtual void requestAppLink(const AppLink& link) = 0;

  // Library / home / recent badges and the "N new chapters" suffix.
  virtual const char* badgeForPath(const char* path) const = 0;
  virtual UpdateGap updateGapForPath(const char* path) const = 0;
  // True when the user has a usable session, i.e. the entry points should say
  // "account" rather than "log in" and an update check can run.
  virtual bool sessionPresent() const = 0;
  // Chapter totals for one packaged book (home dashboard). `readable` counts
  // chapters that hold real text, `total` the catalogue size.
  virtual bool cachedChapterSummary(const char* path, uint32_t& total, bool& readable) const = 0;
  // The cached update snapshot no longer describes the source's catalogue.
  virtual void invalidateUpdates() = 0;

  // Background work that must keep running while other screens are open.
  // start() loads persisted state and never opens the radio.
  virtual void start() = 0;
  virtual void poll() = 0;
  virtual JobBadge jobBadge() const = 0;
  virtual uint32_t jobRevision() const = 0;
  virtual void acknowledgeBook(const char* bookId) = 0;
  virtual void onStandby() = 0;
  virtual void setReaderForeground(bool foreground) = 0;

  // Entry points from core UI that do not go through the reader.
  virtual void requestSearch(const char* query) = 0;
  // 0 shelf, 1 discover, 2 search, 3 account.
  virtual void requestShortcut(uint8_t shortcut) = 0;

  // Returns true when the line was a source-specific debug command.
  virtual bool debugCommand(const char* line) = 0;
};

// Accessors. active() is nullptr when the private component is not linked.
ISource* active();
inline bool present() { return active() != nullptr; }

// Convenience that tolerates a missing source.
const char* badgeForPath(const char* path);
UpdateGap updateGapForPath(const char* path);
bool cachedChapterSummary(const char* path, uint32_t& total, bool& readable);
bool sessionPresent();
void invalidateUpdates();
void start();
void poll();
JobBadge jobBadge();
uint32_t jobRevision();
void onStandby();
void setReaderForeground(bool foreground);

}  // namespace booksource
