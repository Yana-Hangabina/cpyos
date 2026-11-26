#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "booksource/PsramAllocator.h"

// CloudZao third-party firmware resource API for the Metalio E-INK4 Plus.
//
// The endpoints and their wire contract are documented at
// https://burner.cloudzao.com/zh/ota-docs (protocol 1.1, 2026-11-18):
//   POST   /xiaozhi/api/firmware-resources/ota
//   POST   /xiaozhi/api/firmware-resources/push-resources/next
//   DELETE /xiaozhi/api/firmware-resources/push-resources
//   POST   /xiaozhi/api/firmware-ota/check  (plus_ota.cpp)
// Device calls carry no secret: integrationId/firmwareId are public identifiers
// and the partner API issues per-device tokens after mini-program pairing. There
// is no X-Resource-Key, X-OTA-Key or access-token file any more.

namespace plus_resources {

enum class Type : uint8_t { Unknown = 0, Book, Badge, Font };
enum class ItemState : uint8_t { Pending = 0, Unsupported, SavedNotAcknowledged };

using PsramString = jjwxc::PsramString;

struct Item {
  Type type = Type::Unknown;
  ItemState state = ItemState::Pending;
  PsramString task_id;
  PsramString resource_id;
  PsramString name;          // filename from the service
  PsramString display_name;  // resourceName when present
  PsramString content_type;
  PsramString community_type;  // wallpaper / badge / book_cover / reading_background
  PsramString download_url;    // signed URL: never log it
  PsramString format;
  PsramString font_style;
  PsramString sha256;
  PsramString local_path;
  uint64_t file_size = 0;
  int page_count = 0;
  int font_weight = 0;
  bool supported = false;
  // Cheap card-side state for the list ("downloaded" tag) and deletion. The
  // authoritative check before reusing a file is still the SHA-256 in
  // downloadAndAck(); this one only compares existence and size.
  bool downloaded = false;
};

using Items = std::vector<Item, jjwxc::PsramAllocator<Item>>;

struct BindResult {
  bool ok = false;
  bool already_bound = false;
  int http = 0;
  std::string code;     // six digits, string on purpose (leading zero)
  std::string message;  // activation.message, may contain a URL/newlines
  std::string bind_url; // first http(s):// line of message; never logged
  std::string error;    // human readable; never contains the key
};

struct FetchResult {
  bool ok = false;
  int http = 0;
  int business_code = 0;
  std::string error;
  Items items;
};

struct TransferResult {
  bool ok = false;
  bool acked = false;
  bool unsupported = false;
  std::string error;
};

using ProgressFn = void (*)(size_t done, size_t total, void* ctx);

// True when a key with the mpr_ prefix is available. Reading the key never
// prints its value; only the source and length are logged.
bool configured();
const char* keySource();
// Internal: the same secret used as X-Resource-Key (never log or display it).
const char* keyValue();

const char* deviceId();
const char* currentVersion();

// Drop the stored pairing session (deviceCode/activation code) without touching
// tokens. Used by the guide's "retry": a code that the mini-program already
// rejected can otherwise be shown forever because the device keeps polling it.
void discardPairing();

// Blocking; call from a worker task. The caller must ensure the STA link.
BindResult bindDevice();
// Blocking; call from a worker task.
FetchResult fetchResources();
// Downloads to the item's validated local path, verifies size + SHA-256,
// atomically installs the file, then acknowledges the task. A file that is
// already present and verified is acked without downloading again.
TransferResult downloadAndAck(Item& item, ProgressFn progress, void* ctx, const bool* cancel_flag);

std::string formatBytes(uint64_t bytes);
const char* typeName(Type type);  // "BOOK" / "BADGE" / "FONT"
std::string extensionOf(const std::string& name);

// True when the item's destination file already exists with the advertised
// size (or an unknown size). Existence only: the download path still verifies
// the SHA-256 before it reuses a file.
bool isDownloaded(const Item& item);

// Local record of resources this device installed, so the page can still show
// (and delete) them after the server has acknowledged the delivery and dropped
// it from the pending list. Keyed by destination path.
struct InstalledResource {
  Type type = Type::Unknown;
  PsramString path;
  PsramString sha256;
  uint64_t file_size = 0;
};
using InstalledList = std::vector<InstalledResource, jjwxc::PsramAllocator<InstalledResource>>;

// Reads the record and drops entries whose file has disappeared (the record is
// rewritten when that happens). Always rewrites nothing else.
void listInstalled(InstalledList& out);
// Adds or replaces this item's entry. Call after the file is on the card.
bool recordInstalled(const Item& item);
// Removes one path from the record. Call after the file was deleted.
bool forgetInstalled(const char* path);

}  // namespace plus_resources
