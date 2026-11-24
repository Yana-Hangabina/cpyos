#pragma once

#include <functional>
#include <string>

#include "pdf/PdfEngineManifest.h"
#include "pdf/PdfModuleRuntime.h"

#if defined(CROSSPOINT_PDF_XIP)

namespace pdf_engine {

/// Outcome of the network fetch/download phase.
enum class Failure : uint8_t {
  None,
  NotConfigured,  // no manifest URL (build define or override) -- update unavailable
  NoWifi,         // radio down: refused before DNS/TLS (lwIP udp_new_ip_type)
  Timeout,
  Network,
  Manifest,       // manifest fetch/parse failed and no usable cache
  WrongImage,     // no manifest entry matches this firmware's compatibility class
  Download,
  Verify,         // downloaded size/sha256 mismatch
  SdCard,
  Cancelled,
};

/// Worker lifecycle.  startUpdate() launches a PSRAM-stack task that runs ONLY
/// the network + SD-download + SHA-verify phases (never an internal-flash write).
/// When it reaches Done/Failed the caller (on the UI/internal stack) reads
/// result() and, if Failure::None, calls installFromTemp() to write the
/// pdfmod partition.  That split is mandatory: esp_partition_write on a PSRAM
/// stack trips esp_task_stack_is_sane_cache_disabled() and restart-loops.
enum class State : uint8_t { Idle, Running, Done, Failed };

struct Result {
  Failure failure = Failure::None;
  std::string detail;
  Entry entry;
};

using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

/// Which phase the worker is in, so the hosting UI can put a number on the
/// wait card instead of swimming a duck for two minutes.  The values are
/// monotonic within one run.
enum class Stage : uint8_t { Idle, Manifest, Body, Verify };

struct Progress {
  Stage stage = Stage::Idle;
  size_t downloaded = 0;  // bytes of the engine image received so far
  size_t total = 0;       // manifest size, 0 until the manifest is parsed
};

const char* manifestUrl();
void setManifestUrlOverride(const char* url);
bool isConfigured();

/// The SD temp file the worker writes and installFromTemp() reads.
const char* tempPath();

// ---- worker-driven flow -------------------------------------------------
bool startUpdate();               // returns false if one is already running
Progress progress();              // safe to call from the UI task while it runs
bool updateInProgress();
void cancelUpdate();
State pollUpdate();               // joins the finished worker; updates result()
const Result& result();
/// Call only after pollUpdate()==Done with result().failure==None.  On the UI task.
pdf_module::InstallStatus installFromTemp();

// ---- primitives (also usable by a caller-supplied worker) ---------------
// Resolves the newest manifest entry that matches THIS app's compatibility
// class (engineAbi/hostApi/layoutKey).  Entries for other classes are ignored,
// which is what lets a device on older firmware keep updating.
Failure fetchManifest(Entry& entry, std::string& detail, const bool* cancelFlag);
Failure downloadToTemp(const Entry& entry, ProgressCallback progress, const bool* cancelFlag);

}  // namespace pdf_engine

#endif
