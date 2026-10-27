#include "JjwxcUpdates.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WeReadStore.h>

#include <cstdio>
#include <cstring>

#include "JjwxcStore.h"

// The implementation is PSRAM-gated as a whole: the app itself only exists there
// (AppsMenuActivity), a 300-row snapshot is 9.6 KB of a budget the C3 does not
// have, and a C3 will never see a /JJWXC/ path anyway. The stubs keep the
// call sites free of #ifdef noise for a few dozen bytes of Flash.
#ifdef BOARD_HAS_PSRAM

namespace jjwxcUpdates {
namespace {

constexpr const char* kTag = "JXUPD";
constexpr const char* kSnapshotPath = "/.crosspoint/jjwxc/updates.bin";
constexpr uint16_t kRecordSize = sizeof(Entry);

// Serialised as uint32 little-endian at both ends; kept explicit so the file
// format does not depend on the compiler's packing of a struct.
constexpr size_t kHeaderSize = 12;

// Per-book gap answers, so buildScreen() can ask once per row per frame without
// touching the SD card again. 24 slots is far more than one screen of JJWXC
// books and costs 24 * 32 bytes of .bss.
constexpr int kGapCacheSlots = 24;

struct GapSlot {
  char novelId[16];
  Gap gap;
};
GapSlot gGap[kGapCacheSlots];
uint8_t gGapCursor = 0;
bool gGapFull = false;

memory::ByteBuffer gData;
uint32_t gCount = 0;
// 0 = not read yet, 1 = rows loaded, 2 = no usable snapshot (missing/damaged)
uint8_t gState = 0;

uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool ensureLoaded() {
  if (gState != 0) return gState == 1;

  HalFile file;
  if (!Storage.openFileForRead(kTag, kSnapshotPath, file)) {
    gState = 2;
    return false;
  }
  uint8_t header[kHeaderSize];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    LOG_ERR(kTag, "short snapshot header");
    gState = 2;
    return false;
  }
  if (readLe32(header) != kSnapshotMagic || readLe16(header + 4) != kVersion ||
      readLe16(header + 6) != kRecordSize) {
    LOG_ERR(kTag, "bad snapshot header (magic=%08lx version=%u size=%u)", (unsigned long)readLe32(header),
            (unsigned)readLe16(header + 4), (unsigned)readLe16(header + 6));
    gState = 2;
    return false;
  }
  const uint32_t count = readLe32(header + 8);
  if (count == 0 || count > kMaxEntries) {
    LOG_ERR(kTag, "snapshot row count %u out of range", (unsigned)count);
    gState = 2;
    return false;
  }
  const size_t bytes = static_cast<size_t>(count) * kRecordSize;
  auto buf = memory::makePsramByteBufferUninitializedNoThrow(bytes);
  if (!buf) {
    LOG_ERR(kTag, "no PSRAM for %u snapshot rows", (unsigned)count);
    // Not sticky: a later frame with more headroom may succeed.
    return false;
  }
  if (file.read(buf.get(), bytes) != static_cast<int>(bytes)) {
    LOG_ERR(kTag, "short snapshot body (%u rows)", (unsigned)count);
    gState = 2;
    return false;
  }
  gData = std::move(buf);
  gCount = count;
  gState = 1;
  LOG_INF(kTag, "snapshot: %u books", (unsigned)count);
  return true;
}

void clearGapCache() {
  memset(gGap, 0, sizeof(gGap));
  gGapCursor = 0;
  gGapFull = false;
}

const Gap* cachedGap(const char* novelId) {
  const int slots = gGapFull ? kGapCacheSlots : gGapCursor;
  for (int i = 0; i < slots; ++i) {
    if (strncmp(gGap[i].novelId, novelId, sizeof(gGap[i].novelId)) == 0) return &gGap[i].gap;
  }
  return nullptr;
}

void storeGap(const char* novelId, const Gap& gap) {
  if (cachedGap(novelId) != nullptr) return;
  GapSlot& slot = gGap[gGapCursor];
  snprintf(slot.novelId, sizeof(slot.novelId), "%s", novelId);
  slot.gap = gap;
  if (++gGapCursor >= kGapCacheSlots) {
    gGapCursor = 0;
    gGapFull = true;
  }
}

const Entry* find(const char* novelId) {
  if (novelId == nullptr || novelId[0] == '\0') return nullptr;
  if (!ensureLoaded()) return nullptr;
  const auto* rows = reinterpret_cast<const Entry*>(gData.get());
  for (uint32_t i = 0; i < gCount; ++i) {
    if (strncmp(rows[i].novelId, novelId, sizeof(rows[i].novelId)) == 0) return &rows[i];
  }
  return nullptr;
}

}  // namespace

