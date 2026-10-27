#include "JjwxcCrypto.h"

#include <stdlib.h>
#include <string.h>

namespace jjwxc {
namespace {

// ------------------------------------------------------------------ MD5 ----
struct Md5Ctx {
  uint32_t state[4];
  uint64_t bits;
  uint8_t buf[64];
  size_t bufLen;
};

constexpr uint32_t kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                             5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                             4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                             6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

constexpr uint32_t kK[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};

inline uint32_t rotl(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }

void md5Transform(uint32_t state[4], const uint8_t block[64]) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = static_cast<uint32_t>(block[i * 4]) | (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    const uint32_t tmp = d;
    d = c;
    c = b;
    b = b + rotl(a + f + kK[i] + m[g], kS[i]);
    a = tmp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

void md5Raw(const uint8_t* data, size_t len, uint8_t out[16]) {
  Md5Ctx ctx{};
  ctx.state[0] = 0x67452301u;
  ctx.state[1] = 0xefcdab89u;
  ctx.state[2] = 0x98badcfeu;
  ctx.state[3] = 0x10325476u;
  ctx.bits = static_cast<uint64_t>(len) * 8;

  while (len >= 64) {
    md5Transform(ctx.state, data);
    data += 64;
    len -= 64;
  }
  uint8_t tail[128];
  memcpy(tail, data, len);
  tail[len++] = 0x80;
  const size_t padTo = (len <= 56) ? 56 : 120;
  while (len < padTo) tail[len++] = 0;
  const uint64_t bits = ctx.bits;
  for (int i = 0; i < 8; ++i) tail[len++] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);
  for (size_t off = 0; off < len; off += 64) md5Transform(ctx.state, tail + off);

  for (int i = 0; i < 4; ++i) {
    out[i * 4] = static_cast<uint8_t>(ctx.state[i] & 0xff);
    out[i * 4 + 1] = static_cast<uint8_t>((ctx.state[i] >> 8) & 0xff);
    out[i * 4 + 2] = static_cast<uint8_t>((ctx.state[i] >> 16) & 0xff);
    out[i * 4 + 3] = static_cast<uint8_t>((ctx.state[i] >> 24) & 0xff);
  }
}

constexpr char kHexDigits[] = "0123456789abcdef";

// ------------------------------------------------------------------ DES ----
// Compact table-driven DES. Key schedule is expanded once per operation; the
// per-chapter cost is negligible next to network latency.

// DES tables extracted verbatim from pyDes (reference implementation) to avoid
// transcription errors. Validated against NIST KAT and pycryptodome by
// src/JjwxcCryptoTest.cpp.

constexpr uint8_t kPc1[56] = {
    56, 48, 40, 32, 24, 16, 8, 0, 57, 49, 41, 33, 25, 17,
    9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35,
    62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21,
    13, 5, 60, 52, 44, 36, 28, 20, 12, 4, 27, 19, 11, 3
};

constexpr uint8_t kPc2[48] = {
    13, 16, 10, 23, 0, 4, 2, 27, 14, 5, 20, 9, 22, 18, 11, 3,
    25, 7, 15, 6, 26, 19, 12, 1, 40, 51, 30, 36, 46, 54, 29, 39,
    50, 44, 32, 47, 43, 48, 38, 55, 33, 52, 45, 41, 49, 35, 28, 31
};

constexpr uint8_t kShifts[16] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

constexpr uint8_t kIp[64] = {
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
    56, 48, 40, 32, 24, 16, 8, 0, 58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6
};

constexpr uint8_t kFp[64] = {
    39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28,
    35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26,
    33, 1, 41, 9, 49, 17, 57, 25, 32, 0, 40, 8, 48, 16, 56, 24
};

constexpr uint8_t kE[48] = {
    31, 0, 1, 2, 3, 4, 3, 4, 5, 6, 7, 8, 7, 8, 9, 10,
    11, 12, 11, 12, 13, 14, 15, 16, 15, 16, 17, 18, 19, 20, 19, 20,
    21, 22, 23, 24, 23, 24, 25, 26, 27, 28, 27, 28, 29, 30, 31, 0
};

constexpr uint8_t kP[32] = {
    15, 6, 19, 20, 28, 11, 27, 16, 0, 14, 22, 25, 4, 17, 30, 9,
    1, 7, 23, 13, 31, 26, 2, 8, 18, 12, 29, 5, 21, 10, 3, 24
};

constexpr uint8_t kSbox[8][64] = {
    {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8, 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
    {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5, 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1, 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
    {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9, 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
    {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6, 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
    {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8, 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
    {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6, 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
    {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2, 7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11},
};

// Bit index is 0-based, MSB-first within each byte (pyDes convention).
inline bool bitAt(const uint8_t* data, size_t bitLen, uint8_t pos) {
  const size_t idx = pos;
  if (idx >= bitLen) return false;
  return (data[idx >> 3] >> (7 - (idx & 7))) & 1u;
}

struct DesKeySchedule {
  uint8_t round[16][6];  // 48 bits per round
};

void desKeySchedule(const uint8_t key[8], DesKeySchedule& ks, bool decrypt) {
  uint8_t pc1[56];
  for (int i = 0; i < 56; ++i) pc1[i] = bitAt(key, 64, kPc1[i]) ? 1 : 0;

  uint8_t c[28], d[28];
  memcpy(c, pc1, 28);
  memcpy(d, pc1 + 28, 28);

  for (int r = 0; r < 16; ++r) {
    const uint8_t sh = kShifts[r];
    for (uint8_t k = 0; k < sh; ++k) {
      const uint8_t c0 = c[0], d0 = d[0];
      memmove(c, c + 1, 27);
      c[27] = c0;
      memmove(d, d + 1, 27);
      d[27] = d0;
    }
    uint8_t cd[56];
    memcpy(cd, c, 28);
    memcpy(cd + 28, d, 28);
    const int outRound = decrypt ? (15 - r) : r;
    memset(ks.round[outRound], 0, 6);
    // cd[] holds one bit per byte (not packed), so index it directly.
    for (int i = 0; i < 48; ++i) {
      if (cd[kPc2[i]]) {
        ks.round[outRound][i >> 3] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
      }
    }
  }
}

uint32_t desF(uint32_t r, const uint8_t subkey[6]) {
  // Expand R (32 bits) to 48 bits, XOR with the round subkey.
  uint8_t rBytes[4] = {static_cast<uint8_t>(r >> 24), static_cast<uint8_t>(r >> 16),
                       static_cast<uint8_t>(r >> 8), static_cast<uint8_t>(r)};
  uint8_t expanded[6] = {0};
  for (int i = 0; i < 48; ++i) {
    if (bitAt(rBytes, 32, kE[i])) {
      expanded[i >> 3] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
    }
  }
  for (int i = 0; i < 6; ++i) expanded[i] ^= subkey[i];

  // Eight S-boxes: 6 bits in, 4 bits out.
  uint32_t sOut = 0;
  for (int s = 0; s < 8; ++s) {
    uint8_t six = 0;
    for (int i = 0; i < 6; ++i) {
      const int bitIdx = s * 6 + i;
      if ((expanded[bitIdx >> 3] >> (7 - (bitIdx & 7))) & 1u) {
        six |= static_cast<uint8_t>(1u << (5 - i));
      }
    }
    // six holds bits b1..b6 with b1 in bit5. row = b1*2 + b6, col = b2..b5.
    const int row = (((six >> 5) & 1) << 1) | (six & 1);
    const int col = (six >> 1) & 0x0f;
    const uint32_t v = kSbox[s][(row << 4) | col];
    sOut |= v << (28 - s * 4);
  }

  // P permutation.
  uint32_t out = 0;
  for (int i = 0; i < 32; ++i) {
    // kP holds 0-based bit positions (pyDes convention): output bit i takes
    // input bit kP[i], and bit j of a 32-bit word lives at shift (31 - j).
    const uint32_t srcBit = (sOut >> (31 - kP[i])) & 1u;
    out |= srcBit << (31 - i);
  }
  return out;
}

void desBlock(const uint8_t in[8], uint8_t out[8], const DesKeySchedule& ks) {
  uint8_t ip[64];
  for (int i = 0; i < 64; ++i) ip[i] = bitAt(in, 64, kIp[i]) ? 1 : 0;

  uint8_t packed[8] = {0};
  for (int i = 0; i < 64; ++i) {
    if (ip[i]) packed[i >> 3] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
  }

  uint32_t l = (static_cast<uint32_t>(packed[0]) << 24) | (static_cast<uint32_t>(packed[1]) << 16) |
               (static_cast<uint32_t>(packed[2]) << 8) | packed[3];
  uint32_t r = (static_cast<uint32_t>(packed[4]) << 24) | (static_cast<uint32_t>(packed[5]) << 16) |
               (static_cast<uint32_t>(packed[6]) << 8) | packed[7];

  for (int i = 0; i < 16; ++i) {
    const uint32_t nr = l ^ desF(r, ks.round[i]);
    l = r;
    r = nr;
  }

  uint8_t pre[8] = {static_cast<uint8_t>(r >> 24), static_cast<uint8_t>(r >> 16),
                    static_cast<uint8_t>(r >> 8),  static_cast<uint8_t>(r),
                    static_cast<uint8_t>(l >> 24), static_cast<uint8_t>(l >> 16),
                    static_cast<uint8_t>(l >> 8),  static_cast<uint8_t>(l)};
  for (int i = 0; i < 64; ++i) {
    const bool b = bitAt(pre, 64, kFp[i]);
    if (b) out[i >> 3] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
    else out[i >> 3] &= static_cast<uint8_t>(~(1u << (7 - (i & 7))));
  }
}

// ------------------------------------------------------------------ AES ----
// FIPS-197 inverse cipher, AES-128 only.
//
// The forward S-box is the single hardcoded table here. Its inverse is derived at
// compile time instead of being typed out: a second 256-entry table is 256 more
// chances to transpose a byte, and the failure mode would surface only as
// "decryption produces garbage" with nothing pointing at the bad entry.
constexpr uint8_t kAesSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,  //
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,  //
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,  //
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,  //
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,  //
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,  //
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,  //
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,  //
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,  //
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,  //
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,  //
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,  //
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,  //
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,  //
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,  //
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

struct AesByteTable {
  uint8_t v[256];
};

constexpr AesByteTable makeAesInvSbox() {
  AesByteTable t{};
  for (int i = 0; i < 256; ++i) t.v[kAesSbox[i]] = static_cast<uint8_t>(i);
  return t;
}
constexpr AesByteTable kAesInvSbox = makeAesInvSbox();

// Only the ten values AES-128 ever indexes.
constexpr uint8_t kAesRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

constexpr uint8_t xtime(const uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) != 0 ? 0x1b : 0x00)); }

// GF(2^8) product. `b` is always one of four fixed coefficients at the call site,
// so the loop is short and branch-predictable; a 256-entry log/antilog pair would
// be faster and cost more Flash than the whole rest of this file.
constexpr uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; ++i) {
    if ((b & 1) != 0) p ^= a;
    a = xtime(a);
    b = static_cast<uint8_t>(b >> 1);
  }
  return p;
}

// 11 round keys, laid out so rk[round][row + 4*col] matches the column-major state.
struct AesKeySchedule {
  uint8_t rk[11][16];
};

void aesExpandKey(const uint8_t key[16], AesKeySchedule& ks) {
  uint8_t w[44][4];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) w[i][j] = key[4 * i + j];
  }
  for (int i = 4; i < 44; ++i) {
    uint8_t t[4] = {w[i - 1][0], w[i - 1][1], w[i - 1][2], w[i - 1][3]};
    if (i % 4 == 0) {
      const uint8_t first = t[0];
      // RotWord, then SubWord, then the round constant.
      t[0] = static_cast<uint8_t>(kAesSbox[t[1]] ^ kAesRcon[i / 4 - 1]);
      t[1] = kAesSbox[t[2]];
      t[2] = kAesSbox[t[3]];
      t[3] = kAesSbox[first];
    }
    for (int j = 0; j < 4; ++j) w[i][j] = static_cast<uint8_t>(w[i - 4][j] ^ t[j]);
  }
  for (int r = 0; r < 11; ++r) {
    for (int c = 0; c < 4; ++c) {
      for (int b = 0; b < 4; ++b) ks.rk[r][4 * c + b] = w[4 * r + c][b];
    }
  }
}

void aesAddRoundKey(uint8_t s[16], const uint8_t rk[16]) {
  for (int i = 0; i < 16; ++i) s[i] = static_cast<uint8_t>(s[i] ^ rk[i]);
}

void aesInvSubBytes(uint8_t s[16]) {
  for (int i = 0; i < 16; ++i) s[i] = kAesInvSbox.v[s[i]];
}

// Row r rotates right by r, i.e. new[r][c] = old[r][(c - r) mod 4].
void aesInvShiftRows(uint8_t s[16]) {
  const uint8_t in[16] = {s[0],  s[1],  s[2],  s[3],  s[4],  s[5],  s[6],  s[7],
                          s[8],  s[9],  s[10], s[11], s[12], s[13], s[14], s[15]};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) s[r + 4 * c] = in[r + 4 * ((c + 4 - r) % 4)];
  }
}

