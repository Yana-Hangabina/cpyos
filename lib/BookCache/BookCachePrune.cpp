#include "BookCachePrune.h"

#include <Logging.h>

#include <HalStorage.h>

#include <string>

namespace book_cache {
namespace {

// A cache directory is small (tens of entries) and every pass that removes anything halves
// the remainder under the worst iterator model, so this is never reached in practice; it
// exists so a filesystem that reports a removal it did not perform cannot spin forever on a
// reader's cache.
constexpr unsigned kMaxPasses = 32;

// One full pass over the directory.
//
// Deleting while enumerating is safe when the iterator is *entry-positioned* (a freed dir
// entry is skipped as empty, which is how SdFat walks a directory) but silently skips every
// entry that follows a removal when it is *index-positioned* (children shift down into the
// hole).  The host mock in test/book_cache_prune exposed exactly that: half of a seeded
// cache survived the sweep.  So a pass reports how many entries it actually removed and the
// caller repeats until a pass removes nothing -- converging under either model, allocating
// nothing, and costing one extra directory scan per halving.
struct PassResult {
  unsigned removed = 0;
  unsigned kept = 0;
  bool failed = false;
};

PassResult runPass(const std::string& cachePath) {
  PassResult result;
  auto dir = Storage.open(cachePath.c_str());
  if (!dir) return result;

  char name[128];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (!shouldDelete(name)) {
      ++result.kept;
      file.close();
      continue;
    }
    const bool isDir = file.isDirectory();
    std::string entryPath = cachePath;
    if (!entryPath.empty() && entryPath.back() != '/') entryPath += '/';
    entryPath += name;
    // Close the entry before removing it: the shipped recursive delete (SDCardManager's
    // removeDir) and the settings-page sweep both do, and a live handle makes the removal
    // fail on this stack.
    file.close();
    if (isDir ? Storage.removeDir(entryPath.c_str()) : Storage.remove(entryPath.c_str())) {
      ++result.removed;
    } else {
      // One stuck entry must not abandon the rest of the directory, but it must not be
      // reported as a clean clear either: the caller shows success/failure to the user, and
      // the blob they were trying to get rid of is still there.
      result.failed = true;
      LOG_ERR("CACHE", "Failed to remove %s", entryPath.c_str());
    }
  }
  dir.close();
  return result;
}

}  // namespace

bool pruneRegenerable(const std::string& cachePath, unsigned& deletedOut, unsigned& keptOut) {
  deletedOut = 0;
  keptOut = 0;

  if (cachePath.empty()) return false;  // never sweep "/" by accident

  auto probe = Storage.open(cachePath.c_str());
  if (!probe) {
    if (!Storage.exists(cachePath.c_str())) return true;  // nothing was ever cached
    LOG_ERR("CACHE", "Cannot open cache directory for pruning: %s", cachePath.c_str());
    return false;
  }
  const bool isDirectory = probe.isDirectory();
  probe.close();
  if (!isDirectory) {
    // A file where a directory was expected is a corrupt layout, not an empty cache.
    LOG_ERR("CACHE", "Cache path is not a directory: %s", cachePath.c_str());
    return false;
  }

  bool ok = true;
  PassResult last;
  unsigned rounds = 0;
  do {
    last = runPass(cachePath);
    deletedOut += last.removed;
    keptOut = last.kept;
    if (last.failed) ok = false;
    ++rounds;
  } while (last.removed > 0 && rounds < kMaxPasses);

  if (last.removed > 0) {
    LOG_ERR("CACHE", "Cache sweep did not converge after %u passes: %s", rounds, cachePath.c_str());
    return false;
  }
  LOG_DBG("CACHE", "pruned %s: %u removed, %u kept", cachePath.c_str(), deletedOut, keptOut);
  return ok;
}

bool pruneRegenerable(const std::string& cachePath) {
  unsigned deleted = 0;
  unsigned kept = 0;
  return pruneRegenerable(cachePath, deleted, kept);
}

}  // namespace book_cache
