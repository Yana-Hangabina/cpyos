#pragma once

#include <cstddef>
#include <cstdint>

#include "WebTypes.h"

namespace webbrowser {

enum LineFlags : uint8_t {
  LineHeading = 1u << 0,
  LinePre = 1u << 1,
  LineList = 1u << 2,
  LineQuote = 1u << 3,
};

struct PagerSpec {
  int contentWidth = 0;
  int pageHeight = 0;
  int lineHeight = 0;
  int blockGap = 8;
  int headingGap = 14;
  int maxLines = 20000;
  int maxPages = 900;
  // Width of a UTF-8 substring in pixels. The caller caches whatever the font
  // layer makes expensive; the pager only sums single codepoints.
  int (*measure)(void* context, const char* utf8, size_t length, bool bold) = nullptr;
  // Exact inter-word space advance. Measured as a one-character string a space
  // comes out narrower than the space the renderer inserts between two glyphs,
  // which under-measures every line (the browser's first device page overflowed
  // by ~8%). Left/right are the adjacent code points (0 at line edges).
  int (*spaceAdvance)(void* context, uint32_t leftCp, uint32_t rightCp, bool bold) = nullptr;
  void* measureContext = nullptr;
  // Verify every wrapped range with a whole-string measure. Exact, but on this
  // device a whole CJK line costs ~50 ms (the font decompressor walks glyph
  // groups), so the product leaves it off and relies on the glyph-width model.
  bool verifyWholeLines = false;
};

struct PagedLine {
  uint32_t textStart = 0;
  uint32_t textLength = 0;
  int32_t y = 0;
  uint32_t blockIndex = 0;
  uint8_t flags = 0;
};

struct PagedPage {
  uint32_t firstLine = 0;
  uint32_t lineCount = 0;
  int32_t yOffset = 0;  // page index * pageHeight
};

struct Pagination {
  WebVector<PagedLine> lines;
  WebVector<PagedPage> pages;
  bool truncated = false;

  void clear() {
    lines.clear();
    pages.clear();
    truncated = false;
  }
};

// Wraps a Document into lines and groups them into fixed-height pages.
//
// Break opportunities are: spaces, after a hyphen, and between CJK ideographs
// (with the usual prohibition before closing punctuation and after opening
// punctuation). Lines are never split across pages; a line that would straddle
// a page boundary moves to the next page instead of being clipped.
bool paginate(const Document& document, const PagerSpec& spec, Pagination& out);

}  // namespace webbrowser
