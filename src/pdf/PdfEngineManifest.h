#pragma once

// Pure, host-testable parser for the PDF engine update manifest.
//
// The manifest is a rolling index: one line per published engine, kept forever.
// A device picks the newest line that matches its own compatibility class, so a
// user who flashes an older firmware can still get the latest engine ever built
// for that contract.  The running app is never asked to accept an image from a
// different class -- it downloads only what it can run.
//
// Format (TAB-separated, trailing newline allowed):
//   PDFENG2
//   url<TAB>size<TAB>sha256hex<TAB>engineAbi<TAB>hostApiMajor<TAB>hostApiMinor<TAB>layoutKeyHex<TAB>engineRev<TAB>buildIdHex
//
//   - url: https:// asset location (the caller applies the gh-proxy shortlist)
//   - size: blob bytes, 512 .. 4 MiB
//   - sha256hex: integrity of the downloaded blob
//   - engineAbi/hostApi/layoutKey: the compatibility class (see PdfModuleApi.h)
//   - engineRev: monotonically increasing build counter; highest wins
//   - buildIdHex: sha256(firmware ELF) for provenance only
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "PdfModuleApi.h"

namespace pdf_engine {

constexpr uint64_t kMinBlobBytes = 512;
constexpr uint64_t kMaxBlobBytes = 4ull * 1024 * 1024;
constexpr size_t kMaxUrlBytes = 512;
constexpr size_t kMaxEntries = 16;
constexpr const char* kManifestHeader = "PDFENG2";

struct Entry {
  std::string url;
  uint64_t size = 0;
  std::array<uint8_t, 32> sha256{};
  uint32_t engineAbi = 0;
  uint16_t hostApiMajor = 0;
  uint16_t hostApiMinor = 0;
  uint32_t layoutKey = 0;
  uint32_t engineRev = 0;
  std::array<uint8_t, 32> buildId{};
};

inline bool parseHexByte(const char* hex, uint8_t& out) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  const int hi = nib(hex[0]);
  const int lo = nib(hex[1]);
  if (hi < 0 || lo < 0) return false;
  out = static_cast<uint8_t>((hi << 4) | lo);
  return true;
}

// 64 hex chars -> 32 bytes.  Rejects any other length or a non-hex char.
inline bool parseHex32(const std::string& hex, std::array<uint8_t, 32>& out) {
  if (hex.size() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    if (!parseHexByte(hex.data() + i * 2, out[i])) return false;
  }
  return true;
}

inline std::string hex32Lower(const std::array<uint8_t, 32>& bytes) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const uint8_t b : bytes) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0F]);
  }
  return out;
}

inline bool parseU32Dec(const std::string& text, uint32_t& out) {
  if (text.empty() || text.size() > 10) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || v > 0xFFFFFFFFull) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

inline bool parseU32HexOrDec(const std::string& text, uint32_t& out) {
  std::string s = text;
  int base = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s = s.substr(2);
    base = 16;
  }
  if (s.empty() || s.size() > 8) return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(s.c_str(), &end, base);
  if (end == nullptr || *end != '\0' || v > 0xFFFFFFFFul) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

inline void trimLine(std::string& s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
  s.erase(0, start);
}

// Parses one TAB-separated line into an Entry.  Returns false on any malformed
// field and never partially fills `out` on failure.
inline bool parseEntry(const std::string& line, Entry& out) {
  std::array<std::string, 9> fields;
  size_t n = 0;
  size_t fstart = 0;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '\t') {
      if (n >= fields.size()) return false;  // too many fields
      fields[n++] = line.substr(fstart, i - fstart);
      fstart = i + 1;
    }
  }
  if (n != fields.size()) return false;

  const std::string& url = fields[0];
  if (url.empty() || url.size() > kMaxUrlBytes) return false;
  if (url.rfind("https://", 0) != 0) return false;  // TLS only; the transport is wolfSSL
  for (const char c : url) {
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) == 0x7F) return false;
  }

  Entry cand;
  cand.url = url;
  char* end = nullptr;
  const unsigned long long size = std::strtoull(fields[1].c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || size < kMinBlobBytes || size > kMaxBlobBytes) return false;
  cand.size = size;
  if (!parseHex32(fields[2], cand.sha256)) return false;

  uint32_t abi = 0;
  if (!parseU32Dec(fields[3], abi)) return false;
  uint32_t major = 0;
  uint32_t minor = 0;
  if (!parseU32Dec(fields[4], major) || major > 0xFFFFu) return false;
  if (!parseU32Dec(fields[5], minor) || minor > 0xFFFFu) return false;
  uint32_t layout = 0;
  if (!parseU32HexOrDec(fields[6], layout)) return false;
  if (!parseU32Dec(fields[7], cand.engineRev)) return false;
  if (!parseHex32(fields[8], cand.buildId)) return false;

  cand.engineAbi = abi;
  cand.hostApiMajor = static_cast<uint16_t>(major);
  cand.hostApiMinor = static_cast<uint16_t>(minor);
  cand.layoutKey = layout;
  out = std::move(cand);
  return true;
}

// Parses the whole manifest.  `count` receives the number of entries; returns
// false if any entry is malformed or the header line is missing, so a corrupted
// index is refused instead of silently hiding the compatible line.
inline bool parseManifest(const std::string& text, std::array<Entry, kMaxEntries>& out, size_t& count) {
  count = 0;
  bool sawHeader = false;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    trimLine(line);
    if (line.empty() || line[0] == '#') continue;
    if (!sawHeader) {
      if (line != kManifestHeader) return false;
      sawHeader = true;
      continue;
    }
    if (count >= out.size()) return false;  // bounded: refuse rather than truncate
    if (!parseEntry(line, out[count])) return false;
    ++count;
  }
  return sawHeader && count > 0;
}

// True when `entry` can run on an app built from this tree.
constexpr bool matchesThisApp(const Entry& entry) {
  return entry.engineAbi == kPdfModuleAbiVersion &&
         entry.hostApiMajor == kPdfModuleHostApiMajor &&
         entry.hostApiMinor <= kPdfModuleHostApiMinor &&
         entry.layoutKey == kPdfModuleLayoutKey;
}

// Picks the newest compatible entry.  Ties keep the earlier line, so the
// publisher controls the order for equal revisions.
inline bool selectForApp(const std::array<Entry, kMaxEntries>& entries, const size_t count, Entry& out) {
  bool found = false;
  for (size_t i = 0; i < count; ++i) {
    if (!matchesThisApp(entries[i])) continue;
    if (!found || entries[i].engineRev > out.engineRev) {
      out = entries[i];
      found = true;
    }
  }
  return found;
}

}  // namespace pdf_engine
