#pragma once

// xiaozhi WebSocket audio framing, extracted from the vendor
// protocols/websocket_protocol.cc so it can be host-tested without a server.
//
//   v1: raw Opus payload, no header.
//   v2: 16-byte big-endian header
//       [0..1] version  [2..3] type  [4..7] reserved
//       [8..11] timestamp ms  [12..15] payload size
//   v3: 4-byte big-endian header
//       [0] type  [1] reserved  [2..3] payload size
//
// type 0 is Opus; type 1 (JSON) is not requested by this client.

#include <cstddef>
#include <cstdint>

namespace plus_voice {

constexpr uint8_t kTypeOpus = 0;

inline uint32_t readBe16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
inline uint32_t readBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void writeBe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v & 0xFF);
}
inline void writeBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t((v >> 16) & 0xFF);
  p[2] = uint8_t((v >> 8) & 0xFF);
  p[3] = uint8_t(v & 0xFF);
}

// Writes the header for the given protocol version. Returns the header size
// (0 for v1 = no header). Returns 0 for v2/v3 when the output buffer is too
// small or the payload cannot be expressed in the header field.
inline size_t buildAudioHeader(uint8_t version, uint8_t type, uint32_t timestampMs, size_t payloadLen,
                               uint8_t* out, size_t cap) {
  if (version == 2) {
    if (out == nullptr || cap < 16 || payloadLen > 0xFFFFFFFFu) return 0;
    writeBe16(out + 0, version);
    writeBe16(out + 2, type);
    writeBe32(out + 4, 0);
    writeBe32(out + 8, timestampMs);
    writeBe32(out + 12, static_cast<uint32_t>(payloadLen));
    return 16;
  }
  if (version == 3) {
    if (out == nullptr || cap < 4 || payloadLen > 0xFFFFu) return 0;
    out[0] = type;
    out[1] = 0;
    writeBe16(out + 2, static_cast<uint16_t>(payloadLen));
    return 4;
  }
  return 0;  // v1: bare payload
}

// Validates the header and points at the Opus payload. v1 has no header.
// Returns false on truncation or a non-Opus binary type.
inline bool parseAudioHeader(uint8_t version, const uint8_t* data, size_t len, const uint8_t** payload,
                             size_t* payloadLen) {
  if (data == nullptr || payload == nullptr || payloadLen == nullptr) return false;
  *payload = data;
  *payloadLen = len;
  if (version == 2) {
    if (len < 16 || readBe16(data + 2) != kTypeOpus) return false;
    const uint32_t declared = readBe32(data + 12);
    *payload = data + 16;
    *payloadLen = len - 16;
    if (declared < *payloadLen) *payloadLen = declared;
    return true;
  }
  if (version == 3) {
    if (len < 4 || data[0] != kTypeOpus) return false;
    const uint32_t declared = readBe16(data + 2);
    *payload = data + 4;
    *payloadLen = len - 4;
    if (declared < *payloadLen) *payloadLen = declared;
    return true;
  }
  return true;
}

}  // namespace plus_voice
