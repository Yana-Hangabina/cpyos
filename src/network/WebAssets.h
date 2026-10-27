#pragma once

// Access to the six gzip'd web assets the file-transfer server serves.
//
// Two implementations, selected at compile time:
//
//   embedded (default: C3, simulator, every other target)
//     WebAssets.cpp includes the `*.generated.h` arrays build_html.py writes and
//     returns pointers into flash. Zero runtime cost, nothing to flash besides app0.
//
//   CROSSPOINT_WEB_ASSETS_FROM_PARTITION (Read Pico only)
//     app0 has no room for ~154 KB of pages, so they live in build/web_pack.dat
//     in the data/spiffs partition (offset owned by WebPackFormat.h). begin()
//     reads and validates the pack into PSRAM; the handlers ask for a view.
//     A missing, outdated or corrupt pack leaves the server running: pages fall
//     back to recoveryPage() and /api/status, /upload and WebDAV keep working.
//     That is deliberate -- a web page is how the pack gets reflashed and how
//     the SD card is reached, so it must never take the server down with it.
//
// The two branches share one interface so CrossPointWebServer.cpp has no
// conditionals of its own. Do not gate this on BOARD_HAS_PSRAM: other S3 boards
// have PSRAM but do not write a data partition, and must keep the arrays.

#include <cstddef>
#include <cstdint>

namespace web_assets {

enum class Asset : uint8_t {
  FilesPage,
  SettingsPage,
  JsZip,
  PluginRunner,
};

// A gzip'd asset in flash or PSRAM. `data == nullptr` means "unavailable": the
// pack is missing/rejected, or this one asset failed its CRC.
struct View {
  const char* data = nullptr;
  size_t size = 0;
  explicit operator bool() const { return data != nullptr; }
};

// `chinese` selects the localized page and is ignored for JsZip/PluginRunner.
View view(Asset asset, bool chinese);

// Loads and validates the pack. Partition builds only; the embedded build has
// nothing to do. Returns true when the requested pages are available.
// Idempotent. Must run on a task whose stack is in internal RAM (it reads the
// internal flash, which disables the caches).
bool begin();

// Releases the pack buffer. Called from CrossPointWebServer::stop().
void end();

bool ready();

// nullptr when ready; otherwise a human-readable reason for the log.
const char* statusText();

// Small uncompressed page shown in place of /files, /settings and /plugins-run
// when their asset is unavailable. Never null.
const char* recoveryPage(size_t& size);

}  // namespace web_assets
