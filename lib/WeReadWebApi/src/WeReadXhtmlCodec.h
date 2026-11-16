#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace WeReadXhtmlCodec {

bool writeLiteral(HalFile& output, const char* text);
bool writeXmlText(HalFile& output, const char* text);

bool sanitizeChapter(const std::string& inputPath, const std::string& outputPath, const std::string& imageIndexPath,
                     uint32_t chapterIndex, const char* title, bool plainText, uint8_t* readBuffer,
                     size_t readBufferSize, char* tagBuffer, size_t tagBufferSize);

// Writes a localized "this chapter cannot be read" page plus an empty image
// index. Used for paid/locked/preview chapters so the rest of the book still
// caches and the reader shows the real reason in place of the chapter.
bool writeUnavailableChapter(const std::string& outputPath, const std::string& imageIndexPath, const char* title,
                             const char* message);
// True when `path` is a placeholder written above. Used to re-fetch a chapter
// after the account purchases it instead of reusing the placeholder forever.
bool isPlaceholderChapter(const std::string& path);

bool visibleToNativeOffset(const std::string& path, uint32_t visibleOffset, uint32_t& nativeOffset);
bool nativeToVisibleOffset(const std::string& path, uint32_t nativeOffset, uint32_t& visibleOffset);

}  // namespace WeReadXhtmlCodec
