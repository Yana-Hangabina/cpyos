#include "AnnotationExport.h"

#include <BookCachePrune.h>
#include <HalStorage.h>
#include <Logging.h>

#include <ctime>

namespace annotation_export {

Result write(const char* logModule, const std::string& cachePath, const std::string& bookTitle,
             const std::vector<annotations::Annotation>& list, std::string& outPath) {
  outPath.clear();
  if (cachePath.empty()) return Result::NoBook;

  // Human-readable date for the export header; an unset clock simply omits it rather than
  // printing an epoch number.
  char stamp[16] = "";
  const time_t now = time(nullptr);
  if (now > 0) {
    // localtime_r, not localtime: newlib's localtime() needs a 2.5 KB thread-local buffer,
    // and on the RISC-V port that TLS block is placed in every task stack (the 1 KB IPC task
    // overflows and corrupts the heap).
    tm local{};
    if (localtime_r(&now, &local) != nullptr) strftime(stamp, sizeof(stamp), "%Y-%m-%d", &local);
  }

  const std::string body = annotations::formatExport(bookTitle, stamp, list);
  const std::string path = cachePath + "/" + book_cache::kAnnotationsExportFile;
  outPath = path;

  HalFile file;
  if (!Storage.openFileForWrite(logModule, path, file)) {
    LOG_ERR(logModule, "annotations: cannot open %s for export", path.c_str());
    return Result::CannotOpen;
  }
  const size_t written = file.write(body.data(), body.size());
  file.close();
  if (written != body.size()) {
    LOG_ERR(logModule, "annotations: short export %u/%u bytes -> %s", static_cast<unsigned>(written),
            static_cast<unsigned>(body.size()), path.c_str());
    return Result::ShortWrite;
  }
  LOG_DBG(logModule, "annotations exported: %u bytes -> %s", static_cast<unsigned>(written), path.c_str());
  return Result::Written;
}

}  // namespace annotation_export
