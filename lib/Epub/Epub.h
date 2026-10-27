#pragma once
#include <CancelCheck.h>
#include <ContentProtection.h>
#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUB file?
  std::string filepath;
  // Logical source path used for progress, bookmarks, recents and stats. For a
  // converted FB2 this is the .fb2 on the card; filepath is its generated EPUB.
  // Equal to filepath for every other book.
  std::string sourcePath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  // Font files declared in the OPF manifest (A5: EPUB-embedded fonts). Absolute
  // zip paths, in manifest order; the reader picks the first loadable one.
  std::vector<std::string> embeddedFonts;
  // Optional encrypted-entry accessor. Entries are decoded in memory and stay
  // encrypted at rest. Null when the accessor is not needed or unavailable.
  std::unique_ptr<freeink::content::ContentDecryptor> decryptor;
  // User-presentable reason the encrypted-entry accessor could not be opened.
  std::string protectionError;

  bool findContentOpfFile(std::string* contentOpfFile, ZipFile* sharedZip = nullptr) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true,
                       bool metadataOnly = false, ZipFile* sharedZip = nullptr);
  bool generateThumbBmpForCover(int height, const std::string& coverImageHref) const;
  bool openProtection();
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  CssParser::ParseResult parseCssFiles(CssParser::CacheStatus existingCacheStatus) const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir);
  Epub(std::string filepath, std::string sourcePath, const std::string& cacheDir);
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool loadMetadata(std::string& title, std::string& author);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getSourcePath() const { return sourcePath; }
  // Empty unless the encrypted-entry accessor failed to open.
  const std::string& getProtectionError() const { return protectionError; }
  // True when this package carries encrypted entries and the per-book key was
  // opened (openProtection()). Used to decide whether the reader's plaintext
  // section/html caches may be left on the card after a session.
  bool isProtected() const { return decryptor != nullptr; }
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  // Font files declared in the OPF manifest (A5: EPUB-embedded fonts), in
  // manifest order; the reader picks the first loadable one.
  const std::vector<std::string>& getEmbeddedFonts() const { return embeddedFonts; }
  // Fill embeddedFonts from the package's own file list when the OPF-derived list
  // is empty. Needed because load() returns from the metadata cache without
  // re-parsing the OPF, so the manifest-derived list only exists on a cache build.
  // Bounded and idempotent; a no-op once the list is non-empty.
  void discoverEmbeddedFontsFromZip();
  std::string getCoverOverridePath() const;
  bool hasCoverOverride() const;
  std::string getCoverBmpPath(bool cropped = false, bool originalThresholds = false) const;
  bool generateCoverBmp(bool cropped = false, bool originalThresholds = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  // Locate the cover without building spine, TOC, or reading caches.
  bool generateThumbBmpFromSource(int height);
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize, bool allowEarlyStop = false,
                                size_t psramChunkSize = 0, CancelCheck cancellation = {}) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath, CancelCheck cancellation = {}) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
