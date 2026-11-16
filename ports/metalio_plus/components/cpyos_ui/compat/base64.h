#pragma once

// Minimal Arduino-core `base64` namespace for the IDF port.
//
// The upstream code only needs encode() (web server, credential obfuscation,
// KOReader basic auth). decode() lives in the callers that use mbedtls, so this
// shim does not pretend to implement it.

#include <cstddef>
#include <cstdint>

#include "WString.h"

namespace base64 {
String encode(const uint8_t *data, size_t len);
String encode(const char *text);
String encode(const String &text);
}  // namespace base64
