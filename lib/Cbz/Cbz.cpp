#include "Cbz.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <ZipFile.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <string_view>

#include "CbzPageName.h"

namespace {
constexpr size_t kMaxNameBytes = 512 * 1024;
constexpr size_t kInternalNameBytes = 96 * 1024;

memory::ByteBuffer allocateTable(const size_t bytes) {
  auto psram = memory::makePsramByteBufferNoThrow(bytes);
  if (psram) return psram;
  if (bytes > kInternalNameBytes) return {};
  return memory::makeInternalByteBufferNoThrow(bytes);
}
}  // namespace

Cbz::Cbz(std::string path, std::string cacheBasePath) : filepath(std::move(path)) {
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = std::move(cacheBasePath) + "/cbz_" + std::to_string(hash);
}

bool Cbz::isImageEntry(const std::string_view name) {
  if (name.empty() || name.front() == '.' || name.back() == '/') return false;
  if (name.rfind("__MACOSX/", 0) == 0 || name.find("/._") != std::string_view::npos) return false;
  return FsHelpers::hasJpgExtension(name) || FsHelpers::hasPngExtension(name);
}

bool Cbz::ensurePageTable(const int count, const size_t nameBytes) {
  if (count <= 0 || count > kMaxPages || nameBytes == 0 || nameBytes > kMaxNameBytes) return false;
  offsets_ = allocateTable(static_cast<size_t>(count) * sizeof(uint32_t));
  names_ = allocateTable(nameBytes);
  order_ = allocateTable(static_cast<size_t>(count) * sizeof(uint16_t));
  if (!offsets_ || !names_ || !order_) {
    LOG_ERR("CBZ", "OOM page table: %d pages, %u names", count, static_cast<unsigned>(nameBytes));
    return false;
  }
  return true;
}

bool Cbz::load() {
  if (loaded) return true;
  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("CBZ", "File does not exist: %s", filepath.c_str());
    return false;
  }

  ZipFile zip(filepath);
  int count = 0;
  size_t nameBytes = 0;
  const bool counted = zip.enumerateFileEntries([&](std::string_view path, uint32_t, uint32_t) {
    if (!isImageEntry(path)) return;
    if (count >= kMaxPages) return;
    ++count;
    nameBytes += path.size() + 1;
  });
  if (!counted || !ensurePageTable(count, nameBytes)) {
    LOG_ERR("CBZ", "No usable pages or page table too large: %s", filepath.c_str());
    return false;
  }

  auto* offsets = reinterpret_cast<uint32_t*>(offsets_.get());
  char* names = reinterpret_cast<char*>(names_.get());
  int index = 0;
  size_t used = 0;
  const bool filled = zip.enumerateFileEntries([&](std::string_view path, uint32_t, uint32_t) {
    if (index >= count || !isImageEntry(path)) return;
    offsets[index++] = static_cast<uint32_t>(used);
    memcpy(names + used, path.data(), path.size());
    names[used + path.size()] = '\0';
    used += path.size() + 1;
  });
  if (!filled || index != count || used != nameBytes) {
    LOG_ERR("CBZ", "Archive changed while listing pages: %s", filepath.c_str());
    loaded = false;
    return false;
  }

  auto* order = reinterpret_cast<uint16_t*>(order_.get());
  for (int i = 0; i < count; ++i) order[i] = static_cast<uint16_t>(i);
  std::sort(order, order + count, [this](uint16_t a, uint16_t b) {
    return cbz_page::compareNatural(rawName(a), rawName(b)) < 0;
  });

  pageCount = count;
  loaded = true;
  LOG_INF("CBZ", "Loaded %d pages from %s", pageCount, filepath.c_str());
  return true;
}

const char* Cbz::rawName(const int index) const {
  const auto* offsets = reinterpret_cast<const uint32_t*>(offsets_.get());
  return reinterpret_cast<const char*>(names_.get() + offsets[index]);
}

std::string Cbz::getTitle() const {
  size_t slash = filepath.find_last_of('/');
  size_t start = slash == std::string::npos ? 0 : slash + 1;
  size_t dot = filepath.find_last_of('.');
  if (dot == std::string::npos || dot <= start) return filepath.substr(start);
  return filepath.substr(start, dot - start);
}

std::string Cbz::getPageName(const int index) const {
  if (!loaded || index < 0 || index >= pageCount) return {};
  const auto* order = reinterpret_cast<const uint16_t*>(order_.get());
  return rawName(order[index]);
}

bool Cbz::extractPage(const int index, const std::string& destPath) const {
  if (!loaded || index < 0 || index >= pageCount) return false;
  const std::string name = getPageName(index);
  if (name.empty()) return false;

  ZipFile zip(filepath);
  HalFile output;
  if (!Storage.openFileForWrite("CBZ", destPath, output)) return false;
  const bool ok = zip.readFileToStream(name.c_str(), output, 4096);
  output.close();
  if (!ok && Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
  return ok;
}

bool Cbz::convertPage(const int index, const std::string& destBmpPath, const int maxWidth, const int maxHeight,
                      const bool oneBit) const {
  if (!loaded || maxWidth <= 0 || maxHeight <= 0) return false;
  const std::string name = getPageName(index);
  if (name.empty()) return false;
  const bool jpeg = FsHelpers::hasJpgExtension(name);
  const bool png = FsHelpers::hasPngExtension(name);
  if (!jpeg && !png) return false;

  setupCacheDir();
  const std::string sourcePath = cachePath + (jpeg ? "/.page-src.jpg" : "/.page-src.png");
  if (!extractPage(index, sourcePath)) return false;

  HalFile input;
  HalFile output;
  bool ok = false;
  if (Storage.openFileForRead("CBZ", sourcePath, input) &&
      Storage.openFileForWrite("CBZ", destBmpPath, output)) {
    if (jpeg) {
      ok = oneBit ? JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(input, output, maxWidth, maxHeight, false)
                  : JpegToBmpConverter::jpegFileToBmpStreamWithSize(input, output, maxWidth, maxHeight, false);
    } else {
      ok = oneBit ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(input, output, maxWidth, maxHeight, false)
                  : PngToBmpConverter::pngFileToBmpStreamWithSize(input, output, maxWidth, maxHeight);
    }
    output.flush();
    if (!ok) {
      input.close();
      output.close();
      Storage.remove(destBmpPath.c_str());
    }
  }
  input.close();
  output.close();
  Storage.remove(sourcePath.c_str());
  return ok;
}

bool Cbz::renderPageToBmp(const int index, const std::string& destBmpPath, const int maxWidth,
                          const int maxHeight) const {
  return convertPage(index, destBmpPath, maxWidth, maxHeight, true);
}

void Cbz::setupCacheDir() const { Storage.mkdir(cachePath.c_str(), true); }

bool Cbz::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) return true;
  return Storage.removeDir(cachePath.c_str());
}

std::string Cbz::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

std::string Cbz::getThumbBmpPath(const int height) const {
  return cachePath + "/thumb_" + std::to_string(height) + ".bmp";
}

bool Cbz::generateCoverBmp() const {
  if (Storage.exists(getCoverBmpPath().c_str())) return true;
  return convertPage(0, getCoverBmpPath(), 600, 900, false);
}

bool Cbz::generateThumbBmp(const int height) const {
  if (height <= 0) return false;
  const std::string path = getThumbBmpPath(height);
  if (Storage.exists(path.c_str())) return true;
  return convertPage(0, path, height, height, false);
}
