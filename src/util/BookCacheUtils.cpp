#include "BookCacheUtils.h"

#include <cstdint>

#include <Cbz.h>
#include <Epub.h>
#include <Fb2.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointState.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "util/BookmarkUtil.h"

namespace {
bool renameIfTargetMissing(const std::string& oldPath, const std::string& newPath) {
  if (oldPath == newPath || !Storage.exists(oldPath.c_str())) return true;
  if (Storage.exists(newPath.c_str())) return false;
  return Storage.rename(oldPath.c_str(), newPath.c_str());
}

std::string remapCacheAsset(const std::string& assetPath, const std::string& oldCachePath,
                            const std::string& newCachePath) {
  if (oldCachePath.empty() || assetPath.rfind(oldCachePath, 0) != 0) return assetPath;
  return newCachePath + assetPath.substr(oldCachePath.size());
}
}  // namespace

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";
  constexpr char CBZ_PREFIX[] = "cbz_";
  constexpr char FB2_PREFIX[] = "fb2_";
  constexpr char PDF_PREFIX[] = "pdf_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0 ||
         strncmp(name, CBZ_PREFIX, std::size(CBZ_PREFIX) - 1) == 0 ||
         strncmp(name, FB2_PREFIX, std::size(FB2_PREFIX) - 1) == 0 ||
         strncmp(name, PDF_PREFIX, std::size(PDF_PREFIX) - 1) == 0;
}

bool clearBookCache(const std::string& path) {
  bool cleared = true;
  if (FsHelpers::hasReflowableBookExtension(path)) {
    cleared = Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    cleared = Xtc(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasCbzExtension(path)) {
    cleared = Cbz(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasFb2Extension(path)) {
    const Fb2 fb2(path, "/.crosspoint");
    // The EPUB cache key includes the generated file's size/mtime, so resolve
    // it before removing the FB2 cache directory.
    const bool hasEpub = Storage.exists(fb2.getEpubPath().c_str());
    const std::string epubCache =
        hasEpub ? Epub(fb2.getEpubPath(), "/.crosspoint").getCachePath() : std::string();
    cleared = fb2.clearCache();
    if (!epubCache.empty() && Storage.exists(epubCache.c_str())) {
      cleared = Storage.removeDir(epubCache.c_str()) && cleared;
    }
  } else if (FsHelpers::hasPdfExtension(path)) {
    const std::string cache = pdfCachePath(path);
    if (Storage.exists(cache.c_str())) cleared = Storage.removeDir(cache.c_str());
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    cleared = Epub(path, "/.crosspoint").clearCache();
  } else {
    return true;
  }
  if (!cleared) return false;
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
  return true;
}

std::string pdfCachePath(const std::string& path) {
  if (!FsHelpers::hasPdfExtension(path)) return {};
  uint32_t hash = 2166136261u;
  for (const unsigned char c : path) {
    hash ^= c;
    hash *= 16777619u;
  }
  return "/.crosspoint/pdf_" + std::to_string(hash);
}

std::string bookCachePath(const std::string& path) {
  if (FsHelpers::hasPdfExtension(path)) return pdfCachePath(path);
  if (FsHelpers::hasReflowableBookExtension(path)) return Epub(path, "/.crosspoint").getCachePath();
  if (FsHelpers::hasXtcExtension(path)) return Xtc(path, "/.crosspoint").getCachePath();
  if (FsHelpers::hasCbzExtension(path)) return Cbz(path, "/.crosspoint").getCachePath();
  if (FsHelpers::hasFb2Extension(path)) return Fb2(path, "/.crosspoint").getCachePath();
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  return {};
}

bool relocateBookArtifacts(const std::string& oldPath, const std::string& newPath) {
  bool ok = true;
  const std::string oldCachePath = bookCachePath(oldPath);
  const std::string newCachePath = bookCachePath(newPath);
  if (!oldCachePath.empty() && oldCachePath != newCachePath && Storage.exists(oldCachePath.c_str())) {
    if (Storage.exists(newCachePath.c_str()) || !Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) ok = false;
  }

  if (FsHelpers::hasTxtExtension(oldPath) || FsHelpers::hasMarkdownExtension(oldPath)) {
    const std::string oldLegacy = Txt(oldPath, "/.crosspoint").getCachePath();
    const std::string newLegacy = Txt(newPath, "/.crosspoint").getCachePath();
    if (!renameIfTargetMissing(oldLegacy, newLegacy)) ok = false;
  }

  if (FsHelpers::hasCbzExtension(oldPath)) {
    const std::string oldLegacy = Cbz(oldPath, "/.crosspoint").getCachePath();
    const std::string newLegacy = Cbz(newPath, "/.crosspoint").getCachePath();
    if (!renameIfTargetMissing(oldLegacy, newLegacy)) ok = false;
  }

  if (FsHelpers::hasFb2Extension(oldPath)) {
    const std::string oldLegacy = Fb2(oldPath, "/.crosspoint").getCachePath();
    const std::string newLegacy = Fb2(newPath, "/.crosspoint").getCachePath();
    if (!renameIfTargetMissing(oldLegacy, newLegacy)) ok = false;
  }

  if (FsHelpers::hasEpubExtension(oldPath)) {
    const std::string oldBookmarkPath = BookmarkUtil::getBookmarkPath(oldPath);
    const std::string newBookmarkPath = BookmarkUtil::getBookmarkPath(newPath);
    if (!renameIfTargetMissing(oldBookmarkPath, newBookmarkPath)) ok = false;
  }
  return ok;
}

bool relocateBookReferences(const std::string& oldPath, const std::string& newPath) {
  bool ok = true;
  const std::string oldCachePath = bookCachePath(oldPath);
  const std::string newCachePath = bookCachePath(newPath);
  if (!RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath)) ok = false;

  if (const ReadingBookStats* stats = READING_STATS.findBook(oldPath)) {
    const std::string newCoverPath = remapCacheAsset(stats->coverBmpPath, oldCachePath, newCachePath);
    if (!READING_STATS.updateBookPath(oldPath, newPath, "", "", newCoverPath)) ok = false;
  }

  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    if (!APP_STATE.saveToFile()) ok = false;
  }
  return ok;
}