void aesInvMixColumns(uint8_t s[16]) {
  for (int c = 0; c < 4; ++c) {
    const uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
    s[4 * c] = static_cast<uint8_t>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
    s[4 * c + 1] = static_cast<uint8_t>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
    s[4 * c + 2] = static_cast<uint8_t>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
    s[4 * c + 3] = static_cast<uint8_t>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
  }
}

void aesDecryptBlock(const uint8_t in[16], uint8_t out[16], const AesKeySchedule& ks) {
  uint8_t s[16];
  for (int i = 0; i < 16; ++i) s[i] = in[i];
  aesAddRoundKey(s, ks.rk[10]);
  for (int round = 9; round >= 1; --round) {
    aesInvShiftRows(s);
    aesInvSubBytes(s);
    aesAddRoundKey(s, ks.rk[round]);
    aesInvMixColumns(s);
  }
  aesInvShiftRows(s);
  aesInvSubBytes(s);
  aesAddRoundKey(s, ks.rk[0]);
  for (int i = 0; i < 16; ++i) out[i] = s[i];
}

// Validates and strips PKCS#7 over the last `pad` bytes. Returns the plaintext
// length or -1. Checked rather than trusted: a wrong key yields a random last byte
// and an unchecked strip would then report a length up to 255 bytes past the buffer.
int32_t aesStripPkcs7(const uint8_t* plain, size_t len) {
  if (len == 0) return -1;
  const uint8_t pad = plain[len - 1];
  if (pad == 0 || pad > Aes128::kBlockSize || pad > len) return -1;
  for (size_t i = len - pad; i < len; ++i) {
    if (plain[i] != pad) return -1;
  }
  return static_cast<int32_t>(len - pad);
}

}  // namespace

