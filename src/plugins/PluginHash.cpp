#include "PluginHash.h"

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <mbedtls/sha256.h>

#include <cstring>

#include "HalStorage.h"

namespace native_plugins {
namespace {
constexpr size_t kHashBufferBytes = 512;
}

bool sha256File(const char* path, uint8_t out[32]) {
  HalFile file;
  if (!Storage.openFileForRead("PLUG", path, file)) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buffer[kHashBufferBytes];
  bool ok = true;
  for (;;) {
    const int read = file.read(buffer, sizeof(buffer));
    if (read < 0) {
      ok = false;
      break;
    }
    if (read == 0) break;
    mbedtls_sha256_update(&ctx, buffer, static_cast<size_t>(read));
  }
  if (ok) mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
  return ok;
}

std::string sha256FileHex(const char* path) {
  uint8_t digest[32] = {};
  if (!sha256File(path, digest)) return {};
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.resize(64);
  for (size_t i = 0; i < sizeof(digest); ++i) {
    out[2 * i] = kHex[digest[i] >> 4];
    out[2 * i + 1] = kHex[digest[i] & 0x0F];
  }
  return out;
}

bool hexToSha256(const char* hex, uint8_t out[32]) {
  if (hex == nullptr || std::strlen(hex) != 64) return false;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < 32; ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

}  // namespace native_plugins

#endif  // CROSSPOINT_PLUGIN_RUNTIME
