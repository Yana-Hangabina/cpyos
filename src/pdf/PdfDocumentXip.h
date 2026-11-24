#pragma once

#if !defined(CROSSPOINT_PDF_XIP)
#error "PdfDocumentXip.h is only for CROSSPOINT_PDF_XIP builds"
#endif

#include <string>
#include <vector>

#include "PdfModuleApi.h"

/// App-side facade for the MuPDF engine in the `pdfmod` partition.  The
/// public surface intentionally mirrors PdfDocument.h so PdfReaderActivity
/// does not need a second layout/cache/annotation implementation.
class PdfDocument {
 public:
  explicit PdfDocument(std::string path);
  ~PdfDocument();

  PdfDocument(const PdfDocument&) = delete;
  PdfDocument& operator=(const PdfDocument&) = delete;

  bool load();
  int pageCount() const { return pageCount_; }
  bool pageHasText(int page);

  /// Load the XIP engine from flash on the calling task.  This must happen on
  /// the UI task (internal stack): the module loader uses esp_partition_read,
  /// and IDF refuses that from a PSRAM-stack task.
  static bool preloadEngine();

  void setPassword(std::string password) { password_ = std::move(password); }
  bool needsPassword() const { return passwordRequired_; }
  bool wrongPassword() const { return passwordWrong_; }

  bool extractText(int page, std::vector<std::string>& paragraphs, bool& hasImages);

  struct OutlineEntry {
    std::string title;
    int page = -1;
  };
  const std::vector<OutlineEntry>& outline() const { return outline_; }

  bool renderPageToBmp(int page, const std::string& bmpPath, int maxWidth, int maxHeight, bool cover = false);
  bool writeBlankBmp(const std::string& bmpPath, int maxWidth, int maxHeight);

  void trimMemory();
  void setUseDisplayList(bool enabled);
  /// Whether the engine may split one MuPDF text block into the paragraphs it fused
  /// (src/pdf/PdfLineJoin.h).  This is the reader's 智能合并断行 option: set it before
  /// load(), and note that load() re-sends it, because opening a document -- including a
  /// retry after a wrong password -- creates fresh engine-side state.
  /// Kept in step with the module-side declaration in PdfDocument.h by hand: the two
  /// headers declare the same class name for the two images, which is exactly the shape of
  /// mistake that makes one side gain a member the other cannot see.  Adding a method to
  /// one header without the other fails the app build, so the drift is loud.
  void setParagraphSplit(bool enabled);
  bool ensureTileBuffer(int width);

  const std::string& path() const { return path_; }

 private:
  void close();

  std::string path_;
  std::string password_;
  bool passwordRequired_ = false;
  bool passwordWrong_ = false;
  void* handle_ = nullptr;
  int pageCount_ = 0;
  bool useDisplayList_ = false;
  bool splitParagraphs_ = true;  // matches SETTINGS.pdfSmartLineMerge's shipped default
  std::vector<OutlineEntry> outline_;
};