void Md5::hex(const uint8_t* data, size_t len, char hexOut[kHexLen + 1]) {
  uint8_t digest[16];
  md5Raw(data, len, digest);
  for (int i = 0; i < 16; ++i) {
    hexOut[i * 2] = kHexDigits[digest[i] >> 4];
    hexOut[i * 2 + 1] = kHexDigits[digest[i] & 0x0f];
  }
  hexOut[kHexLen] = '\0';
}

int32_t Des::cbcDecrypt(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen], const uint8_t* in,
                        size_t inLen, uint8_t* out, size_t outCap) {
  if (inLen == 0 || (inLen % kBlockSize) != 0) return -1;
  if (outCap < inLen) return -1;

  DesKeySchedule ks;
  desKeySchedule(key, ks, /*decrypt=*/true);

  uint8_t prev[kBlockSize];
  memcpy(prev, iv, kBlockSize);
  for (size_t off = 0; off < inLen; off += kBlockSize) {
    uint8_t plain[kBlockSize];
    desBlock(in + off, plain, ks);
    for (size_t i = 0; i < kBlockSize; ++i) out[off + i] = static_cast<uint8_t>(plain[i] ^ prev[i]);
    memcpy(prev, in + off, kBlockSize);
  }

  const uint8_t pad = out[inLen - 1];
  if (pad == 0 || pad > kBlockSize) return -1;
  for (size_t i = inLen - pad; i < inLen; ++i) {
    if (out[i] != pad) return -1;
  }
  return static_cast<int32_t>(inLen - pad);
}

