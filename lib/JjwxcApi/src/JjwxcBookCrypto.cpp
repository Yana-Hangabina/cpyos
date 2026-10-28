#include "JjwxcBookCrypto.h"

#include <string.h>

#include "JjwxcCrypto.h"  // jjwxc::Md5 -- version-1 legacy KDF only

namespace jjwxc {
namespace {

constexpr char kMagic[4] = {'J', 'J', 'X', '1'};

// Domain-separation labels. Fixed ASCII strings hashed together with fixed-size
// binary inputs (6-byte MAC, 16-byte salt), so no length prefix is needed.
constexpr char kKeyLabelV2[] = "JJXBOOK/key/v2";
constexpr char kKeyLabelV1[] = "JJXBOOK/key/v1";

static_assert(sizeof(kKeyLabelV2) - 1 + BookCrypto::kMacLen + BookCrypto::kSaltLen <= 64, "v2 label fits");
static_assert(sizeof(kKeyLabelV1) - 1 + BookCrypto::kMacLen + BookCrypto::kSaltLen <= 64, "v1 label fits");

// ---------------------------------------------------------------- SHA-256 ---
// Self-contained so this module stays host-testable without wolfSSL/mbedTLS.
// The device paths that matter (the KDF) run once per session; the streaming
// CTR path never hashes.
struct Sha256Ctx {
  uint32_t h[8];
  uint64_t bits;
  uint8_t buf[64];
  size_t bufLen;
};

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

void sha256Compress(uint32_t h[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
    const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

void sha256Init(Sha256Ctx& c) {
  c.h[0] = 0x6a09e667u;
  c.h[1] = 0xbb67ae85u;
  c.h[2] = 0x3c6ef372u;
  c.h[3] = 0xa54ff53au;
  c.h[4] = 0x510e527fu;
  c.h[5] = 0x9b05688cu;
  c.h[6] = 0x1f83d9abu;
  c.h[7] = 0x5be0cd19u;
  c.bits = 0;
  c.bufLen = 0;
}

void sha256Update(Sha256Ctx& c, const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) return;
  c.bits += static_cast<uint64_t>(len) * 8u;
  while (len > 0) {
    const size_t take = (64 - c.bufLen < len) ? 64 - c.bufLen : len;
    memcpy(c.buf + c.bufLen, data, take);
    c.bufLen += take;
    data += take;
    len -= take;
    if (c.bufLen == 64) {
      sha256Compress(c.h, c.buf);
      c.bufLen = 0;
    }
  }
}

void sha256Final(Sha256Ctx& c, uint8_t out[32]) {
  const uint64_t bits = c.bits;
  c.buf[c.bufLen++] = 0x80;
  if (c.bufLen > 56) {
    memset(c.buf + c.bufLen, 0, 64 - c.bufLen);
    sha256Compress(c.h, c.buf);
    c.bufLen = 0;
  }
  memset(c.buf + c.bufLen, 0, 56 - c.bufLen);
  for (size_t i = 0; i < 8; ++i) c.buf[56 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  sha256Compress(c.h, c.buf);
  for (size_t i = 0; i < 8; ++i) {
    out[i * 4 + 0] = static_cast<uint8_t>(c.h[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(c.h[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(c.h[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(c.h[i]);
  }
}

void hmacSha256Impl(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen, uint8_t out[32]) {
  uint8_t k[64] = {};
  if (keyLen > sizeof(k)) {
    Sha256Ctx hash;
    sha256Init(hash);
    sha256Update(hash, key, keyLen);
    sha256Final(hash, k);
  } else if (key != nullptr && keyLen > 0) {
    memcpy(k, key, keyLen);
  }

  uint8_t ipad[64];
  uint8_t opad[64];
  for (size_t i = 0; i < sizeof(k); ++i) {
    ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
    opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
  }

  uint8_t inner[32];
  Sha256Ctx ctx;
  sha256Init(ctx);
  sha256Update(ctx, ipad, sizeof(ipad));
  sha256Update(ctx, data, dataLen);
  sha256Final(ctx, inner);

  sha256Init(ctx);
  sha256Update(ctx, opad, sizeof(opad));
  sha256Update(ctx, inner, sizeof(inner));
  sha256Final(ctx, out);

  memset(k, 0, sizeof(k));
  memset(ipad, 0, sizeof(ipad));
  memset(opad, 0, sizeof(opad));
}

// ---------------------------------------------------------------- AES-128 ---
// Forward direction only: CTR and the EPUB CBC writer need the block cipher in
// the encrypt direction. ~256 B of rodata (S-box) plus code.
constexpr uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

inline uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80u) ? 0x1bu : 0x00u)); }

void expandKey(const uint8_t key[BookCrypto::kKeyLen], uint8_t roundKeys[176]) {
  memcpy(roundKeys, key, BookCrypto::kKeyLen);
  uint8_t rcon = 1;
  for (size_t i = BookCrypto::kKeyLen; i < 176; i += 4) {
    uint8_t t0 = roundKeys[i - 4];
    uint8_t t1 = roundKeys[i - 3];
    uint8_t t2 = roundKeys[i - 2];
    uint8_t t3 = roundKeys[i - 1];
    if (i % 16 == 0) {
      // RotWord + SubWord + Rcon
      const uint8_t first = t0;
      t0 = static_cast<uint8_t>(kSbox[t1] ^ rcon);
      t1 = kSbox[t2];
      t2 = kSbox[t3];
      t3 = kSbox[first];
      rcon = xtime(rcon);
    }
    roundKeys[i + 0] = static_cast<uint8_t>(roundKeys[i - 16 + 0] ^ t0);
    roundKeys[i + 1] = static_cast<uint8_t>(roundKeys[i - 16 + 1] ^ t1);
    roundKeys[i + 2] = static_cast<uint8_t>(roundKeys[i - 16 + 2] ^ t2);
    roundKeys[i + 3] = static_cast<uint8_t>(roundKeys[i - 16 + 3] ^ t3);
  }
}

void addRoundKey(uint8_t state[16], const uint8_t* roundKey) {
  for (size_t i = 0; i < 16; ++i) state[i] ^= roundKey[i];
}

void subBytes(uint8_t state[16]) {
  for (size_t i = 0; i < 16; ++i) state[i] = kSbox[state[i]];
}

// State is column-major: state[row + 4 * column]. Row r rotates left by r.
void shiftRows(uint8_t s[16]) {
  uint8_t t = s[1];
  s[1] = s[5];
  s[5] = s[9];
  s[9] = s[13];
  s[13] = t;

  t = s[2];
  s[2] = s[10];
  s[10] = t;
  t = s[6];
  s[6] = s[14];
  s[14] = t;

  t = s[15];
  s[15] = s[11];
  s[11] = s[7];
  s[7] = s[3];
  s[3] = t;
}

void mixColumns(uint8_t s[16]) {
  for (size_t c = 0; c < 4; ++c) {
    uint8_t* a = s + 4 * c;
    const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
    const uint8_t all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
    a[0] = static_cast<uint8_t>(a0 ^ all ^ xtime(static_cast<uint8_t>(a0 ^ a1)));
    a[1] = static_cast<uint8_t>(a1 ^ all ^ xtime(static_cast<uint8_t>(a1 ^ a2)));
    a[2] = static_cast<uint8_t>(a2 ^ all ^ xtime(static_cast<uint8_t>(a2 ^ a3)));
    a[3] = static_cast<uint8_t>(a3 ^ all ^ xtime(static_cast<uint8_t>(a3 ^ a0)));
  }
}

void encryptBlockWithSchedule(const uint8_t roundKeys[176], const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  memcpy(s, in, 16);

  addRoundKey(s, roundKeys);
  for (size_t round = 1; round < 10; ++round) {
    subBytes(s);
    shiftRows(s);
    mixColumns(s);
    addRoundKey(s, roundKeys + 16 * round);
  }
  subBytes(s);
  shiftRows(s);
  addRoundKey(s, roundKeys + 160);
  memcpy(out, s, 16);
}

void incrementCounter(uint8_t counter[16]) {
  // 128-bit big-endian increment; NIST SP 800-38A treats the whole block as the
  // counter, so a carry out of the low byte must propagate upwards.
  for (int i = 15; i >= 0; --i) {
    if (++counter[i] != 0) break;
  }
}

void ctrXorWithSchedule(const uint8_t roundKeys[176], const uint8_t initialCounter[16], const uint8_t* in, size_t len,
                        uint8_t* out) {
  uint8_t counter[16];
  memcpy(counter, initialCounter, 16);  // copied before the first write: `out` may alias `in`
  uint8_t stream[16];
  size_t off = 0;
  while (off < len) {
    encryptBlockWithSchedule(roundKeys, counter, stream);
    const size_t n = (len - off < 16) ? (len - off) : 16;
    for (size_t i = 0; i < n; ++i) out[off + i] = static_cast<uint8_t>(in[off + i] ^ stream[i]);
    incrementCounter(counter);
    off += n;
  }
}

// ------------------------------------------------------------------ misc ----
uint32_t crc32Bytes(const uint8_t* data, size_t len) {
  // CRC-32/ISO-HDLC: reflected polynomial 0xEDB88320, init/final 0xFFFFFFFF.
  // Bitwise on purpose: a 1 KB table is not worth the Flash for cache-sized
  // blobs, and this keeps the module's static data to just the AES S-box.
  uint32_t crc = BookCrypto::crc32Update(0xFFFFFFFFu, data, len);
  return crc ^ 0xFFFFFFFFu;
}

void putLe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

uint32_t getLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
  return static_cast<uint8_t>(c - 'A' + 10);  // Md5::hex emits lowercase; tolerated for safety
}

// Md5::hex returns 32 lowercase hex chars; unpack them into the raw 16-byte
// digest. Keeping the string form only inside this file avoids touching (or
// duplicating) JjwxcCrypto's one-shot MD5 API.
void hexToDigest(const char hex[33], uint8_t out[16]) {
  for (size_t i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>((hexNibble(hex[i * 2]) << 4) | hexNibble(hex[i * 2 + 1]));
}

void md5Digest(const char* label, size_t labelLen, const uint8_t* mac, const uint8_t* salt, uint8_t out[16]) {
  uint8_t buf[64];
  size_t n = 0;
  memcpy(buf + n, label, labelLen);
  n += labelLen;
  memcpy(buf + n, mac, BookCrypto::kMacLen);
  n += BookCrypto::kMacLen;
  memcpy(buf + n, salt, BookCrypto::kSaltLen);
  n += BookCrypto::kSaltLen;

  char hex[33];
  Md5::hex(buf, n, hex);
  hexToDigest(hex, out);
}

}  // namespace

// --------------------------------------------------------------- public -----
bool BookCrypto::deriveBookKey(const uint8_t (&secret)[kSecretLen], const uint8_t (&mac)[kMacLen],
                               const uint8_t (&salt)[kSaltLen], uint8_t (&key)[kKeyLen]) {
  uint8_t message[64];
  size_t n = 0;
  constexpr size_t kLabelLen = sizeof(kKeyLabelV2) - 1;
  memcpy(message, kKeyLabelV2, kLabelLen);
  n += kLabelLen;
  memcpy(message + n, mac, kMacLen);
  n += kMacLen;
  memcpy(message + n, salt, kSaltLen);
  n += kSaltLen;

  uint8_t digest[32];
  hmacSha256Impl(secret, kSecretLen, message, n, digest);
  memcpy(key, digest, kKeyLen);
  return true;
}

bool BookCrypto::deriveLegacyBookKey(const uint8_t (&mac)[kMacLen], const uint8_t (&salt)[kSaltLen],
                                     uint8_t (&key)[kKeyLen]) {
  md5Digest(kKeyLabelV1, sizeof(kKeyLabelV1) - 1, mac, salt, key);
  return true;
}

uint8_t BookCrypto::blobVersion(const uint8_t* in, size_t inLen) {
  if (in == nullptr || inLen < kHeaderLen) return 0;
  if (memcmp(in + kMagicOffset, kMagic, sizeof(kMagic)) != 0) return 0;
  if (in[kReservedOffset + 0] != 0 || in[kReservedOffset + 1] != 0 || in[kReservedOffset + 2] != 0) return 0;
  const uint8_t version = in[kVersionOffset];
  return (version == kVersion || version == kVersionLegacy) ? version : 0;
}

void BookCrypto::writeHeader(uint8_t out[kHeaderLen], const uint8_t nonce[kNonceLen], uint32_t plainLen,
                             uint32_t plainCrc) {
  memcpy(out + kMagicOffset, kMagic, sizeof(kMagic));
  out[kVersionOffset] = kVersion;
  out[kReservedOffset + 0] = 0;
  out[kReservedOffset + 1] = 0;
  out[kReservedOffset + 2] = 0;
  memcpy(out + kNonceOffset, nonce, kNonceLen);
  putLe32(out + kLengthOffset, plainLen);
  putLe32(out + kCrcOffset, plainCrc);
}

bool BookCrypto::encryptBookBlob(const uint8_t (&key)[kKeyLen], const uint8_t (&nonce)[kNonceLen],
                                 const uint8_t* plain, size_t len, uint8_t* out, size_t outCap, size_t& outLen) {
  outLen = 0;
  if (out == nullptr) return false;
  if (plain == nullptr && len != 0) return false;
  if (len > 0xFFFFFFFFu) return false;
  if (outCap < kHeaderLen || len > outCap - kHeaderLen) return false;
  if (plain != nullptr && len != 0) {
    // Reject any overlap, not just `out == plain`: the header is written before
    // the ciphertext, so a partially overlapping buffer would corrupt the input.
    const uintptr_t inLo = reinterpret_cast<uintptr_t>(plain);
    const uintptr_t inHi = inLo + len;
    const uintptr_t outLo = reinterpret_cast<uintptr_t>(out);
    const uintptr_t outHi = outLo + kHeaderLen + len;
    if (outLo < inHi && inLo < outHi) return false;
  }

  const uint32_t plainLen = static_cast<uint32_t>(len);
  const uint32_t plainCrc = crc32Bytes(plain, len);
  writeHeader(out, nonce, plainLen, plainCrc);
  aes128CtrXor(key, nonce, plain, len, out + kHeaderLen);
  outLen = kHeaderLen + len;
  return true;
}

bool BookCrypto::decryptBookBlob(const uint8_t (&key)[kKeyLen], const uint8_t* in, size_t inLen, uint8_t* out,
                                 size_t outCap, size_t& outLen) {
  outLen = 0;
  if (in == nullptr || out == nullptr) return false;
  if (blobVersion(in, inLen) == 0) return false;

  const uint32_t plainLen = getLe32(in + kLengthOffset);
  const uint32_t expectedCrc = getLe32(in + kCrcOffset);
  if (static_cast<size_t>(plainLen) != inLen - kHeaderLen) return false;  // truncation or trailing bytes
  const size_t len = static_cast<size_t>(plainLen);
  if (outCap < len) return false;

  // The nonce is copied inside aes128CtrXor before the first write, so out == in
  // is safe: the ciphertext lives 32 bytes ahead of the output cursor.
  aes128CtrXor(key, in + kNonceOffset, in + kHeaderLen, len, out);

  if (crc32Bytes(out, len) != expectedCrc) {
    // Wrong key, stale salt, or tampering. Scrub rather than hand back garbage:
    // a caller that logs and forgets to check the return value must not be able
    // to render this.
    memset(out, 0, len);
    return false;
  }

  outLen = len;
  return true;
}

void BookCrypto::CtrStream::init(CtrStream& state, const uint8_t (&key)[kKeyLen],
                                 const uint8_t (&nonce)[kNonceLen]) {
  expandKey(key, state.roundKeys);
  memcpy(state.counter, nonce, kNonceLen);
  state.pos = kNonceLen;
}

void BookCrypto::CtrStream::xorBytes(CtrStream& state, const uint8_t* in, uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (state.pos >= kNonceLen) {
      encryptBlockWithSchedule(state.roundKeys, state.counter, state.keystream);
      incrementCounter(state.counter);
      state.pos = 0;
    }
    out[i] = static_cast<uint8_t>(in[i] ^ state.keystream[state.pos++]);
  }
}

void BookCrypto::aes128EncryptBlock(const uint8_t key[kKeyLen], const uint8_t in[16], uint8_t out[16]) {
  uint8_t roundKeys[176];
  expandKey(key, roundKeys);
  encryptBlockWithSchedule(roundKeys, in, out);
}

void BookCrypto::aes128CtrXor(const uint8_t key[kKeyLen], const uint8_t initialCounter[16], const uint8_t* in,
                              size_t len, uint8_t* out) {
  if (len == 0) return;
  uint8_t roundKeys[176];
  expandKey(key, roundKeys);
  ctrXorWithSchedule(roundKeys, initialCounter, in, len, out);
}

bool BookCrypto::aes128CbcEncrypt(const uint8_t key[kKeyLen], const uint8_t iv[16], const uint8_t* in, size_t len,
                                  uint8_t* out) {
  if (key == nullptr || iv == nullptr || in == nullptr || out == nullptr || (len % 16) != 0) return false;
  if (len == 0) return true;

  uint8_t roundKeys[176];
  expandKey(key, roundKeys);
  uint8_t previous[16];
  memcpy(previous, iv, sizeof(previous));
  for (size_t off = 0; off < len; off += 16) {
    uint8_t block[16];
    for (size_t i = 0; i < sizeof(block); ++i) block[i] = static_cast<uint8_t>(in[off + i] ^ previous[i]);
    encryptBlockWithSchedule(roundKeys, block, out + off);
    memcpy(previous, out + off, sizeof(previous));
  }
  return true;
}

uint32_t BookCrypto::crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  if (data == nullptr) return crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

void BookCrypto::sha256Digest(const uint8_t* data, size_t len, uint8_t out[32]) {
  Sha256Ctx ctx;
  sha256Init(ctx);
  sha256Update(ctx, data, len);
  sha256Final(ctx, out);
}

void BookCrypto::hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen, uint8_t out[32]) {
  hmacSha256Impl(key, keyLen, data, dataLen, out);
}

}  // namespace jjwxc
