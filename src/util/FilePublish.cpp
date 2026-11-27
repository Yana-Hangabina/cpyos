#include "FilePublish.h"

#include <HalStorage.h>
#include <Logging.h>

#include "util/FilePublishRules.h"

namespace {

constexpr size_t kCopyChunk = 1024;

// Byte-for-byte overwrite of `dest` with `source`. Returns true only when every
// byte of the source reached the destination.
bool copyOver(const std::string& source, const std::string& dest, const size_t sourceSize, const char* logModule) {
  uint64_t totalBytes = 0;
  uint64_t freeBytes = 0;
  if (Storage.getSpace(totalBytes, freeBytes) && freeBytes + sourceSize < sourceSize) {
    // The card cannot hold the file; say so before the destination is truncated,
    // because after that a failed copy has cost the user their only copy.
    LOG_ERR("FPUB", "%s: refusing to overwrite, %u bytes needed and %u free", logModule,
            static_cast<unsigned>(sourceSize), static_cast<unsigned>(freeBytes));
    return false;
  }

  HalFile in;
  if (!Storage.openFileForRead("FPUB", source.c_str(), in)) return false;
  HalFile out;
  if (!Storage.openFileForWrite("FPUB", dest.c_str(), out)) return false;

  uint8_t buffer[kCopyChunk];
  size_t written = 0;
  for (;;) {
    const int got = in.read(buffer, sizeof(buffer));
    if (got <= 0) break;
    if (out.write(buffer, static_cast<size_t>(got)) != static_cast<size_t>(got)) {
      LOG_ERR("FPUB", "%s: write stalled at %u of %u bytes", logModule, static_cast<unsigned>(written),
              static_cast<unsigned>(sourceSize));
      out.close();
      in.close();
      return false;
    }
    written += static_cast<size_t>(got);
  }
  out.flush();
  const bool complete = written == sourceSize && out.size() == sourceSize;
  out.close();
  in.close();
  if (!complete) {
    LOG_ERR("FPUB", "%s: copied %u of %u bytes", logModule, static_cast<unsigned>(written),
            static_cast<unsigned>(sourceSize));
  }
  return complete;
}

size_t fileSizeOf(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("FPUB", path.c_str(), file)) return 0;
  return file.size();
}

}  // namespace

namespace file_publish {

Outcome replace(const char* logModule, const std::string& source, const std::string& dest) {
  Attempts attempts;
  attempts.sourcePresent = Storage.exists(source.c_str());
  if (!attempts.sourcePresent) {
    LOG_ERR("FPUB", "%s: nothing to publish at %s", logModule, source.c_str());
    return classify(attempts);
  }
  attempts.destPresent = Storage.exists(dest.c_str());

  if (attempts.destPresent) {
    // FAT rename does not replace, so the old file has to go first. Whether that
    // worked is worth knowing on its own: it is the failure that leaves a stuck
    // directory entry behind.
    attempts.removeOk = Storage.remove(dest.c_str());
    attempts.renameOk = attempts.removeOk && Storage.rename(source.c_str(), dest.c_str());
  } else {
    attempts.removeOk = true;
    attempts.renameOk = Storage.rename(source.c_str(), dest.c_str());
  }

  if (attempts.renameOk) {
    const Outcome outcome = classify(attempts);
    LOG_INF("FPUB", "%s: published %s (%s)", logModule, dest.c_str(), name(outcome));
    return outcome;
  }

  LOG_ERR("FPUB", "%s: rename into %s failed (dest existed=%d, removed=%d); overwriting in place", logModule,
          dest.c_str(), attempts.destPresent ? 1 : 0, attempts.removeOk ? 1 : 0);
  attempts.copyOk = copyOver(source, dest, fileSizeOf(source), logModule);
  if (attempts.copyOk && !Storage.remove(source.c_str())) {
    // The published file is correct; only the scratch copy is left behind. Not a
    // failure, but it must not look like one later, so name it.
    LOG_ERR("FPUB", "%s: published but could not remove the scratch file %s", logModule, source.c_str());
  }

  const Outcome outcome = classify(attempts);
  if (!succeeded(outcome)) {
    LOG_ERR("FPUB", "%s: cannot publish %s over %s (%s)", logModule, source.c_str(), dest.c_str(), name(outcome));
  } else {
    LOG_INF("FPUB", "%s: published %s (%s)", logModule, dest.c_str(), name(outcome));
  }
  return outcome;
}

}  // namespace file_publish