size_t Des::cbcEncrypt(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen], const uint8_t* in,
                       size_t inLen, uint8_t* out, size_t outCap) {
  const size_t pad = static_cast<size_t>(kBlockSize - (inLen % kBlockSize));
  const size_t total = inLen + pad;
  if (outCap < total) return 0;

  DesKeySchedule ks;
  desKeySchedule(key, ks, /*decrypt=*/false);

  uint8_t block[kBlockSize];
  uint8_t prev[kBlockSize];
  memcpy(prev, iv, kBlockSize);

  size_t off = 0;
  while (off < total) {
    const size_t remaining = total - off;
    const size_t take = remaining < kBlockSize ? remaining : kBlockSize;
    if (off < inLen) {
      const size_t fromIn = (inLen - off) < take ? (inLen - off) : take;
      memcpy(block, in + off, fromIn);
      for (size_t i = fromIn; i < take; ++i) block[i] = static_cast<uint8_t>(pad);
    } else {
      memset(block, static_cast<int>(pad), take);
    }
    for (size_t i = take; i < kBlockSize; ++i) block[i] = static_cast<uint8_t>(pad);
    for (size_t i = 0; i < kBlockSize; ++i) block[i] ^= prev[i];
    desBlock(block, out + off, ks);
    memcpy(prev, out + off, kBlockSize);
    off += kBlockSize;
  }
  return total;
}

