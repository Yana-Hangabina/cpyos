#pragma once

#include <cstddef>
#include <cstdint>

// Cross-activity view of "how far behind the card is" for a JJWXC book.
//
// The app is the only writer: after a successful, unfiltered walk of the online
// shelf it drops one snapshot of the remote catalogue facts
// (`/.crosspoint/jjwxc/updates.bin`). The shelf, the home continue card and the
// reader only ever READ that file, which is what lets them answer "is there a
// new chapter?" with the radio off -- the project's rule is that neither the
// shelf nor the reader brings up Wi-Fi on its own.
//
// Everything here is best-effort. A missing snapshot, a card that never synced,
// or a book that was renamed on the server all degrade to `known == false`, and
// callers must render "unknown" rather than "no updates": those are different
// statements and conflating them is how a stale badge turns into a lie.
namespace jjwxcUpdates {

constexpr uint32_t kMaxEntries = 300;
constexpr uint16_t kVersion = 1;
constexpr uint32_t kSnapshotMagic = 0x3155584A;  // "JXU1"

// One book, 32 bytes. `lastChapterId` and `chapterCount` are both kept because
// they answer different questions: the count drives "N new chapters", the id
// catches the case where the remote catalogue was revised without growing.
struct Entry {
  char novelId[16];
  uint32_t lastChapterId;
  uint32_t chapterCount;
  uint32_t fetchedAt;  // unix seconds of the shelf walk that produced this row
  uint8_t renewStatus;
  uint8_t reserved[3];
};
static_assert(sizeof(Entry) == 32, "JXU1 record layout must stay at 32 bytes");

struct Gap {
  bool known = false;        // a snapshot row and a readable local toc both exist
  uint32_t newChapters = 0;  // remote count - local count, clamped at 0
  uint32_t cachedChapters = 0;
  uint32_t remoteChapters = 0;
  uint32_t fetchedAt = 0;
};

// Replaces the snapshot atomically (`.part` + rename through WeRead's
// IndexWriter). count == 0 removes it instead of writing an empty index.
bool saveSnapshot(const Entry* entries, size_t count);

// Drops the in-memory copy so the next query re-reads the card. Call after the
// snapshot file changed underneath us (i.e. when the app returns).
void invalidate();

// True when a WAP session file is on the card. The Library uses this to decide
// whether a sync may cost a round trip at all; it deliberately does NOT decrypt
// the jar -- whether the sid is still alive is the app's answer to give.
bool sessionPresent();

// "/JJWXC/<title>-<novelId>.epub" -> novelId. The id is taken after the LAST
// '-' (a title may contain dashes) and must be all digits: anything else in that
// folder was not written by this app.
bool novelIdFromPath(const char* path, char* out, size_t outCap);

// Local-vs-remote gap for a card path. Cheap after the first call per book: the
// toc is read once and the answer is cached, so a rendering pass can ask per row.
Gap gapForPath(const char* path);
Gap gapForNovel(const char* novelId);

}  // namespace jjwxcUpdates
