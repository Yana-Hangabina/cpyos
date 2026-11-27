// Host regression for book_cache::pruneRegenerable() -- the sweep itself, against an
// in-memory filesystem (stubs/HalStorage.h).
//
// The pure rule has its own suite (BookCachePruneTest.cpp). This one pins the execution,
// which is where the remaining damage would live: dropping the directory instead of its
// regenerable contents, failing to recurse into sections/, reporting success after a
// removal failed, or leaving the directory absent so a later setupCacheDir() has to
// re-create it.
//
// What this suite CANNOT prove: whether SdFat's real openNextFile() tolerates removing the
// entry that was just opened. That is taken from SDCardManager::removeDir, which performs
// exactly this sequence on this filesystem, and confirmed on hardware by CMD:PRUNE (see
// run_book_cache_prune_test.sh for the expected serial line).
#include <BookCachePrune.h>

#include <HalStorage.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
int checks = 0;
const char* g_model = "index-positioned";
fakefs::Iterator g_mode = fakefs::Iterator::IndexPositioned;

// fakefs::reset() clears the iterator knob along with the tree, so every case has to
// re-apply the model under test.  Getting this wrong made the whole second pass of the
// suite silently re-run the FIRST model, which is exactly the kind of green that proves
// nothing: the assertions all held, and the model they were labelled with never executed.
void resetFs() {
  fakefs::reset();
  fakefs::iterator = g_mode;
}

void expect(const bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL [%s iterator]: %s\n", g_model, what);
  }
}

constexpr const char* kCache = "/.crosspoint/epub_1234";

const char* const kUserData[] = {"annotations.bin", "annotations.txt", "bookmarks.bin", "progress.bin"};
const char* const kDerived[] = {"meta.bin", "book.bin", "cover.bmp", "thumb_200.bmp", "page_11.bmp",
                                "reflow.bin", "txt-cache-version"};

void seedTypicalCache() {
  resetFs();
  fakefs::addDir(kCache);
  for (const char* name : kUserData) fakefs::addFile(std::string(kCache) + "/" + name);
  for (const char* name : kDerived) fakefs::addFile(std::string(kCache) + "/" + name);
  fakefs::addFile(std::string(kCache) + "/progress.bin.tmp");
  // Two nesting levels: a real EPUB cache holds sections/*.bin and html/*.html.
  fakefs::addFile(std::string(kCache) + "/sections/section_0.bin");
  fakefs::addFile(std::string(kCache) + "/sections/section_1.bin");
  fakefs::addFile(std::string(kCache) + "/html/0.html");
}

bool exists(const std::string& path) { return fakefs::nodes.count(path) != 0; }

// 1. The point of the whole change.
void testUserDataSurvivesAndDerivedVanishes() {
  seedTypicalCache();
  unsigned deleted = 0;
  unsigned kept = 0;
  expect(book_cache::pruneRegenerable(kCache, deleted, kept), "prune reports success");
  for (const char* name : kUserData) {
    expect(exists(std::string(kCache) + "/" + name), name);
  }
  expect(exists(std::string(kCache) + "/progress.bin.tmp"), "a temp sibling of progress.bin survives");
  for (const char* name : kDerived) {
    expect(!exists(std::string(kCache) + "/" + name), name);
  }
  expect(!exists(std::string(kCache) + "/sections/section_0.bin"), "sections/ must be removed recursively");
  expect(!exists(std::string(kCache) + "/sections/section_1.bin"), "sections/ must be removed recursively");
  expect(!exists(std::string(kCache) + "/html/0.html"), "html/ must be removed with the cache");
  expect(!exists(std::string(kCache) + "/sections"), "the sections/ node itself must go");
}

// 2. The directory stays: every reader's setupCacheDir() is mkdir-if-absent, but keeping the
//    node is what makes the preserved files addressable without a second write path.
void testDirectoryItselfIsKept() {
  seedTypicalCache();
  expect(book_cache::pruneRegenerable(kCache), "prune reports success");
  expect(exists(kCache), "the cache directory must not be removed");
  expect(fakefs::removeDirCalls.end() == std::find(fakefs::removeDirCalls.begin(), fakefs::removeDirCalls.end(),
                                                   std::string(kCache)),
         "the sweep must never rmtree the cache directory itself");
}

