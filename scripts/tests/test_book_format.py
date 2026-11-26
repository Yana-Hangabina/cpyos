"""Pin the shelf/file-manager format tags (src/util/BookFormat.h).

The badge is pure display, but it is also a claim ("this row is an EPUB"), so
the two rules that can silently go wrong are pinned here:
  * every format the reader can open maps to its own tag,
  * the WeRead and JJWXC shelves never get a tag (those books are read through
    their apps, not as card files).

The FsHelpers extension checks are compiled from the production source; only
their declarations come from a stub header (the real one pulls in Arduino).
"""
from pathlib import Path
import tempfile
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp

# Declarations only: BookFormat.h includes <FsHelpers.h>, and the definitions
# below are the production ones.
STUB = r'''
#pragma once
#include <string_view>
namespace FsHelpers {
bool checkFileExtension(std::string_view fileName, const char* extension);
bool hasEpubExtension(std::string_view fileName);
bool hasPdfExtension(std::string_view fileName);
bool hasFb2Extension(std::string_view fileName);
bool hasXtcExtension(std::string_view fileName);
bool hasCbzExtension(std::string_view fileName);
bool hasMarkdownExtension(std::string_view fileName);
bool hasTxtExtension(std::string_view fileName);
}
'''


class BookFormatTest(unittest.TestCase):
    def test_badges_and_store_exclusion(self):
        fs = (ROOT / 'lib/FsHelpers/FsHelpers.cpp').read_text()
        helpers = '\n'.join(method(fs, name) for name in (
            'bool checkFileExtension(', 'bool hasEpubExtension(', 'bool hasPdfExtension(',
            'bool hasFb2Extension(', 'bool hasXtcExtension(', 'bool hasCbzExtension(',
            'bool hasMarkdownExtension(', 'bool hasTxtExtension('))
        with tempfile.TemporaryDirectory(prefix='book-format-stub-') as stub_dir:
            (Path(stub_dir) / 'FsHelpers.h').write_text(STUB)
            run_cpp(r'''
#include <cassert>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
namespace FsHelpers {
''' + helpers + r'''
}
#include "util/BookFormat.h"

int main() {
  using book_format::badgeFor;
  assert(std::strcmp(badgeFor("novel.epub"), "EPUB") == 0);
  assert(std::strcmp(badgeFor("NOVEL.EPUB"), "EPUB") == 0);
  assert(std::strcmp(badgeFor("scan.pdf"), "PDF") == 0);
  assert(std::strcmp(badgeFor("book.fb2"), "FB2") == 0);
  assert(std::strcmp(badgeFor("comic.xtc"), "XTC") == 0);
  assert(std::strcmp(badgeFor("comic.xtch"), "XTC") == 0);
  assert(std::strcmp(badgeFor("volume.cbz"), "CBZ") == 0);
  assert(std::strcmp(badgeFor("notes.md"), "MD") == 0);
  assert(std::strcmp(badgeFor("notes.txt"), "TXT") == 0);
  // Not books: images, folders, unknown extensions, empty names.
  assert(badgeFor("cover.jpg") == nullptr);
  assert(badgeFor("cover.PNG") == nullptr);
  assert(badgeFor("folder/") == nullptr);
  assert(badgeFor("archive.zip") == nullptr);
  assert(badgeFor("") == nullptr);
  // Store shelves are excluded from the shelf tag.
  assert(book_format::isStorePath("/WeRead/a.epub"));
  assert(book_format::isStorePath("/JJWXC/a.epub"));
  assert(!book_format::isStorePath("/books/a.epub"));
  assert(book_format::badgeForLocalPath("/WeRead/a.epub") == nullptr);
  assert(book_format::badgeForLocalPath("/JJWXC/a.epub") == nullptr);
  assert(std::strcmp(book_format::badgeForLocalPath("/books/a.epub"), "EPUB") == 0);
  return 0;
}
''', include_dirs=(stub_dir,))


if __name__ == '__main__':
    unittest.main()
