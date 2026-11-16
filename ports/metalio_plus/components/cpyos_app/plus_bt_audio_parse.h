#pragma once

// BH1098 inquiry replies, extracted from the vendor bluetooth_uart.cc flow so
// the parser can be host-tested without a radio.
//
// The module emits one line per device: "AT+BT:<12 hex><name>". The address is
// always the first 12 characters after the prefix; the rest (possibly empty,
// possibly UTF-8) is the advertised name.

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace plus_bt_audio {

inline bool isHexAddress(const char* text, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

inline bool parseDeviceLine(const char* line, char* address, size_t addressCap, char* name, size_t nameCap) {
  constexpr const char* kPrefix = "AT+BT:";
  if (line == nullptr || address == nullptr || name == nullptr) return false;
  if (std::strncmp(line, kPrefix, 6) != 0) return false;
  const char* payload = line + 6;
  const size_t len = std::strlen(payload);
  if (len < 12 || !isHexAddress(payload, 12)) return false;
  std::snprintf(address, addressCap, "%.12s", payload);
  std::snprintf(name, nameCap, "%s", payload + 12);
  return true;
}

// Status lines the page latches onto; kept here so the spelling is in one
// place (the vendor module prints them verbatim).
inline bool isInquiryComplete(const char* line) {
  return line != nullptr && std::strstr(line, "INQ COMPLETE") != nullptr;
}
inline bool isConnectSuccess(const char* line) {
  return line != nullptr && std::strstr(line, "CONNECT SUCCESS") != nullptr;
}
inline bool isConnectTimeout(const char* line) {
  return line != nullptr && std::strstr(line, "CONNECT TIMEOUT") != nullptr;
}

}  // namespace plus_bt_audio
