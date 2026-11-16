// Pure protocol helpers for the WebSocket upload server (Calibre plugin path).
//
// Documented in docs/webserver-endpoints.md: ws://<ip>:81/ then
//   START:<filename>:<size>:<path> -> READY -> binary chunks
//   -> PROGRESS:<received>:<total> (every 64 KiB) -> DONE.
// Kept free of ESP-IDF includes so the parsing rules have host tests.
#pragma once

#include <cstdint>
#include <string>

namespace ws_upload {

inline constexpr uint64_t kMaxBytes = 512ull * 1024ull * 1024ull;

struct Start {
  std::string filename;
  uint64_t size = 0;
  std::string path = "/";
};

// A filename must be a single path component: no separators, no "..", no
// leading dot (dot directories are private), printable only.
inline bool safeFilename(const std::string& name) {
  if (name.empty() || name.size() > 200) return false;
  if (name == "." || name == "..") return false;
  if (name.front() == '.') return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7f) return false;
    if (c == '/' || c == '\\') return false;
  }
  return true;
}

inline bool parseStart(const std::string& message, Start& out) {
  constexpr const char* kPrefix = "START:";
  constexpr size_t kPrefixLen = 6;
  if (message.size() <= kPrefixLen || message.compare(0, kPrefixLen, kPrefix) != 0) return false;
  const size_t first = message.find(':', kPrefixLen);
  if (first == std::string::npos) return false;
  const size_t second = message.find(':', first + 1);
  if (second == std::string::npos) return false;

  out.filename = message.substr(kPrefixLen, first - kPrefixLen);
  if (!safeFilename(out.filename)) return false;

  const std::string sizeText = message.substr(first + 1, second - first - 1);
  if (sizeText.empty() || sizeText.size() > 20) return false;
  uint64_t size = 0;
  for (char c : sizeText) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (size > (kMaxBytes - digit) / 10) return false;  // > cap or overflow
    size = size * 10 + digit;
  }
  out.size = size;

  out.path = message.substr(second + 1);
  if (out.path.empty()) out.path = "/";
  if (out.path.front() != '/') return false;
  if (out.path.size() > 512) return false;
  return true;
}

}  // namespace ws_upload
