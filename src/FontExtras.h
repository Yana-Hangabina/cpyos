#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "FontExtrasFormat.h"

namespace font_extras {

using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

constexpr const char* kStateDir = "/.crosspoint/font_extras";
// Versioned so a file left cross-linked/read-only by an interrupted write can
// be abandoned without blocking future installs. The pre-version file is never
// read again; there was no released firmware that installed font packs.
constexpr const char* kStateFile = "/.crosspoint/font_extras/installed-v2.txt";
constexpr const char* kManifestCache = "/.crosspoint/font_extras/manifest.txt";
constexpr const char* kManifestTemp = "/.crosspoint/font_extras/manifest.tmp";
constexpr const char* kEmojiPath = "/.crosspoint/font_extras/emoji.ttf";

struct InstalledPack {
  char id[kMaxIdBytes + 1] = {};
  char family[kMaxFamilyBytes + 1] = {};
  char file[kMaxFileBytes + 1] = {};
  Kind kind = Kind::Fallback;
  uint64_t size = 0;
  uint8_t sha256[32] = {};
};

// Compile-time manifest URL (CPYOS_FONT_EXTRAS_MANIFEST_URL) or the serial
// override set by setManifestUrlOverride(). Empty when this build has neither.
const char* manifestUrl();
void setManifestUrlOverride(const char* url);

// Why a fetch/install failed, so the UI can name the cause instead of showing
// one generic "failed" (and offer the right action: retry, connect, free space).
enum class Failure : uint8_t {
  None = 0,
  NoWifi,     // station not connected
  Timeout,    // connect/handshake deadline hit
  Network,    // DNS/TCP/TLS/transport failed
  HttpStatus, // non-2xx or rejected credentials
  Manifest,   // manifest did not parse/validate
  Download,   // pack transfer failed
  Verify,     // size/SHA-256/sfnt mismatch
  SdCard,     // SD write/rename failed (not 'Storage': HalStorage.h defines that macro)
};

// Fetches and parses the manifest. Falls back to the cached copy when the
// network fails, so an installed device can still list/uninstall packs offline.
bool fetchManifest(std::vector<Pack>& packs, bool& fromCache, std::string& error,
                   const ProgressCallback& progress = nullptr, const bool* cancelFlag = nullptr,
                   Failure* failure = nullptr);
// Parses only the last saved manifest (no network); used to populate the list
// before the user asks to check for updates.
bool loadCachedManifest(std::vector<Pack>& packs, std::string& error);

bool loadInstalled(std::vector<InstalledPack>& out);
bool isInstalled(const Pack& pack);
// Families of installed fallback packs whose file still exists. The caller
// loads each one as a renderer supplement (at most kMaxSupplementFamilies).
bool installedFallbackFamilies(std::vector<std::string>& out);
bool emojiInstalled();

// Downloads one pack to the SD card, verifies size/SHA-256/sfnt and updates
// the installed state. A failed attempt never replaces an existing file.
bool downloadAndInstall(const Pack& pack, const ProgressCallback& progress, const bool* cancelFlag,
                        std::string& error, Failure* failure = nullptr);
bool removePack(const Pack& pack, std::string& error);
// Serial-test helper: install a font already on the SD card, without network.
// The public flow uses downloadAndInstall(); this exists so the supplement
// loader can be exercised on a device with no Wi-Fi.
bool installLocalFile(const char* id, const char* kind, const char* family, const char* sourcePath,
                      std::string& error);
bool removeById(const char* id, std::string& error);

std::string packPath(const Pack& pack);
std::string formatSize(uint64_t bytes);

// Serial diagnostics: fetch the manifest, install one pack by id, print status.
bool debugCheck(std::vector<Pack>& packs, bool& fromCache, std::string& error);
bool debugInstallById(const char* id, std::string& error);
void debugPrintStatus();

// Number of fallback families the loader will actually use (keeps the boot
// cost and PSRAM footprint bounded).
constexpr size_t kMaxSupplementFamilies = 3;

}  // namespace font_extras
