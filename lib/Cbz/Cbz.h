#pragma once

#include <Memory.h>

#include <cstdint>
#include <string>
#include <string_view>

// CBZ comic archive reader. Pages are listed once from the ZIP central
// directory, sorted naturally (page2 before page10), and kept in a compact
// PSRAM-backed table so page turns do not allocate.
class Cbz {
 public:
  static constexpr int kMaxPages = 4096;

  explicit Cbz(std::string path, std::string cacheBasePath);

  bool load();
  bool isLoaded() const { return loaded; }

  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] int getPageCount() const { return pageCount; }

  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] std::string getAuthor() const { return {}; }

  // Page entry name inside the ZIP, in reading order.
  [[nodiscard]] std::string getPageName(int index) const;

  // Extract a page image to destPath. The caller chooses the extension from
  // getPageName(); this only copies the (deflated) entry bytes.
  bool extractPage(int index, const std::string& destPath) const;

  // Render a page directly to a 1-bit BMP scaled to fit maxWidth x maxHeight.
  bool renderPageToBmp(int index, const std::string& destBmpPath, int maxWidth, int maxHeight) const;

  void setupCacheDir() const;
  bool clearCache() const;

  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] std::string getThumbBmpPath(int height) const;
  bool generateCoverBmp() const;
  bool generateThumbBmp(int height) const;

 private:
  static bool isImageEntry(std::string_view name);
  bool ensurePageTable(const int count, const size_t nameBytes);
  bool convertPage(int index, const std::string& destBmpPath, int maxWidth, int maxHeight, bool oneBit) const;
  const char* rawName(int index) const;

  std::string filepath;
  std::string cachePath;
  bool loaded = false;
  int pageCount = 0;
  memory::ByteBuffer names_;
  memory::ByteBuffer offsets_;
  memory::ByteBuffer order_;
};
