#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "InxRecentLayout.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
#include <cstddef>

#include "Memory.h"
#endif

struct ReadingBookStats;
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
class HalFile;
#endif

class InxRecentActivity final : public Activity {
  enum class CoverCacheState : uint8_t { Unchecked, Ready, Missing, Unavailable };
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  enum class CoverCacheLoadResult : uint8_t { Loaded, Stream, Invalid };

  struct CoverRamCache {
    memory::ByteBuffer bytes;
    size_t size = 0;
    bool attempted = false;
  };

  static constexpr size_t MAX_CACHED_COVER_FILE_BYTES = 64 * 1024;
  static constexpr size_t MAX_COVER_CACHE_BYTES = 512 * 1024;
  static constexpr size_t COVER_CACHE_MAX_ALLOC_RESERVE = 8 * 1024;
#endif

  const std::vector<RecentBook>* books = nullptr;
  std::array<const ReadingBookStats*, RecentBooksStore::MAX_RECENT_BOOKS> bookStats{};
  std::array<CoverCacheState, RecentBooksStore::MAX_RECENT_BOOKS> targetCoverStates{};
  std::array<CoverCacheState, RecentBooksStore::MAX_RECENT_BOOKS> fallbackCoverStates{};
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  std::array<CoverRamCache, RecentBooksStore::MAX_RECENT_BOOKS> targetCoverCaches{};
  std::array<CoverRamCache, RecentBooksStore::MAX_RECENT_BOOKS> fallbackCoverCaches{};
  size_t cachedCoverBytes = 0;
#endif
  int selected = 0;
  int thumbnailHeight = 0;

  Rect contentRect() const;
  const ReadingBookStats* statsAt(int index) const;
  void openSelected();
  void setThumbnailHeight(int displayHeight);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  void clearCoverCaches();
  CoverCacheLoadResult tryLoadCoverCache(HalFile& file, CoverRamCache& cache);
  bool tryDrawBookCover(const std::string& path, const Rect& bounds, CoverCacheState& state, CoverRamCache& cache);
#else
  bool tryDrawBookCover(const std::string& path, const Rect& bounds, CoverCacheState& state);
#endif
  bool drawBookCover(int bookIndex, const Rect& bounds);
  bool prepareNextMissingCover();
  // One progress push while covers are generated, then a single final push:
  // the old code pushed a full refresh per missing cover (N covers = N scans).
  bool coverProgressPushed_ = false;
  // When the current cover-loading pass started (0 = not started). Bounds how long
  // the previous screen may stay up before one progress push is allowed, so a slow
  // generator cannot leave a stale page on the panel indefinitely.
  uint32_t coverLoadingSinceMs_ = 0;


  // refreshed UI home (design frame 01): one large continue card, a 140 px reading
  // strip with the week bars, the three most recent covers, and the update row.
  // Ring slots: 0 continue, 1 today, 2..4 recent books, 5 sync.
  struct DashLayout {
    Rect firstCard;
    Rect todayCard;
    Rect recentCard;
    Rect recentBooks[3];
    Rect recentAll;
    Rect syncRow;
  };
  static constexpr int kDashSlots = 7;

  // Chapter counts read once on entry for the continue card and the three
  // recent covers. Local EPUBs keep total == 0 (unknown) so the UI omits the
  // count instead of inventing one.
  struct CachedChapterInfo {
    uint32_t cached = 0;
    uint32_t total = 0;
  };
  std::array<CachedChapterInfo, 4> chapterInfo_{};
  CachedChapterInfo readChapterInfo(const RecentBook& book) const;
  bool syncRowVisible() const;
  DashLayout dashboardLayout(const Rect& content) const;
  int dashboardIndexFromPoint(int x, int y) const;
  void loopDashboard();
  void dashboardActivate(int index);
  void drawDashboard(const Rect& content, bool drawCoverArt);
  Rect dashboardCoverRect(const Rect& content) const;
  void checkJjwxcUpdates();
  bool syncInFlight_ = false;

 public:
  explicit InxRecentActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("InxRecent", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  MainTab mainTab() const override { return MainTab::Recent; }
  void selectMainTabContentEdge(MainTabContentEdge edge) override;
};
