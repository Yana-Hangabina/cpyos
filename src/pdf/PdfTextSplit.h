#pragma once

#include <string>
#include <vector>

// The pdfmod engine hands the app one text buffer per page, in which MuPDF's
// structured-text output separates *blocks* with a blank line ("\n\n") and
// keeps everything else verbatim. Splitting that buffer is what decides the
// unit `pdf_merge` then reasons about, so it is the load-bearing half of the
// reflow pipeline -- yet it lives in device-only code (`PdfDocumentXip.cpp`)
// where nothing can test it.
//
// This header is the same loop, extracted verbatim so a host test can pin its
// edges (test/pdf_line_merge covers the merge rules; this covers the split):
//   * a paragraph is text between blank lines, not between newlines;
//   * a buffer that ends without a trailing blank line still yields its last
//     paragraph (the `i == n` case, which is where an off-by-one silently drops
//     the tail of every page);
//   * the `i + 1 < n` guard means a lone trailing "\n" is content, not a break;
//   * whitespace-only input yields no paragraphs at all, which the caller reads
//     as "this page has no text" and renders as a bitmap.
//
// Pure, header-only, no HAL or ESP headers.
namespace pdf_text {

inline std::vector<std::string> splitParagraphs(const char* buffer, const size_t n) {
  std::vector<std::string> paragraphs;
  if (buffer == nullptr) return paragraphs;
  size_t start = 0;
  for (size_t i = 0; i <= n; ++i) {
    if (i == n || (i + 1 < n && buffer[i] == '\n' && buffer[i + 1] == '\n')) {
      if (i > start) paragraphs.emplace_back(buffer + start, i - start);
      if (i + 1 < n) ++i;
      start = i + 1;
    }
  }
  return paragraphs;
}

}  // namespace pdf_text
