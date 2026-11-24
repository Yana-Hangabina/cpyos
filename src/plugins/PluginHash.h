#pragma once

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <cstddef>
#include <cstdint>
#include <string>

namespace native_plugins {

// SHA-256 of a card file; false on an open/read error.
bool sha256File(const char* path, uint8_t out[32]);
// Lower-case hex SHA-256; empty on error.
std::string sha256FileHex(const char* path);
// Decode a 64-character lower/upper-case hex digest.
bool hexToSha256(const char* hex, uint8_t out[32]);

}  // namespace native_plugins

#endif  // CROSSPOINT_PLUGIN_RUNTIME
