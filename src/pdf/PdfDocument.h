#pragma once

#if defined(CROSSPOINT_MUPDF)

#include <string>
#include <vector>

#include "mupdf/fitz.h"

/// Thin owner of one MuPDF document opened through a HalFile-backed stream.
/// MuPDF's default fz_open_file() uses stdio, which does not see CrossMux's
/// SdFat volume; the custom stream below is what makes SD PDFs work.
class PdfDocument {
 public:
  explicit PdfDocument(std::string path);
  ~PdfDocument();

  PdfDocument(const PdfDocument&) = delete;
  PdfDocument& operator=(const PdfDocument&) = delete;

  bool load();
  int pageCount() const { return pageCount_; }
  bool pageHasText(int page);

  /// Extracts the page's text as paragraphs for the reflow path.  Returns false
  /// when there is no extractable text.  `hasImages` is set when the page draws
  /// images, in which case the caller must keep the bitmap rendering (figures
  /// and tables would be lost by reflowing).
  bool extractText(int page, std::vector<std::string>& paragraphs, bool& hasImages);

  struct OutlineEntry {
    std::string title;
    int page = -1;
  };
  const std::vector<OutlineEntry>& outline() const { return outline_; }

  /// Renders one page to an 8-bit grayscale BMP.  With cover=false the page is
  /// fitted inside maxWidth x maxHeight; with cover=true it fills the box and
  /// is center-cropped (used for image-only pages).
  bool renderPageToBmp(int page, const std::string& bmpPath, int maxWidth, int maxHeight, bool cover = false);
  bool writeBlankBmp(const std::string& bmpPath, int maxWidth, int maxHeight);

  /// WhiteOS-style memory recovery: drop cached images and trim the MuPDF store
  /// before asking the caller to retry.  Safe to call between page renders.
  void trimMemory();
  /// Only enable on a worker with a large stack: building a display list recurses
  /// deeper than the 8 KB Arduino loopTask stack can hold.
  void setUseDisplayList(bool enabled) { useDisplayList_ = enabled; }
  /// Preallocate the reusable tile buffer before page resources consume PSRAM.
  bool ensureTileBuffer(int width);

  const std::string& path() const { return path_; }

 private:
  void close();
  void loadOutline();

  std::string path_;
  fz_context* ctx_ = nullptr;
  fz_document* doc_ = nullptr;
  fz_stream* stream_ = nullptr;
  int pageCount_ = 0;
  bool useDisplayList_ = false;
  std::vector<OutlineEntry> outline_;
};

#endif
