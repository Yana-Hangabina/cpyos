#pragma once

#include <cstdint>

#include "PdfModuleApi.h"

namespace pdf_module {
enum class Refusal : uint8_t {
  None = 0,
  NoPartition,   // no engine image installed
  BadHeader,     // not a pdfmod image, or geometry/CRC rejected
  Incompatible,  // imageOK, but engineAbi/hostApi/layoutKey do not match this app
  MapFailed,     // the MMU window could not be reserved at the linked address
  NoPsram,       // no PSRAM heap; the XIP engine refuses to run (power-cycle/init required)
};

/// Outcome of a device-side engine install/check.  The body of these functions
/// is only compiled into the XIP (`readpico_pdf`) firmware; the enum is always
/// available so callers can name the states.
/// Which remedy a refusal actually calls for.
///
/// A download fixes an engine that is absent, unreadable, or built for another
/// compatibility class.  `MapFailed`/`NoPsram` are this boot's memory state: the
/// image on the card is fine, and sending the reader to fetch another copy of it
/// is worse than asking them to restart.  Lives here (and is pinned by
/// test/pdf_engine_remedy) because the reader, the settings row and the update
/// page must agree on that split -- a wrong mapping silently strands a user in a
/// download loop.
constexpr bool refusalIsDownloadable(const Refusal refusal) {
  return refusal == Refusal::Incompatible || refusal == Refusal::NoPartition || refusal == Refusal::BadHeader;
}

/// The reader stores `Refusal + 1` in one byte so that 0 means "no problem".
constexpr uint8_t encodeRefusal(const Refusal refusal) {
  return static_cast<uint8_t>(refusal) + 1;
}

constexpr Refusal decodeRefusal(const uint8_t stored) {
  return static_cast<Refusal>(stored == 0 ? 0 : stored - 1);
}

enum class InstallStatus : uint8_t {
  Ok,        // image is valid and built for the running app
  NotFound,  // the SD file is missing or empty
  BadHeader, // not a pdfmod image (magic/version/abi/size wrong)
  Mismatch,  // valid image, but bound to a different firmware build
  IoError,   // flash erase/write or partition lookup failed
  Busy,      // a PdfDocument still holds the engine; unmapping would crash it
};

/// Maps the `pdfmod` partition at fixed instruction/data virtual addresses and
/// returns the engine operation table.  Returns false when the partition is
/// missing, the header is invalid, or the module was built for another app.
bool ensureLoaded();
void unload();

/// Request that the engine be unmapped when convenient.  Unmapping external
/// memory makes IDF flush/invalidate caches, which corrupts a task whose stack
/// lives in PSRAM (the reader's onExit runs on exactly such a task), so callers
/// on a PSRAM stack schedule it and the main loop (internal stack) flushes.
void scheduleUnload();
/// Returns true when this call actually unmapped.  Safe to call every loop; it
/// does nothing unless a request is pending, no document is open, and the
/// caller reports the reader is gone (`readerIdle`) -- a reader that is being
/// created would otherwise race this unmap with its own ensureLoaded().
bool flushScheduledUnload(bool readerIdle);

const PdfModuleOps* ops();

/// True while the engine image is mapped into the external-memory window.
/// Light sleep that powers the mapped storage down would make the next return
/// into engine code fatal, so sleep paths consult this instead of guessing.
bool isLoaded();

/// Reports what an SD engine image at `path` is without changing the partition,
/// so a mismatch can be explained before the good engine is erased.
InstallStatus checkSdImage(const char* path);

/// Replace the pdfmod partition from an SD file.  Refuses (Busy) before
/// erasing while any PdfDocument still holds a handle, and refuses (Mismatch)
/// before erasing when the image was not built for the running app.  Must be
/// called on the internal-stack UI task and not from module code.
InstallStatus installFromSdFile(const char* path);

/// Engine-handle lifetime, counted by PdfDocument on every handle transition.
/// The install path asks instead of trusting its callers to remember: an
/// unmapped engine under a live MuPDF document is a crash, not an error code.
void noteDocumentOpened();
void noteDocumentClosed();
uint32_t openDocuments();
Refusal lastRefusal();

/// Why the last ensureLoaded() failed (None when it succeeded). The reader uses
/// this to tell the user the engine needs updating instead of silently exiting.

/// The SD path the Settings maintenance row installs from when the user has not
/// chosen another file.
const char* defaultSdImagePath();

/// The host service table handed to the loaded engine (app-side implementation
/// lives in PdfModuleHostServices.cpp).  Both loaders use this one table.
const PdfModuleHost* hostTable();

/// Copies the running app image's build id (the SHA-256 esptool stores at offset
/// 0xb0) into `out`.  Provenance only: compatibility is decided by
/// pdfModuleCompatible(), never by this digest.  Returns false when unreadable.
bool runningBuildId(uint8_t out[32]);

/// The ABI the loaded engine reported through its ops table, or 0 when no engine is loaded.
uint32_t loadedAbi();

/// The address esp_mmu_map() chose for the code window (0 when no attempt ran).
/// Diagnostic only: when it differs from kPdfModuleCodeVaddr that constant must
/// be updated to the reported value and both images rebuilt.
uint32_t debugMappedCodeAddr();

/// Read-only boot check of the installed engine header: magic/version/ABI/host
/// API/layout key.  It maps and executes nothing, so it is safe before the UI
/// exists and tells whether an open would be refused as incompatible (the case
/// an update fixes) without waiting for a user to open a document.
void debugLogImageHeader();

}  // namespace pdf_module