int32_t Des::cbcDecryptInPlace(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen], uint8_t* buf, size_t bufLen) {
  if (bufLen == 0 || (bufLen % kBlockSize) != 0) return -1;

  DesKeySchedule ks;
  desKeySchedule(key, ks, /*decrypt=*/true);

  uint8_t prev[kBlockSize];
  uint8_t savedCt[kBlockSize];
  memcpy(prev, iv, kBlockSize);
  for (size_t off = 0; off < bufLen; off += kBlockSize) {
    // Save the ciphertext before overwriting the block: it is the next IV.
    memcpy(savedCt, buf + off, kBlockSize);
    uint8_t plain[kBlockSize];
    desBlock(savedCt, plain, ks);
    for (size_t i = 0; i < kBlockSize; ++i) buf[off + i] = static_cast<uint8_t>(plain[i] ^ prev[i]);
    memcpy(prev, savedCt, kBlockSize);
  }

  const uint8_t pad = buf[bufLen - 1];
  if (pad == 0 || pad > kBlockSize) return -1;
  for (size_t i = bufLen - pad; i < bufLen; ++i) {
    if (buf[i] != pad) return -1;
  }
  return static_cast<int32_t>(bufLen - pad);
}

int32_t Aes128::cbcDecrypt(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen], const uint8_t* in, const size_t inLen,
                           uint8_t* out, const size_t outCap) {
  if (in == nullptr || out == nullptr) return -1;
  if (inLen == 0 || inLen % kBlockSize != 0) return -1;
  if (outCap < inLen) return -1;

  AesKeySchedule ks;
  aesExpandKey(key, ks);
  uint8_t prev[kBlockSize];
  memcpy(prev, iv, kBlockSize);
  for (size_t off = 0; off < inLen; off += kBlockSize) {
    uint8_t block[kBlockSize];
    aesDecryptBlock(in + off, block, ks);
    for (size_t i = 0; i < kBlockSize; ++i) out[off + i] = static_cast<uint8_t>(block[i] ^ prev[i]);
    memcpy(prev, in + off, kBlockSize);
  }
  return aesStripPkcs7(out, inLen);
}

