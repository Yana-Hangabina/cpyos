// Host regression for book_cache::shouldDelete() -- the rule that decides whether a
// cache clear destroys a reader's highlights.
//
// The bug this pins: every reader's clearCache() called Storage.removeDir() on the
// per-book cache directory, which is also where annotations.bin, bookmarks.bin and
// progress.bin live.  "Clear Reading Cache" therefore deleted user work in all six
// formats, and PDF lost it silently whenever the file's size or mtime changed.
#include <BookCachePrune.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void expect(const bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

void keeps(const char* name) {
  ++checks;
  if (book_cache::shouldDelete(name)) {
    ++failures;
    std::printf("FAIL: %s must survive a cache clear\n", name);
  }
}

void drops(const char* name) {
  ++checks;
  if (!book_cache::shouldDelete(name)) {
    ++failures;
    std::printf("FAIL: %s must be deleted by a cache clear\n", name);
  }
}

// 1. The reader's own files, in every format that writes them.
void testUserDataSurvives() {
  keeps("annotations.bin");
  keeps("annotations.txt");  // an export the user asked for, not a cache
  keeps("bookmarks.bin");
  keeps("progress.bin");
}

// 2. Everything a renderer can rebuild must go, or "clear cache" frees no space and fixes
//    no corrupt pagination -- which is the whole reason the entry exists.
void testRegenerableArtifactsGo() {
  for (const char* name : {"book.bin", "meta.bin", "reflow.bin", "links.bin", "chapters.bin",
                           "index.bin", "toc.bin", "txt-cache-version", "cover.bmp", "cover.jpg",
                           "thumb_200.bmp", "thumb_1080_g8.bmp", "page_12.bmp", "0.html", "sections",
                           "html", "spine.bin", "novel.bin"}) {
    drops(name);
  }
}

// 3. A new cache artifact must default to deletable: an allowlist of USER data means a
//    forgotten addition costs one rebuild, whereas a forgotten addition to a cache denylist
//    would survive forever in a directory the user asked to empty.
void testUnknownNameDefaultsToDeleteable() {
  drops("some_future_renderer_cache.dat");
  drops("glyphs");
}

// 4. Atomic-write siblings of kept files are kept: replaceFile()/ProgressFile write
//    "<name>.tmp" and rename it over the real file, so after a power loss mid-write the
//    .tmp/.bak can be the ONLY copy of a reader's notes.
void testTempSiblingsOfUserDataAreKept() {
  keeps("progress.bin.tmp");
  keeps("annotations.bin.tmp");
  keeps("annotations.bin.bak");
  keeps("bookmarks.bin.part");
  // Siblings of cache files stay deletable -- the rule follows the base name, not ".tmp".
  drops("book.bin.tmp");
  drops("cover.bmp.bak");
  drops("sections.tmp");
}

// 5. Name matching is exact, not a prefix: a lookalike must not be mistaken for user data,
//    and must not be preserved merely because it shares a stem.
void testLookalikesAreDeleted() {
  drops("progress.binx");
  drops("progress.backup");
  drops("progress");
  drops("annotations.bin.old");   // not one of the recognised temp suffixes
  drops("xannotations.bin");
  drops("progress.bin.tmp.tmp");  // only one temp level is a writer's shape
}

// 6. Degenerate input must be refused rather than acted on: an entry whose name did not fit
//    the caller's buffer reads as empty, and deleting "" would target the directory itself.
void testUnnamedEntryIsUntouched() {
  expect(!book_cache::shouldDelete(nullptr), "a null name must never be marked deletable");
  expect(!book_cache::shouldDelete(""), "an empty name must never be marked deletable");
}

// 7. The temp-suffix probe copies a base name into a fixed buffer; a long entry name must
//    stay inside it (ASan/UBSan catch it if the bound is wrong).
void testLongNamesDoNotOverflow() {
  std::string longBase(120, 'a');
  const std::string withTemp = longBase + ".tmp";
  const std::string longKept = std::string(70, 'p') + "rogress.bin";  // 81 chars, not a kept name
  expect(longBase.size() >= 64, "fixture must exceed the internal base-name buffer");
  drops(withTemp.c_str());
  drops(longKept.c_str());
  const std::string exactKeep = "progress.bin";  // short names still keep
  keeps(exactKeep.c_str());
}

// 8. The two halves of the rule must not contradict each other: anything reported as user
//    data has to survive, whoever asks.
void testPredicateIsCoherent() {
  for (const char* name : {"annotations.bin", "annotations.txt", "bookmarks.bin", "progress.bin",
                           "book.bin", "cover.bmp", "progress.bin.tmp", "book.bin.tmp"}) {
    const bool survives = !book_cache::shouldDelete(name);
    expect(!book_cache::isUserDataName(name) || survives, name);
  }
}

// 9. The names on the card ARE the interface.  Renaming one is not a refactor: every
//    highlight, note, bookmark and reading position already written under the old name is
//    silently orphaned (and would then also become deletable, since the keep set matches by
//    name).  Pinned literally, so a rename has to be a deliberate act that passes through
//    this test with a migration -- not a drive-by edit to a constant.
void testOnDiskNamesArePinned() {
  expect(std::strcmp(book_cache::kAnnotationsFile, "annotations.bin") == 0,
         "annotations.bin is the name readers' highlights already live under");
  expect(std::strcmp(book_cache::kAnnotationsExportFile, "annotations.txt") == 0,
         "annotations.txt is the name AnnotationExport already writes");
  expect(std::strcmp(book_cache::kBookmarksFile, "bookmarks.bin") == 0,
         "bookmarks.bin is the name PDF bookmarks already live under");
  expect(std::strcmp(book_cache::kProgressFile, "progress.bin") == 0,
         "progress.bin is the name every reader's position already lives under");
}

}  // namespace

int main() {
  testUserDataSurvives();
  testRegenerableArtifactsGo();
  testUnknownNameDefaultsToDeleteable();
  testTempSiblingsOfUserDataAreKept();
  testLookalikesAreDeleted();
  testUnnamedEntryIsUntouched();
  testLongNamesDoNotOverflow();
  testPredicateIsCoherent();
  testOnDiskNamesArePinned();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
