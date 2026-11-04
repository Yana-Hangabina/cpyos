#pragma once

#include <cstddef>
#include <string_view>

#include "WebTypes.h"

namespace webbrowser {

struct ExtractOptions {
  // A page larger than this is not a reading page; truncate instead of filling
  // PSRAM and paginating a novel-sized DOM.
  size_t maxTextBytes = 768 * 1024;
  size_t maxBlocks = 12000;
  size_t maxLinks = 1200;
};

// Turns a fetched page into a linear reading document: paragraph/heading/list
// blocks of plain text plus resolved links. Scripts, styles, form controls and
// chrome containers (nav/footer/sidebar/ad/comment/...) never reach the text.
bool extractDocument(std::string_view html, std::string_view documentUrl, Document& out,
                     const ExtractOptions& options = {});

// Exposed for tests: tags whose entire subtree is non-content.
bool isSkippedTag(std::string_view name);

}  // namespace webbrowser