bool saveSnapshot(const Entry* entries, size_t count) {
  if (count == 0) {
    // An account with an empty shelf must not leave yesterday's rows behind for
    // a book that was removed from it.
    const bool removed = !Storage.exists(kSnapshotPath) || Storage.remove(kSnapshotPath);
    invalidate();
    return removed;
  }
  if (entries == nullptr || count > kMaxEntries) return false;
  if (!JjwxcStore::ensureRoot()) return false;

  WeReadStore::IndexWriter writer;
  if (!writer.begin(kSnapshotPath, kSnapshotMagic, kRecordSize)) {
    LOG_ERR(kTag, "snapshot writer could not start");
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!writer.append(&entries[i])) {
      writer.abort();
      LOG_ERR(kTag, "snapshot append failed at row %u", (unsigned)i);
      return false;
    }
  }
  if (!writer.finish()) {
    writer.abort();
    LOG_ERR(kTag, "snapshot finish failed");
    return false;
  }
  invalidate();
  LOG_INF(kTag, "snapshot saved: %u books", (unsigned)count);
  return true;
}

void invalidate() {
  gData.reset();
  gCount = 0;
  gState = 0;
  clearGapCache();
}

bool sessionPresent() { return Storage.exists(JjwxcStore::kCookiesPath); }

bool novelIdFromPath(const char* path, char* out, size_t outCap) {
  constexpr const char* kPrefix = "/JJWXC/";
  constexpr size_t kPrefixLen = 7;
  constexpr const char* kSuffix = ".epub";
  constexpr size_t kSuffixLen = 5;
  if (out == nullptr || outCap == 0) return false;
  out[0] = '\0';
  if (path == nullptr) return false;
  const size_t len = strlen(path);
  if (len <= kPrefixLen + kSuffixLen) return false;
  if (strncmp(path, kPrefix, kPrefixLen) != 0) return false;
  if (strcmp(path + len - kSuffixLen, kSuffix) != 0) return false;
  const size_t nameLen = len - kPrefixLen - kSuffixLen;
  const char* name = path + kPrefixLen;
  size_t dash = nameLen;
  while (dash > 0) {
    --dash;
    if (name[dash] == '-') break;
  }
  if (dash == 0 || name[dash] != '-' || dash + 1 >= nameLen) return false;
  const size_t idLen = nameLen - dash - 1;
  if (idLen == 0 || idLen + 1 > outCap) return false;
  for (size_t i = 0; i < idLen; ++i) {
    const char c = name[dash + 1 + i];
    if (c < '0' || c > '9') return false;
  }
  memcpy(out, name + dash + 1, idLen);
  out[idLen] = '\0';
  return true;
}

Gap gapForNovel(const char* novelId) {
  Gap gap;
  if (novelId == nullptr || novelId[0] == '\0') return gap;
  if (gState == 0 && !ensureLoaded()) return gap;
  if (const Gap* hit = cachedGap(novelId)) return *hit;

  const Entry* remote = find(novelId);
  if (remote == nullptr) {
    // No snapshot row: unknown, not "up to date". Cached so the SD card is not
    // asked again for the same book this session.
    storeGap(novelId, gap);
    return gap;
  }
  JjwxcStore::TocReader toc;
  if (!toc.open(novelId) || toc.count() == 0) {
    storeGap(novelId, gap);
    return gap;
  }
  JjwxcStore::TocRecord last;
  const uint32_t localCount = toc.count();
  const bool haveLast = toc.read(localCount - 1, last);
  const uint32_t localLastId = haveLast ? last.chapterId : 0;
  toc.close();

  gap.known = true;
  gap.cachedChapters = localCount;
  gap.remoteChapters = remote->chapterCount;
  gap.fetchedAt = remote->fetchedAt;
  if (remote->chapterCount > localCount) {
    gap.newChapters = remote->chapterCount - localCount;
  } else if (remote->lastChapterId > localLastId && localLastId != 0) {
    // The catalogue was revised without growing (a chapter split or a
    // re-upload). One unknown chapter is the honest answer; the count is not
    // trustworthy here.
    gap.newChapters = 1;
  }
  if (gap.newChapters == 0 && remote->renewStatus != 0 && !haveLast) gap.newChapters = 1;
  storeGap(novelId, gap);
  return gap;
}

Gap gapForPath(const char* path) {
  char novelId[16];
  if (!novelIdFromPath(path, novelId, sizeof(novelId))) return Gap{};
  return gapForNovel(novelId);
}

}  // namespace jjwxcUpdates

#else  // !BOARD_HAS_PSRAM

namespace jjwxcUpdates {
bool saveSnapshot(const Entry*, size_t) { return false; }
void invalidate() {}
bool sessionPresent() { return false; }
bool novelIdFromPath(const char*, char* out, size_t outCap) {
  if (out != nullptr && outCap > 0) out[0] = '\0';
  return false;
}
Gap gapForNovel(const char*) { return Gap{}; }
Gap gapForPath(const char*) { return Gap{}; }
}  // namespace jjwxcUpdates

#endif
