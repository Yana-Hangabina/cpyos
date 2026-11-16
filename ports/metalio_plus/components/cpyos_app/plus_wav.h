#pragma once

// Canonical 44-byte PCM WAV header used by the voice memo. Extracted so the
// exact byte layout can be host-tested: a malformed header makes a recording
// silently unplayable on every other device.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace plus_wav {

constexpr size_t kHeaderBytes = 44;

inline void put16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void put32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// 16-bit mono little-endian PCM at `rate`. `h` must hold kHeaderBytes.
inline void writeHeader(uint8_t* h, const uint32_t dataBytes, const uint32_t rate) {
  std::memcpy(h, "RIFF", 4);
  put32(h + 4, 36 + dataBytes);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);        // fmt chunk size
  put16(h + 20, 1);         // PCM
  put16(h + 22, 1);         // mono
  put32(h + 24, rate);
  put32(h + 28, rate * 2);  // byte rate
  put16(h + 32, 2);         // block align
  put16(h + 34, 16);        // bits per sample
  std::memcpy(h + 36, "data", 4);
  put32(h + 40, dataBytes);
}

}  // namespace plus_wav
