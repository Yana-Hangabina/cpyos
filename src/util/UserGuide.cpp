#include "UserGuide.h"

#include <Arduino.h>
#include <Epub.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <LibraryBuilder.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "RecentBooksStore.h"
#include "UserGuide.generated.h"

namespace UserGuide {
namespace {
// CPYOS keeps its own marker: a device that already ran CrossMux still gets the
// CPYOS guide exactly once on its first CPYOS boot. Later firmware updates never
// write the book again, even if the bundled bytes change.
constexpr const char* CHECKED_PATH = "/.crosspoint/cpyos-user-guide.checked";
constexpr const char* CHECKED_TEMP_PATH = "/.crosspoint/cpyos-user-guide.checked.tmp";
constexpr const char* EPUB_TEMP_PATH = "/.crosspoint/user-guide.epub.tmp";
constexpr uint8_t CHECKED[] = {'1', '\n'};
constexpr size_t CHUNK_SIZE = 256;
enum class State { Skipped, Pending, Attempted };
State state = State::Skipped;

enum class FileMatch { Equal, Different, ReadError };

FileMatch compareFile(const char* path, const uint8_t* data, const size_t size) {
  HalFile file;
  if (!Storage.openFileForRead("GUIDE", path, file)) return FileMatch::ReadError;
  if (file.fileSize() != size) return FileMatch::Different;
  uint8_t buffer[CHUNK_SIZE];
  for (size_t offset = 0; offset < size;) {
    const size_t count = std::min(sizeof(buffer), size - offset);
    if (file.read(buffer, count) != static_cast<int>(count)) return FileMatch::ReadError;
    if (memcmp(buffer, data + offset, count) != 0) return FileMatch::Different;
    offset += count;
  }
  return FileMatch::Equal;
}

bool writeVerified(const char* path, const uint8_t* data, const size_t size) {
  if (!Storage.ensureDirectoryExists("/.crosspoint")) return false;
  if (Storage.exists(path) && !Storage.remove(path)) return false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("GUIDE", path, file)) return false;
    for (size_t offset = 0; offset < size;) {
      const size_t count = std::min(CHUNK_SIZE, size - offset);
      // Flash-backed bytes go straight to the HAL: no whole-book heap allocation.
      if (file.write(data + offset, count) != count) return false;
      offset += count;
    }
    file.flush();
  }
  return compareFile(path, data, size) == FileMatch::Equal;
}

bool markChecked() {
  if (!writeVerified(CHECKED_TEMP_PATH, CHECKED, sizeof(CHECKED)) || !Storage.rename(CHECKED_TEMP_PATH, CHECKED_PATH)) {
    LOG_ERR("GUIDE", "Failed to save first-use marker; retry on next boot");
    return false;
  }
  return true;
}

void install(const bool simplifiedChinese) {
  const auto& selected = simplifiedChinese ? bundled_user_guide::Chinese : bundled_user_guide::English;
  const auto& other = simplifiedChinese ? bundled_user_guide::English : bundled_user_guide::Chinese;
  const bundled_user_guide::Asset* asset = nullptr;
  if (Storage.exists(selected.path)) {
    switch (compareFile(selected.path, selected.data, selected.size)) {
      case FileMatch::ReadError:
        LOG_ERR("GUIDE", "Cannot read existing guide; retry on next boot");
        return;
      case FileMatch::Different:
        LOG_INF("GUIDE", "Preserving existing file: %s", selected.path);
        markChecked();
        return;
      case FileMatch::Equal:
        asset = &selected;  // Resume after a previous boot installed the file but not its recent entry.
        break;
    }
  } else {
    // An interrupted marker write can leave the other language's guide installed.
    // Reuse it instead of writing a second guide in the newly selected language.
    if (Storage.exists(other.path) && compareFile(other.path, other.data, other.size) == FileMatch::Equal) {
      asset = &other;
    } else {
      if (!writeVerified(EPUB_TEMP_PATH, selected.data, selected.size) ||
          !Storage.rename(EPUB_TEMP_PATH, selected.path)) {
        LOG_ERR("GUIDE", "Failed to install guide; retry on next boot");
        return;
      }
      asset = &selected;
    }
  }

  // Epub derives the standard cache path without parsing the archive. Fixed asset
  // paths/cache strings are each <64 bytes (including the 64-bit host hash). These
  // cold-path strings reuse the Epub/recents APIs; no metadata/ZIP buffers are allocated.
  // addBook() de-duplicates and moves an existing entry to the front, so the guide
  // becomes the first "Continue reading" card only on this first CPYOS boot.
  const Epub epub(asset->path, "/.crosspoint");
  if (!RECENT_BOOKS.addBook(asset->path, asset->title, asset->author, epub.getThumbBmpPath())) {
    LOG_ERR("GUIDE", "Failed to save guide in recents; retry on next boot");
    return;
  }
  // The card may already have books, so the library index may predate this file.
  // One rebuild makes the guide visible in the Library without touching recents again.
  if (!library::markLibraryIndexDirty()) {
    LOG_ERR("GUIDE", "Failed to mark the library index dirty");
  }
  markChecked();
}
}  // namespace

void prepare(const bool recentsLoaded) {
  state = State::Skipped;
  if (Storage.exists(CHECKED_PATH)) return;
  if (!recentsLoaded && Storage.exists(RecentBooksStore::getFilePath())) {
    LOG_ERR("GUIDE", "Unreadable recents; skipping first-use check");
    return;
  }
  state = State::Pending;
}

void installIfPending(const bool simplifiedChinese) {
  if (state != State::Pending) return;
  state = State::Attempted;
  const auto started = millis();
  const auto before = HalSystem::getHeapInfo();
  install(simplifiedChinese);
  const auto after = HalSystem::getHeapInfo();
  LOG_INF("GUIDE", "First-use attempt: %lu ms, heap %lu -> %lu, largest %lu -> %lu", millis() - started,
          static_cast<unsigned long>(before.freeBytes), static_cast<unsigned long>(after.freeBytes),
          static_cast<unsigned long>(before.largestFreeBlockBytes),
          static_cast<unsigned long>(after.largestFreeBlockBytes));
}
}  // namespace UserGuide
