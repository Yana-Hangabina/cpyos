#pragma once

#include <cstddef>
#include <cstdint>

#include "JjwxcCacheQueue.h"

// Application-level background driver for the persistent paragraph-comment
// queue. It owns the queue file and the only worker that writes it.
//
// Trigger points:
//   * enqueue from the app (start immediately when the link/saved Wi-Fi allows),
//   * entering standby from any non-reader screen,
//   * Wi-Fi coming back (poll() sees the transition).
// It is suspended while a reader is in the foreground and while a foreground
// JJWXC job (chapter cache etc.) owns the network, so the reader's own comment
// panel prefetch is never competing with it.
namespace jjwxc_cache_service {

enum class Badge : uint8_t { None, Running, Paused, Done };
enum class EnqueueResult : uint8_t { Ok, Invalid, NoCatalogue, Full };

struct Status {
  Badge badge = Badge::None;
  bool workerActive = false;
  bool userPaused = false;
  uint32_t itemIndex = 0;
  uint32_t curOrdinal = 0;
  uint32_t doneChapters = 0;
  uint32_t totalChapters = 0;
  uint32_t doneParagraphs = 0;
  uint32_t totalParagraphs = 0;
  uint32_t requests = 0;
  uint32_t revision = 0;
};

#ifdef BOARD_HAS_PSRAM
void begin();
void poll();
EnqueueResult enqueue(const char* novelId, const char* title, uint32_t fromOrdinal, uint32_t toOrdinal);
void onStandbyEntered();
void setReaderForeground(bool reader);
void setForegroundJob(bool busy);
void acknowledgeBook(const char* novelId);
bool pauseByUser();
bool resumeByUser();
bool clearDone();
bool clearAll();
bool snapshot(JjwxcCacheQueue::Item* out, size_t maxOut, size_t& count, Status& status);
Badge badge();
uint32_t badgeVersion();
#else
// C3 builds have no PSRAM for the scratch/session and the app is compiled out,
// so the queue is inert. Keeping the stubs here means callers do not need the
// PSRAM guard at every hook point.
inline void begin() {}
inline void poll() {}
inline EnqueueResult enqueue(const char*, const char*, uint32_t, uint32_t) { return EnqueueResult::Invalid; }
inline void onStandbyEntered() {}
inline void setReaderForeground(bool) {}
inline void setForegroundJob(bool) {}
inline void acknowledgeBook(const char*) {}
inline bool pauseByUser() { return false; }
inline bool resumeByUser() { return false; }
inline bool clearDone() { return false; }
inline bool clearAll() { return false; }
inline bool snapshot(JjwxcCacheQueue::Item*, size_t, size_t& count, Status& status) {
  count = 0;
  status = Status{};
  return false;
}
inline Badge badge() { return Badge::None; }
inline uint32_t badgeVersion() { return 0; }
#endif

}  // namespace jjwxc_cache_service