int32_t Aes128::cbcDecryptInPlace(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen], uint8_t* buf,
                                  const size_t bufLen) {
  if (buf == nullptr) return -1;
  if (bufLen == 0 || bufLen % kBlockSize != 0) return -1;

  AesKeySchedule ks;
  aesExpandKey(key, ks);
  uint8_t prev[kBlockSize];
  uint8_t ct[kBlockSize];
  memcpy(prev, iv, kBlockSize);
  for (size_t off = 0; off < bufLen; off += kBlockSize) {
    // Saved before the block is overwritten: CBC needs the ciphertext, not the
    // plaintext, of the previous block.
    memcpy(ct, buf + off, kBlockSize);
    uint8_t block[kBlockSize];
    aesDecryptBlock(ct, block, ks);
    for (size_t i = 0; i < kBlockSize; ++i) buf[off + i] = static_cast<uint8_t>(block[i] ^ prev[i]);
    memcpy(prev, ct, kBlockSize);
  }
  return aesStripPkcs7(buf, bufLen);
}

int32_t Base64::decode(const char* in, size_t inLen, uint8_t* out, size_t outCap) {
  static int8_t table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) table[i] = -1;
    const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(alpha[i])] = static_cast<int8_t>(i);
    init = true;
  }

  uint32_t acc = 0;
  int bits = 0;
  size_t written = 0;
  for (size_t i = 0; i < inLen; ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == '=') break;
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    const int8_t v = table[c];
    if (v < 0) return -1;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (written >= outCap) return -1;
      out[written++] = static_cast<uint8_t>((acc >> bits) & 0xff);
    }
  }
  return static_cast<int32_t>(written);
}

