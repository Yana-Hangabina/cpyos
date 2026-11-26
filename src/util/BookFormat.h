#pragma once

#include <FsHelpers.h>

#include <string>
#include <string_view>

// Short, uppercase format tags for local books and documents ("EPUB", "PDF",
// "FB2", ...). Pure display: the reader dispatch itself still lives in
// ReaderActivity::create(). Returns nullptr for folders, images and anything
// else the reader cannot open.
//
// The tags are format acronyms, not translated prose, so they deliberately do
// not go through tr(). Callers that need the store shelves excluded (JJWXC
// exports, WeRead downloads) must ask isStorePath() first: those books are read
// through their own app and a "EPUB" tag there would describe the export file
// rather than what the user is looking at.
namespace book_format {

inline const char* badgeFor(std::string_view name) {
  if (name.empty() || name.back() == '/') return nullptr;
  if (FsHelpers::hasEpubExtension(name)) return "EPUB";
  if (FsHelpers::hasPdfExtension(name)) return "PDF";
  if (FsHelpers::hasFb2Extension(name)) return "FB2";
  if (FsHelpers::hasXtcExtension(name)) return "XTC";
  if (FsHelpers::hasCbzExtension(name)) return "CBZ";
  if (FsHelpers::hasMarkdownExtension(name)) return "MD";
  if (FsHelpers::hasTxtExtension(name)) return "TXT";
  return nullptr;
}

// "/WeRead/..." (WeRead downloads) and "/JJWXC/..." (the JJWXC export shelf).
inline bool isStorePath(std::string_view path) {
  return path.rfind("/WeRead/", 0) == 0 || path.rfind("/JJWXC/", 0) == 0;
}

// Badge for a card path, with the two store shelves excluded.
inline const char* badgeForLocalPath(const std::string& path) {
  if (path.empty() || isStorePath(path)) return nullptr;
  return badgeFor(path);
}

}  // namespace book_format
