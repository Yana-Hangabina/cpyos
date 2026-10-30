#include "BookCoverLoader.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Cbz.h>
#include <Epub.h>
#include <Fb2.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

namespace BookCoverLoader {
namespace {

enum class CachedCoverState { Missing, Ready, Terminal };

CachedCoverState inspectCachedCover(const std::string& path, const bool emptyIsTerminal) {
  if (!Storage.exists(path.c_str())) return CachedCoverState::Missing;

  {
    HalFile file;
    if (!Storage.openFileForRead("COVER", path, file)) return CachedCoverState::Terminal;
    if (file.fileSize() == 0 && emptyIsTerminal) return CachedCoverState::Terminal;

    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
      return CachedCoverState::Ready;
    }
  }

  if (!Storage.remove(path.c_str())) {
    LOG_ERR("COVER", "Failed to remove invalid cover: %s", path.c_str());
    return CachedCoverState::Terminal;
  }
  return CachedCoverState::Missing;
}

template <typename Generate>
std::string ensureCachedCover(const std::string& path, const bool emptyIsTerminal, bool* generated,
                              Generate&& generate) {
  if (generated) *generated = false;
  const CachedCoverState state = inspectCachedCover(path, emptyIsTerminal);
  if (state == CachedCoverState::Ready) return path;
  if (state == CachedCoverState::Terminal) return "";
  // Generation parses the container's cover and writes a thumbnail; on a slow
  // card that is 0.3-3 s of blocked UI. Log it so a "the screen hung" report can
  // be tied to a specific book instead of guesswork.
  const uint32_t started = millis();
  const bool ok = generate();
  const bool ready = ok && inspectCachedCover(path, emptyIsTerminal) == CachedCoverState::Ready;
  LOG_INF("COVER", "thumbnail %s in %ums: %s", ready ? "generated" : "FAILED",
          static_cast<unsigned>(millis() - started), path.c_str());
  if (!ready) return "";
  if (generated) *generated = true;
  return path;
}

// Home and the library grid are both live in a Home <-> Library switch: Home
// needs 4 files, one grid row needs 5. Four slots made every switch evict the
// other screen's files, so both re-read ~500 KB from the card each time.
constexpr int kThumbCacheSlots = 10;
constexpr size_t kThumbCacheMaxBytes = 160 * 1024;
struct ThumbSlot {
  std::string path;
  CachedThumbFile file;
  uint32_t stamp = 0;
};
ThumbSlot g_thumbSlots[kThumbCacheSlots];
uint32_t g_thumbStamp = 0;
size_t g_thumbTotal = 0;

ThumbSlot* findSlot(const std::string& path) {
  for (auto& slot : g_thumbSlots) {
    if (!slot.path.empty() && slot.path == path) return &slot;
  }
  return nullptr;
}

// Resolving a thumbnail path constructs the container object (Epub/Xtc/Cbz),
// which loads its metadata cache from the card. The library grid asks for every
// visible book on every entry, so each entry re-opened five books just to learn
// a file name. Memoise (book path, height) -> thumb path for the session.
struct ThumbPathSlot {
  std::string key;
  std::string path;
  uint32_t stamp = 0;
};
constexpr int kThumbPathSlots = 16;
ThumbPathSlot g_thumbPathSlots[kThumbPathSlots];
uint32_t g_thumbPathStamp = 0;

std::string thumbPathUncached(const std::string& bookPath, int height) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    return epub.getThumbBmpPath(height);
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    return Xtc(bookPath, "/.crosspoint").getThumbBmpPath(height);
  }
  if (FsHelpers::hasCbzExtension(bookPath)) {
    return Cbz(bookPath, "/.crosspoint").getThumbBmpPath(height);
  }
  if (FsHelpers::hasFb2Extension(bookPath)) {
    const Fb2 fb2(bookPath, "/.crosspoint");
    if (!Storage.exists(fb2.getEpubPath().c_str())) return {};
    Epub epub(fb2.getEpubPath(), "/.crosspoint");
    return epub.getThumbBmpPath(height);
  }
  return {};
}

std::string thumbPathFor(const std::string& bookPath, int height) {
  if (height <= 0) return {};
  std::string key = bookPath;
  key += '#';
  key += std::to_string(height);
  ThumbPathSlot* victim = &g_thumbPathSlots[0];
  for (auto& slot : g_thumbPathSlots) {
    if (slot.key == key) {
      slot.stamp = ++g_thumbPathStamp;
      return slot.path;
    }
    if (slot.key.empty()) {
      victim = &slot;
      break;
    }
    if (slot.stamp < victim->stamp) victim = &slot;
  }
  victim->key = key;
  victim->path = thumbPathUncached(bookPath, height);
  victim->stamp = ++g_thumbPathStamp;
  return victim->path;
}

}  // namespace

const CachedThumbFile* thumbFile(const std::string& path) {
  ThumbSlot* slot = findSlot(path);
  if (slot == nullptr) return nullptr;
  slot->stamp = ++g_thumbStamp;
  return &slot->file;
}