size_t Base64::encode(const uint8_t* in, size_t inLen, char* out, size_t outCap) {
  static const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t need = 4 * ((inLen + 2) / 3) + 1;
  if (outCap < need) return 0;
  size_t o = 0;
  for (size_t i = 0; i < inLen; i += 3) {
    const uint32_t b0 = in[i];
    const uint32_t b1 = (i + 1 < inLen) ? in[i + 1] : 0;
    const uint32_t b2 = (i + 2 < inLen) ? in[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out[o++] = alpha[(v >> 18) & 0x3f];
    out[o++] = alpha[(v >> 12) & 0x3f];
    out[o++] = (i + 1 < inLen) ? alpha[(v >> 6) & 0x3f] : '=';
    out[o++] = (i + 2 < inLen) ? alpha[v & 0x3f] : '=';
  }
  out[o] = '\0';
  return o;
}

bool Envelope::deriveKeyIv(const char* accessKey, const char* keyString, const char* body,
                           size_t bodyLen, char keyOut[9], char ivOut[9], const char** cipherStart,
                           size_t* cipherLen) {
  if (!accessKey || !keyString || !body) return false;
  const size_t akLen = strlen(accessKey);
  const size_t ksLen = strlen(keyString);
  if (akLen == 0 || ksLen == 0 || bodyLen <= 12) return false;

  uint32_t sum = 0;
  for (size_t i = 0; i < akLen; ++i) sum += static_cast<uint8_t>(accessKey[i]);

  const size_t start = sum % ksLen;
  size_t finish = start + (sum / 65);
  if (finish > ksLen) finish = ksLen;
  if (finish <= start) return false;
  const size_t partLen = finish - start;

  const bool saltAtEnd = (static_cast<uint8_t>(accessKey[akLen - 1]) % 2) == 1;
  const char* salt;
  if (saltAtEnd) {
    salt = body + (bodyLen - 12);
    *cipherStart = body;
    *cipherLen = bodyLen - 12;
  } else {
    salt = body;
    *cipherStart = body + 12;
    *cipherLen = bodyLen - 12;
  }

  // key = md5hex(keypart + salt)[:8]
  char seed[512];
  if (partLen + 12 >= sizeof(seed)) return false;
  memcpy(seed, keyString + start, partLen);
  memcpy(seed + partLen, salt, 12);
  char hex[33];
  Md5::hex(reinterpret_cast<const uint8_t*>(seed), partLen + 12, hex);
  memcpy(keyOut, hex, 8);
  keyOut[8] = '\0';

  // iv = md5hex(salt)[:8]
  Md5::hex(reinterpret_cast<const uint8_t*>(salt), 12, hex);
  memcpy(ivOut, hex, 8);
  ivOut[8] = '\0';
  return true;
}

// Allocating variant: convenient for host tests and desktop tooling. Device
// code should prefer decodeInPlace(), which needs no heap at all.
bool Envelope::decode(const char* body, size_t bodyLen, const char* accessKey,
                      const char* keyString, char* out, size_t outCap) {
  char key[9], iv[9];
  const char* cipherStart = nullptr;
  size_t cipherLen = 0;
  if (!deriveKeyIv(accessKey, keyString, body, bodyLen, key, iv, &cipherStart, &cipherLen)) {
    return false;
  }

  // Base64 -> binary. Worst case: 3/4 of cipherLen.
  const size_t binCap = (cipherLen / 4 + 1) * 3 + 8;
  uint8_t* bin = static_cast<uint8_t*>(malloc(binCap));
  if (!bin) return false;
  const int32_t binLen = Base64::decode(cipherStart, cipherLen, bin, binCap);
  if (binLen <= 0 || (static_cast<size_t>(binLen) % Des::kBlockSize) != 0) {
    free(bin);
    return false;
  }
  if (static_cast<size_t>(binLen) + 1 > outCap) {
    free(bin);
    return false;
  }

  const int32_t plainLen = Des::cbcDecrypt(reinterpret_cast<const uint8_t*>(key),
                                           reinterpret_cast<const uint8_t*>(iv), bin,
                                           static_cast<size_t>(binLen),
                                           reinterpret_cast<uint8_t*>(out), outCap - 1);
  free(bin);
  if (plainLen < 0) return false;
  out[plainLen] = '\0';
  return true;
}

bool Envelope::decodeInPlace(char* body, size_t bodyLen, const char* accessKey, const char* keyString,
                             size_t* outLen) {
  char key[9], iv[9];
  const char* cipherStart = nullptr;
  size_t cipherLen = 0;
  if (!deriveKeyIv(accessKey, keyString, body, bodyLen, key, iv, &cipherStart, &cipherLen)) return false;
  if (outLen) *outLen = 0;

  // deriveKeyIv() already consumed the 12-byte salt from `body`; nothing else
  // needs it, so the buffer can be overwritten from here on.
  const size_t cipherOffset = static_cast<size_t>(cipherStart - body);
  uint8_t* const dst = reinterpret_cast<uint8_t*>(body);
  const int32_t binLen = Base64::decode(body + cipherOffset, cipherLen, dst, bodyLen);
  if (binLen <= 0 || (static_cast<size_t>(binLen) % Des::kBlockSize) != 0) return false;

  const int32_t plainLen = Des::cbcDecryptInPlace(reinterpret_cast<const uint8_t*>(key),
                                                  reinterpret_cast<const uint8_t*>(iv), dst,
                                                  static_cast<size_t>(binLen));
  if (plainLen < 0) return false;
  body[plainLen] = '\0';
  if (outLen) *outLen = static_cast<size_t>(plainLen);
  return true;
}

// ------------------------------------------------------- DesBase64Stream ----

namespace {

int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// The opaque schedule buffer in the header must match the internal layout.
static_assert(sizeof(DesKeySchedule) == DesBase64Stream::kScheduleBytes,
              "DesBase64Stream::kScheduleBytes must match DesKeySchedule");

}  // namespace

void DesBase64Stream::begin(const uint8_t key[Des::kKeyLen], const uint8_t iv[Des::kKeyLen], TextSink sink,
                            void* ctx) {
  auto* ks = reinterpret_cast<DesKeySchedule*>(schedule_);
  desKeySchedule(key, *ks, /*decrypt=*/true);
  memcpy(prev_, iv, 8);
  blockLen_ = 0;
  b64Acc_ = 0;
  b64Bits_ = 0;
  totalOut_ = 0;
  haveLast_ = false;
  sink_ = sink;
  ctx_ = ctx;
  started_ = true;
  cancelled_ = false;
}

bool DesBase64Stream::flushCipherBlock() {
  auto* ks = reinterpret_cast<DesKeySchedule*>(schedule_);
  uint8_t plain[8];
  desBlock(block_, plain, *ks);
  for (int i = 0; i < 8; ++i) plain[i] = static_cast<uint8_t>(plain[i] ^ prev_[i]);
  memcpy(prev_, block_, 8);

  // Hold one block back: PKCS#7 padding lives in the final block, so it cannot
  // be emitted until finish() proves there is no successor.
  if (haveLast_ && sink_) {
    if (!sink_(ctx_, reinterpret_cast<const char*>(lastPlain_), 8)) {
      cancelled_ = true;
      return false;
    }
    totalOut_ += 8;
  }
  memcpy(lastPlain_, plain, 8);
  haveLast_ = true;
  return true;
}

bool DesBase64Stream::feed(const char* base64, size_t len) {
  if (!started_ || cancelled_) return !cancelled_;
  for (size_t i = 0; i < len; ++i) {
    const char c = base64[i];
    if (c == '=') continue;  // trailing padding; finish() handles the remainder
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    const int v = b64Value(c);
    if (v < 0) return false;
    b64Acc_ = (b64Acc_ << 6) | static_cast<uint32_t>(v);
    b64Bits_ = static_cast<uint8_t>(b64Bits_ + 6);
    if (b64Bits_ >= 8) {
      b64Bits_ = static_cast<uint8_t>(b64Bits_ - 8);
      block_[blockLen_++] = static_cast<uint8_t>((b64Acc_ >> b64Bits_) & 0xff);
      if (blockLen_ == 8) {
        blockLen_ = 0;
        if (!flushCipherBlock()) return false;
      }
    }
  }
  return true;
}

bool DesBase64Stream::finish() {
  if (!started_) return false;
  if (cancelled_) return false;
  // A well-formed PKCS#7 ciphertext is a whole number of blocks with no Base64
  // bits left over.
  if (blockLen_ != 0 || b64Bits_ >= 6) return false;
  if (!haveLast_) return false;

  const uint8_t pad = lastPlain_[7];
  if (pad == 0 || pad > 8) return false;
  for (size_t i = static_cast<size_t>(8 - pad); i < 8; ++i) {
    if (lastPlain_[i] != pad) return false;
  }
  const size_t keep = static_cast<size_t>(8 - pad);
  if (keep > 0 && sink_) {
    if (!sink_(ctx_, reinterpret_cast<const char*>(lastPlain_), keep)) {
      cancelled_ = true;
      return false;
    }
    totalOut_ += keep;
  }
  started_ = false;
  return true;
}

}  // namespace jjwxc
