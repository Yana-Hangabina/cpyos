#pragma once

// Which entries of a per-book cache directory a cache clear must NOT delete.
//
// Every reader keeps its cache in `/.crosspoint/<fmt>_<hash>/` and every clear path used
// to call `Storage.removeDir()` on that directory.  That directory holds two different
// kinds of file:
//
//   * regenerable artifacts -- page/section bitmaps, `book.bin`, `reflow.bin`, `html/`,
//     `sections/`, `thumb_*.bmp`, version markers: deleting them costs one rebuild;
//   * the reader's own data -- `annotations.bin` (highlights and notes), `bookmarks.bin`,
//     `progress.bin`, `annotations.txt` (an export the user asked for): deleting these
//     loses work the reader made, permanently.
//
// So "Clear Reading Cache" was, up to now, a destructive action on user data in all six
// formats -- both from the settings page (which wipes every book cache at once) and from
// the per-book menu entry, and silently for PDF, whose cache is dropped automatically
// whenever the file's size or mtime changes.
//
// The rule is an allowlist of user-data names rather than a denylist of cache names,
// because the regenerable set is large and grows with each renderer: a newly added cache
// artifact must default to deletable (it costs a rebuild), whereas a newly added user
// file defaulting to deletable would be a data-loss bug nobody notices until a reader
// loses their highlights.  Pure and header-only so a host test can pin the rule
// (test/book_cache_prune/).
#include <cstddef>
#include <cstring>
#include <string>

namespace book_cache {

// The reader's own files, by name.  These are not just a cache-clear detail: the names are
// what is ALREADY on readers' cards, so renaming one silently orphans every highlight and
// bookmark taken so far.  They are therefore constants (pinned by name in
// test/book_cache_prune), and the writers use them too -- `annotations.txt` in particular is
// produced by AnnotationExport and must never be a string that only the exporter knows.
inline constexpr const char* kAnnotationsFile = "annotations.bin";
inline constexpr const char* kAnnotationsExportFile = "annotations.txt";
inline constexpr const char* kBookmarksFile = "bookmarks.bin";
inline constexpr const char* kProgressFile = "progress.bin";

// Names that survive a cache clear, in every format that has them.
inline bool isUserDataName(const char* name) {
  if (!name) return false;
  static constexpr const char* const kKeep[] = {kAnnotationsFile, kAnnotationsExportFile, kBookmarksFile,
                                                kProgressFile};
  for (const char* kept : kKeep) {
    if (std::strcmp(name, kept) == 0) return true;
  }
  return false;
}

// True when `name` ends with `suffix`.
inline bool endsWith(const char* name, const char* suffix) {
  const size_t nameLen = std::strlen(name);
  const size_t suffixLen = std::strlen(suffix);
  return nameLen >= suffixLen && std::strcmp(name + (nameLen - suffixLen), suffix) == 0;
}

// Should this directory entry be deleted by a cache clear?
//
// Atomic-write siblings of kept files ("progress.bin.tmp", "annotations.bin.bak") are
// kept too.  They are written by `replaceFile()`/`ProgressFile` immediately before the
// real file, so a `.bak`/`.tmp` can be the ONLY surviving copy of a reader's notes after a
// write was cut short by a power loss -- deleting it while trying to be tidy about stray
// files would destroy exactly the data this function exists to protect.  The cost is that
// one stale temp file outlives a cache clear, and the next save overwrites it.
inline bool shouldDelete(const char* name) {
  if (!name || name[0] == '\0') return false;  // never act on an unnamed entry
  if (isUserDataName(name)) return false;
  static constexpr const char* kTempSuffixes[] = {".tmp", ".bak", ".part"};
  for (const char* suffix : kTempSuffixes) {
    if (endsWith(name, suffix)) {
      char base[64];
      const size_t cut = std::strlen(name) - std::strlen(suffix);
      if (cut < sizeof(base)) {
        std::memcpy(base, name, cut);
        base[cut] = '\0';
        if (isUserDataName(base)) return false;
      }
    }
  }
  return true;
}

// Removes every regenerable entry of `cachePath` and keeps the directory itself, so the
// reader's own files stay addressable.  A missing directory is success (nothing was ever
// cached); a failed removal is reported false after the rest of the sweep has run, so one
// stuck entry cannot leave the other caches uncleaned.  The counts let a caller report how
// much was actually dropped.
bool pruneRegenerable(const std::string& cachePath, unsigned& deletedOut, unsigned& keptOut);
bool pruneRegenerable(const std::string& cachePath);

}  // namespace book_cache