const CachedThumbFile* cacheThumbFile(const std::string& path) {
  if (ThumbSlot* cached = findSlot(path)) return &cached->file;
  HalFile file;
  if (!Storage.openFileForRead("COVER", path, file)) return nullptr;
  const size_t size = file.fileSize();
  if (size == 0 || size > kThumbCacheMaxBytes) return nullptr;
  if (!memory::psramHasHeadroom(size + 64 * 1024, size, 64 * 1024)) return nullptr;
  auto bytes = memory::makePsramByteBufferNoThrow(size);
  if (!bytes) return nullptr;
  if (!file.seek(0) || file.read(bytes.get(), size) != static_cast<int>(size)) return nullptr;
  // Evict the least recently used slot, then store.
  ThumbSlot* victim = &g_thumbSlots[0];
  for (auto& slot : g_thumbSlots) {
    if (slot.path.empty()) {
      victim = &slot;
      break;
    }
    if (slot.stamp < victim->stamp) victim = &slot;
  }
  g_thumbTotal -= victim->file.size;
  victim->path = path;
  victim->file.bytes = std::move(bytes);
  victim->file.size = size;
  victim->stamp = ++g_thumbStamp;
  g_thumbTotal += size;
  return &victim->file;
}

void clearThumbFileCache() {
  for (auto& slot : g_thumbSlots) {
    slot.path.clear();
    slot.file = {};
  }
  g_thumbTotal = 0;
}

std::string ensureThumbnail(const std::string& bookPath, const int height, bool* generated) {
  if (generated) *generated = false;
  if (height <= 0 || !Storage.exists(bookPath.c_str())) return "";

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    const std::string path = epub.getThumbBmpPath(height);
    return ensureCachedCover(path, !epub.hasCoverOverride(), generated, [&]() {
      if (!epub.load(true, true)) return false;
      epub.setupCacheDir();
      return epub.generateThumbBmp(height);
    });
  }

  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    const std::string path = xtc.getThumbBmpPath(height);
    return ensureCachedCover(path, false, generated, [&]() {
      if (!xtc.load()) return false;
      xtc.setupCacheDir();
      return xtc.generateThumbBmp(height);
    });
  }

  if (FsHelpers::hasCbzExtension(bookPath)) {
    Cbz cbz(bookPath, "/.crosspoint");
    const std::string path = cbz.getThumbBmpPath(height);
    return ensureCachedCover(path, false, generated, [&]() {
      if (!cbz.load()) return false;
      cbz.setupCacheDir();
      return cbz.generateThumbBmp(height);
    });
  }

  if (FsHelpers::hasFb2Extension(bookPath)) {
    const Fb2 fb2(bookPath, "/.crosspoint");
    if (!Storage.exists(fb2.getEpubPath().c_str())) return "";
    Epub epub(fb2.getEpubPath(), "/.crosspoint");
    const std::string path = epub.getThumbBmpPath(height);
    return ensureCachedCover(path, false, generated, [&]() {
      if (!epub.load(true, true)) return false;
      epub.setupCacheDir();
      return epub.generateThumbBmp(height);
    });
  }

  return "";
}

std::string cachedThumbnail(const std::string& bookPath, const int height) {
  if (height <= 0 || !Storage.exists(bookPath.c_str())) return "";
  const std::string path = thumbPathFor(bookPath, height);
  if (path.empty()) return "";
  return inspectCachedCover(path, false) == CachedCoverState::Ready ? path : "";
}

std::string ensureFullCover(const std::string& bookPath, std::string* title, std::string* author, bool* generated) {
  if (title) title->clear();
  if (author) author->clear();
  if (generated) *generated = false;
  if (!Storage.exists(bookPath.c_str())) return "";

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    const std::string path = epub.getCoverBmpPath();
    return ensureCachedCover(path, false, generated, [&]() {
      if (!epub.load(true, true)) return false;
      epub.setupCacheDir();
      if (title) *title = epub.getTitle();
      if (author) *author = epub.getAuthor();
      return epub.generateCoverBmp();
    });
  }

  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    const std::string path = xtc.getCoverBmpPath();
    return ensureCachedCover(path, false, generated, [&]() {
      if (!xtc.load()) return false;
      xtc.setupCacheDir();
      if (title) *title = xtc.getTitle();
      if (author) *author = xtc.getAuthor();
      return xtc.generateCoverBmp();
    });
  }

  if (FsHelpers::hasCbzExtension(bookPath)) {
    Cbz cbz(bookPath, "/.crosspoint");
    const std::string path = cbz.getCoverBmpPath();
    return ensureCachedCover(path, false, generated, [&]() {
      if (!cbz.load()) return false;
      cbz.setupCacheDir();
      if (title) *title = cbz.getTitle();
      return cbz.generateCoverBmp();
    });
  }

  if (FsHelpers::hasFb2Extension(bookPath)) {
    const Fb2 fb2(bookPath, "/.crosspoint");
    if (!Storage.exists(fb2.getEpubPath().c_str())) return "";
    const Epub cached(fb2.getEpubPath(), "/.crosspoint");
    const std::string path = cached.getCoverBmpPath();
    return ensureCachedCover(path, false, generated, [&]() {
      Epub epub(fb2.getEpubPath(), "/.crosspoint");
      if (!epub.load(true, true)) return false;
      epub.setupCacheDir();
      if (title) *title = epub.getTitle();
      if (author) *author = epub.getAuthor();
      return epub.generateCoverBmp();
    });
  }

  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    const std::string path = txt.getCoverBmpPath();
    return ensureCachedCover(path, false, generated, [&]() {
      if (!txt.load()) return false;
      txt.setupCacheDir();
      if (title) *title = txt.getTitle();
      return txt.generateCoverBmp();
    });
  }

  return "";
}

}  // namespace BookCoverLoader
