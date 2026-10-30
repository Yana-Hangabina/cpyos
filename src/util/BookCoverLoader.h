#pragma once

#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace BookCoverLoader {

// A decoded-once thumbnail file kept in PSRAM. Grid/list screens re-read and
// re-parse the same BMPs on every repaint (a cover move repaints the frame), so a
// shared cache turns that into memory reads. Static, so it survives the
// Activity being recreated by a Home <-> Library switch.
struct CachedThumbFile {
  memory::ByteBuffer bytes;
  size_t size = 0;
};

// Returns the cached bytes for `path` (nullptr when absent). The returned span
// is owned by the loader and valid until the next store of the same slot.
const CachedThumbFile* thumbFile(const std::string& path);
// Best-effort: reads `path` into the cache (bounded size). Never fatal.
const CachedThumbFile* cacheThumbFile(const std::string& path);
void clearThumbFileCache();

std::string ensureThumbnail(const std::string& bookPath, int height, bool* generated = nullptr);

// Cache probe: the thumbnail path when a VALID cached BMP already exists,
// otherwise "". Never generates — lets callers show cached covers in the same
// frame they become visible instead of waiting for the per-frame generator.
std::string cachedThumbnail(const std::string& bookPath, int height);
std::string ensureFullCover(const std::string& bookPath, std::string* title = nullptr, std::string* author = nullptr,
                            bool* generated = nullptr);

}  // namespace BookCoverLoader