// 3. Counters feed the settings page's report and the device log line.
void testCounts() {
  seedTypicalCache();
  unsigned deleted = 0;
  unsigned kept = 0;
  expect(book_cache::pruneRegenerable(kCache, deleted, kept), "prune reports success");
  expect(kept == 5, "four user files plus one temp sibling are kept");
  expect(deleted == 9, "seven derived blobs plus sections/ plus html/ are deleted");
}

// 4. A cache that was never built is not a failure -- every caller treats "nothing cached"
//    as success, and a false here would light up the settings page's failure state.
void testMissingDirectorySucceeds() {
  resetFs();
  unsigned deleted = 7;
  unsigned kept = 7;
  expect(book_cache::pruneRegenerable("/.crosspoint/epub_9999", deleted, kept), "a missing cache is success");
  expect(deleted == 0 && kept == 0, "a missing cache reports no work done");
  expect(!exists("/.crosspoint/epub_9999"), "a missing cache must not be created by the sweep");
  expect(fakefs::removeCalls.empty() && fakefs::removeDirCalls.empty(), "nothing is touched");
}

// 5. An empty directory is a legitimate state (cleared earlier, book re-opened since).
void testEmptyDirectorySucceeds() {
  resetFs();
  fakefs::addDir(kCache);
  expect(book_cache::pruneRegenerable(kCache), "an empty cache is success");
  expect(exists(kCache), "still there");
}

// 6a. A transient removal failure is reported but does not abandon the directory: the sweep
// retries on a later pass, so the user's "clear cache" still finishes.
void testTransientFailureIsRetried() {
  seedTypicalCache();
  fakefs::failRemovePath = std::string(kCache) + "/book.bin";
  expect(!book_cache::pruneRegenerable(kCache), "a failed removal is still reported");
  expect(!exists(std::string(kCache) + "/book.bin"), "the retried pass removed it");
  expect(!exists(std::string(kCache) + "/meta.bin"), "the sweep continued past the failure");
  expect(!exists(std::string(kCache) + "/page_11.bmp"), "the sweep finished the directory");
  expect(exists(std::string(kCache) + "/annotations.bin"), "user data is untouched by a partial failure");
}

// 6b. An entry that can never be removed must not spin the sweep, and must not be reported
//    as a clean clear -- the settings page shows success/failure, and the corrupt blob the
//    user was trying to get rid of is still there.
void testStuckEntryIsReportedAndTerminates() {
  seedTypicalCache();
  fakefs::failRemovePath = std::string(kCache) + "/book.bin";
  fakefs::stickyRemoveFailure = true;
  expect(!book_cache::pruneRegenerable(kCache), "a permanently stuck entry is reported as failure");
  expect(exists(std::string(kCache) + "/book.bin"), "the stuck entry is still there");
  expect(!exists(std::string(kCache) + "/sections/section_0.bin"), "everything else was still cleared");
  expect(exists(std::string(kCache) + "/progress.bin"), "user data survives a stuck entry");
}

// 7. A file where a directory was expected must not be silently reported as cleared: it would
//    hide a corrupt layout that the reader keeps tripping over.
void testFileInsteadOfDirectoryIsAnError() {
  resetFs();
  fakefs::addFile(kCache);
  expect(!book_cache::pruneRegenerable(kCache), "a non-directory cache path is an error");
  expect(exists(kCache), "and it is left alone rather than deleted");
}

}  // namespace

void runCases() {
  testUserDataSurvivesAndDerivedVanishes();
  testDirectoryItselfIsKept();
  testCounts();
  testMissingDirectorySucceeds();
  testEmptyDirectorySucceeds();
  testTransientFailureIsRetried();
  testStuckEntryIsReportedAndTerminates();
  testFileInsteadOfDirectoryIsAnError();
}

int main() {
  // The sweep must converge whichever way the filesystem's directory cursor reacts to a
  // removal during enumeration -- see runPass() in BookCachePrune.cpp.
  g_model = "index-positioned";
  g_mode = fakefs::Iterator::IndexPositioned;
  runCases();
  g_model = "entry-positioned";
  g_mode = fakefs::Iterator::EntryPositioned;
  runCases();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
