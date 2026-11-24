#pragma once

// Turning a reader's annotations into the export file, shared by the EPUB and PDF readers.
//
// Both readers keep the same `annotations::Annotation` list in the same per-book cache
// directory, so the export is the same operation with a different title -- and two copies of
// it would drift exactly where it hurts: the file name (which is on the keep list of a cache
// clear, see BookCachePrune.h) and the date stamp (which has a portability trap in it).
#include <string>
#include <vector>

#include "Epub/AnnotationStore.h"

namespace annotation_export {

enum class Result {
  Written,
  NoBook,      // no cache directory: nothing is open, or it failed to open
  CannotOpen,  // the path exists but is not writable (card full, read-only, corrupt entry)
  ShortWrite,  // the disk accepted fewer bytes than the export produced
};

// Writes `<cachePath>/annotations.txt` for `bookTitle`.  `outPath` always receives the
// intended path so the caller can report where it looked, and is left empty for NoBook.
Result write(const char* logModule, const std::string& cachePath, const std::string& bookTitle,
             const std::vector<annotations::Annotation>& list, std::string& outPath);

}  // namespace annotation_export
