#pragma once

#include <string>

namespace BookCoverLoader {

std::string ensureThumbnail(const std::string& bookPath, int height, bool* generated = nullptr);

// Cache probe: the thumbnail path when a VALID cached BMP already exists,
// otherwise "". Never generates — lets callers show cached covers in the same
// frame they become visible instead of waiting for the per-frame generator.
std::string cachedThumbnail(const std::string& bookPath, int height);
std::string ensureFullCover(const std::string& bookPath, std::string* title = nullptr, std::string* author = nullptr,
                            bool* generated = nullptr);

}  // namespace BookCoverLoader
