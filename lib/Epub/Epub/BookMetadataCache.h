#pragma once

#include <BufferedFile.h>
#include <HalStorage.h>

#include <algorithm>
#include <memory>
#include <string>

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
  };

  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize;
    int16_t tocIndex;
    // OPF linear="no": an auxiliary document with no place in chapter order or
    // progress weights (notes, cover pages, ad pages).
    bool linear;

    SpineEntry() : cumulativeSize(0), tocIndex(-1), linear(true) {}
    SpineEntry(std::string href, const uint32_t cumulativeSize, const int16_t tocIndex, const bool linear = true)
        : href(std::move(href)), cumulativeSize(cumulativeSize), tocIndex(tocIndex), linear(linear) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
    TocEntry(std::string title, std::string href, std::string anchor, const uint8_t level, const int16_t spineIndex)
        : title(std::move(title)),
          href(std::move(href)),
          anchor(std::move(anchor)),
          level(level),
          spineIndex(spineIndex) {}
  };

 private:
  std::string cachePath;
  uint32_t lutOffset;
  uint16_t spineCount;
  uint16_t tocCount;
  bool loaded;
  bool buildMode;
  bool buildIoFailed = false;

  HalFile bookFile;
  // Temp file handles during build
  HalFile spineFile;
  HalFile tocFile;
  // Buffers the per-entry tmp-file writes during the OPF/TOC passes: those
  // writes interleave with zip-inflate SD reads, and unbuffered they thrash
  // SdFat's shared sector cache (one 512B transaction per 4-byte pod). One
  // wrapper serves whichever pass is active (spine, then toc).
  std::unique_ptr<serialization::BufferedFileWriter> passOut;

  // Cumulative spine sizes stay in book.bin: RAM holds only the total before every
  // SIZE_WINDOW-th item plus one decoded window. The old array cost 4 B per spine item
  // (capped at 1024 items); this costs 4 B per 32 items, so a 6000-chapter book drops
  // from 4 KB to 752 B, and a lookup reads at most one 32-entry window from the card.
  // A failed window read is reported as "not cached" so Epub falls back to the exact
  // getSpineEntry() read rather than answering an approximate size.
  static constexpr int SIZE_WINDOW = 32;
  std::unique_ptr<uint32_t[]> windowStarts;  // total before item w * SIZE_WINDOW
  uint16_t windowStartCount = 0;
  uint32_t bookSize = 0;
  mutable int windowFirst = -1;
  mutable uint32_t window[SIZE_WINDOW] = {};
  bool fillWindow(int first) const;  // caller holds the size window lock

  // One linear bit per spine item: auxiliary documents (linear="no") stay openable
  // but carry no progress weight. 6000 chapters cost 750 B, still far below a
  // full per-item cumulative-size array.
  std::unique_ptr<uint32_t[]> linearBits;
  uint16_t linearBitCount = 0;

  // Index for fast href→spineIndex lookup. Exists only during a large EPUB's
  // TOC pass; a member container would hold its storage for every open book,
  // and clear()/shrink_to_fit() cannot give the deque's node map back.
  //
  // A nothrow array rather than std::deque: resize() on a deque allocates its
  // map and nodes with throwing `new`, which aborts under -fno-exceptions when
  // the heap is tight. One makeUniqueNoThrow<Elem[]>() either yields the whole
  // index or nullptr, so createTocEntry can fall back to the linear scan.
  struct SpineHrefIndexEntry {
    uint64_t hrefHash;  // FNV-1a 64-bit hash
    uint16_t hrefLen;   // length for collision reduction
    int16_t spineIndex;
  };
  std::unique_ptr<SpineHrefIndexEntry[]> spineHrefIndex;

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;

  // FNV-1a 64-bit hash function
  static uint64_t fnvHash64(const std::string& s) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  uint32_t writeSpineEntry(HalFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(HalFile& file, const TocEntry& entry) const;
  bool readSpineEntry(HalFile& file, SpineEntry& entry) const;
  bool readTocEntry(HalFile& file, TocEntry& entry) const;
  void invalidateCorruptCache();

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)), lutOffset(0), spineCount(0), tocCount(0), loaded(false), buildMode(false) {}
  ~BookMetadataCache() = default;

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href, bool linear = true);
  bool endContentOpfPass();
  bool beginTocPass();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  TocEntry getTocEntry(int index);
  // Returns whether the size window answered with the cumulative byte size up to
  // and including `index`. False means "not cached", never "zero".
  bool getCumulativeSize(int index, uint32_t& size) const;
  // First spine index whose cumulative size reaches `size`; -1 past the end, not
  // loaded, or when the window holding the answer cannot be read.
  int getSpineIndexForSize(uint32_t size) const;
  // Whether the in-RAM cache holds the linear flag for this spine item.
  bool getLinear(int index, bool& linear) const;
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  bool isLoaded() const { return loaded; }
};
