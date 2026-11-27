#if defined(CROSSPOINT_PDF_XIP)
#include "PdfDocumentXip.h"
#include <FaultLog.h>
#include <Memory.h>

#include <Logging.h>

#include <cstring>

#include "PdfModuleRuntime.h"

PdfDocument::PdfDocument(std::string path) : path_(std::move(path)) {}

PdfDocument::~PdfDocument() { close(); }

bool PdfDocument::preloadEngine() { return pdf_module::ensureLoaded(); }

void PdfDocument::close() {
  const auto* table = pdf_module::ops();
  if (handle_ && table && table->close) table->close(handle_);
  if (handle_) pdf_module::noteDocumentClosed();
  handle_ = nullptr;
  pageCount_ = 0;
  outline_.clear();
}

bool PdfDocument::load() {
  close();
  passwordRequired_ = false;
  passwordWrong_ = false;
  if (!pdf_module::ensureLoaded()) {
    LOG_ERR("PDFM", "no PDF engine module");
    return false;
  }
  const auto* table = pdf_module::ops();
  if (!table || !table->open) return false;
  handle_ = table->open(path_.c_str(), password_.empty() ? nullptr : password_.c_str());
  if (!handle_) {
    LOG_ERR("PDFM", "cannot open %s in engine", path_.c_str());
    return false;
  }
  // Live from this line on: every failure path below calls close(), which is
  // what balances this.
  pdf_module::noteDocumentOpened();
  if (table->needs_password && table->needs_password(handle_)) {
    passwordRequired_ = true;
    passwordWrong_ = table->wrong_password && table->wrong_password(handle_) != 0;
    close();
    return false;
  }
  pageCount_ = table->page_count(handle_);
  if (pageCount_ <= 0) {
    close();
    return false;
  }
  if (table->set_display_list) table->set_display_list(handle_, useDisplayList_ ? 1 : 0);
  // Re-sent on every open, like the display-list flag above: `load()` also runs when a
  // wrong password is retried, and the engine-side document is new each time.
  if (table->set_paragraph_split) table->set_paragraph_split(handle_, splitParagraphs_ ? 1 : 0);

  const int count = table->outline_count ? table->outline_count(handle_) : 0;
  outline_.reserve(static_cast<size_t>(count > 0 ? count : 0));
  for (int i = 0; i < count; ++i) {
    char title[256] = {};
    int page = -1;
    if (table->outline_at && table->outline_at(handle_, i, title, sizeof(title), &page) && title[0]) {
      outline_.push_back({title, page});
    }
  }
  LOG_INF("PDFM", "opened pages=%d outline=%u", pageCount_, static_cast<unsigned>(outline_.size()));
  return true;
}

void PdfDocument::setParagraphSplit(bool enabled) {
  splitParagraphs_ = enabled;
  const auto* table = pdf_module::ops();
  // Null when the engine predates ABI 3 -- in which case its own header rejected the pair,
  // so this is the same-tree path where the op exists.
  if (handle_ && table && table->set_paragraph_split) table->set_paragraph_split(handle_, enabled ? 1 : 0);
}

bool PdfDocument::pageHasText(int page) {
  const auto* table = pdf_module::ops();
  return handle_ && table && table->page_has_text && table->page_has_text(handle_, page) != 0;
}

bool PdfDocument::extractText(int page, std::vector<std::string>& paragraphs, bool& hasImages) {
  paragraphs.clear();
  hasImages = false;
  const auto* table = pdf_module::ops();
  if (!handle_ || !table || !table->extract_text) return false;
  // Called from the 8 KB UI loop as well as the PDF worker; this must not be a
  // stack buffer. It used to be a throwing std::vector<char>(16384): after any
  // Wi-Fi session the largest *internal* block falls to 8-11 KB on this board, and
  // under -fno-exceptions the failed `new` aborted the reader (PNC-04 class).
  // The buffer is CPU-only, so PSRAM is the right home; both allocations are
  // no-throw and a failure returns false instead of dying.
  constexpr size_t kTextBufferBytes = 16384;
  auto storage = memory::makePsramByteBufferNoThrow(kTextBufferBytes);
  if (storage.get() == nullptr) storage = memory::makeInternalByteBufferNoThrow(kTextBufferBytes);
  if (storage.get() == nullptr) {
    LOG_ERR("PDF", "no %u B buffer for page text; page treated as textless", static_cast<unsigned>(kTextBufferBytes));
    fault::raise(fault::Code::PdfTextBufferRefused, static_cast<uint32_t>(kTextBufferBytes));
    return false;
  }
  char* buffer = reinterpret_cast<char*>(storage.get());
  int images = 0;
  const int n = table->extract_text(handle_, page, buffer, static_cast<int>(kTextBufferBytes) - 1, &images);
  if (n <= 0) return false;
  buffer[kTextBufferBytes - 1] = '\0';
  hasImages = images != 0;
  size_t start = 0;
  for (size_t i = 0; i <= static_cast<size_t>(n); ++i) {
    if (i == static_cast<size_t>(n) || (i + 1 < static_cast<size_t>(n) && buffer[i] == '\n' && buffer[i + 1] == '\n')) {
      if (i > start) paragraphs.emplace_back(buffer + start, i - start);
      if (i + 1 < static_cast<size_t>(n)) ++i;
      start = i + 1;
    }
  }
  return !paragraphs.empty();
}

bool PdfDocument::renderPageToBmp(int page, const std::string& bmpPath, int maxWidth, int maxHeight, bool cover) {
  const auto* table = pdf_module::ops();
  return handle_ && table && table->render_bmp &&
         table->render_bmp(handle_, page, bmpPath.c_str(), maxWidth, maxHeight, cover ? 1 : 0) != 0;
}

bool PdfDocument::writeBlankBmp(const std::string& bmpPath, int maxWidth, int maxHeight) {
  const auto* table = pdf_module::ops();
  return handle_ && table && table->write_blank &&
         table->write_blank(handle_, bmpPath.c_str(), maxWidth, maxHeight) != 0;
}

void PdfDocument::trimMemory() {
  const auto* table = pdf_module::ops();
  if (handle_ && table && table->trim) table->trim(handle_);
}

void PdfDocument::setUseDisplayList(bool enabled) {
  useDisplayList_ = enabled;
  const auto* table = pdf_module::ops();
  if (handle_ && table && table->set_display_list) table->set_display_list(handle_, enabled ? 1 : 0);
}

bool PdfDocument::ensureTileBuffer(int width) {
  const auto* table = pdf_module::ops();
  return handle_ && table && table->ensure_tile ? table->ensure_tile(handle_, width) != 0 : false;
}

#endif
