#include "CmapStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kCmapDir = "/.crosspoint/cmaps/";
constexpr size_t kMaxCmapBytes = 512u * 1024u;
constexpr size_t kMaxNameLen = 63;

// One-entry cache: a PDF usually needs one or two CMaps at a time, and MuPDF
// parses the bytes immediately after this returns.  The buffer lives in PSRAM
// and is replaced on the next miss, so the renderer never holds a dangling
// pointer.  MuPDF renders on one task; no lock is needed.
uint8_t* g_cache = nullptr;
size_t g_cacheSize = 0;
char g_cacheName[kMaxNameLen + 1] = {};

bool validName(const char* name) {
  if (!name || !*name) return false;
  const size_t len = std::strlen(name);
  if (len > kMaxNameLen) return false;
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.';
    if (!ok) return false;
  }
  // No path traversal; the name is used as a single path component.
  return std::strstr(name, "..") == nullptr && name[0] != '.';
}

}  // namespace

extern "C" const unsigned char* crosspoint_cmap_load(const char* name, size_t* size) {
  if (!size) return nullptr;
  *size = 0;
  if (!validName(name)) return nullptr;

  if (g_cache && std::strcmp(g_cacheName, name) == 0) {
    *size = g_cacheSize;
    return g_cache;
  }

  char path[96];
  std::snprintf(path, sizeof(path), "%s%s", kCmapDir, name);
  HalFile file;
  if (!Storage.openFileForRead("PDF", path, file)) {
    LOG_DBG("PDF", "cmap missing: %s", name);
    return nullptr;
  }
  const size_t bytes = file.fileSize();
  if (bytes == 0 || bytes > kMaxCmapBytes) {
    LOG_ERR("PDF", "cmap has implausible size: %s (%u)", name, static_cast<unsigned>(bytes));
    file.close();
    return nullptr;
  }

  uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) {
    LOG_ERR("PDF", "cmap PSRAM allocation failed: %s (%u)", name, static_cast<unsigned>(bytes));
    file.close();
    return nullptr;
  }
  const int got = file.read(buffer, bytes);
  file.close();
  if (got != static_cast<int>(bytes)) {
    LOG_ERR("PDF", "cmap short read: %s (%d/%u)", name, got, static_cast<unsigned>(bytes));
    heap_caps_free(buffer);
    return nullptr;
  }

  if (g_cache) heap_caps_free(g_cache);
  g_cache = buffer;
  g_cacheSize = bytes;
  std::strncpy(g_cacheName, name, kMaxNameLen);
  g_cacheName[kMaxNameLen] = '\0';
  LOG_INF("PDF", "cmap loaded: %s (%u bytes)", name, static_cast<unsigned>(bytes));
  *size = bytes;
  return buffer;
}
