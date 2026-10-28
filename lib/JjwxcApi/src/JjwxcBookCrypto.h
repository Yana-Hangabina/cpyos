// Encryption for cached JJWXC content.
//
// WHAT THIS IS
//   A small, self-contained AES-128-CTR container that keeps the on-SD chapter
//   cache unreadable when the files are copied to another device. Version 2
//   roots the key in the 32-byte device secret kept in NVS:
//
//     key = HMAC-SHA256(deviceSecret, "JJXBOOK/key/v2" || mac || salt)[0..15]
//
//   mac  is the 6-byte factory MAC (HalSystem::getDeviceId), and salt is a
//   16-byte random value generated once per device and stored next to the cache
//   (/.crosspoint/jjwxc/cachekey; this module never touches the filesystem).
//   The secret is the only input an attacker with the SD card does not have.
//
//   Version 1 (the first release) derived the key from the MAC and salt alone:
//
//     key = MD5("JJXBOOK/key/v1" || mac || salt)
//
//   The MAC is printed by `esptool chip_id` and the salt ships on the same card,
//   so v1 was recoverable offline. deriveLegacyBookKey and the version-1 read
//   path exist only so existing caches can be migrated in place; do not create
//   new v1 blobs.
//
// WHAT THIS IS NOT
//   This is *content obfuscation*, not DRM and not protection against a
//   determined attacker. NVS is not flash-encrypted on this build, so a full
//   flash dump recovers the device secret; a task that can run code on the
//   device can always recover the plaintext it renders. It stops the cheap
//   cases: copy the cards, or open the files on a PC, and the content is noise.
//   Do not describe it to users as "copy protection" that survives a motivated
//   adversary.
//
// FILE FORMAT (all integers little-endian)
//
//   offset  size  field
//   0       4     magic "JJX1"
//   4       1     version = 2
//   5       3     reserved, must be 0 (checked; also rounds the header to 32 B)
//   8       16    nonce = initial AES-CTR counter block (public)
//   24      4     plaintext length in bytes
//   28      4     CRC-32 (ISO-HDLC, i.e. zlib/Python binascii.crc32) of plaintext
//   32      N     ciphertext, N = plaintext length
//
//   The nonce is random and supplied by the caller, then stored in the header.
//   Two different plaintexts therefore never share a keystream and equal
//   plaintexts do not produce equal ciphertexts. Version 1 derived the nonce
//   from the plaintext; its layout is otherwise identical, so one decrypt path
//   handles both versions once the caller supplies the matching key.
//
//   The CRC detects truncation, bit rot, a stale salt and the wrong device. It
//   is an accident detector, not a MAC. A deliberate forger cannot produce a
//   valid blob without the key, but this module makes no stronger promise.
//
// MEMORY AND COST
//   No heap, no exceptions, no RTTI, no external crypto dependency. The only
//   static data is a 256-byte AES S-box (SHA-256 constants live in .rodata).
//   One-shot operations take caller-provided buffers. Encryption costs one
//   AES-CTR pass plus one MD5-free SHA-256 KDF (once per session when the caller
//   caches the derived key). CtrStream lets a writer encrypt as it streams, so
//   no plaintext intermediate file is ever published.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

struct BookCrypto {
  static constexpr size_t kMacLen = 6;
  static constexpr size_t kSaltLen = 16;
  static constexpr size_t kSecretLen = 32;
  static constexpr size_t kKeyLen = 16;
  static constexpr size_t kNonceLen = 16;
  static constexpr size_t kHeaderLen = 32;

  // Header layout constants, exported so callers and tests agree on offsets
  // instead of re-deriving them.
  static constexpr size_t kMagicOffset = 0;
  static constexpr size_t kVersionOffset = 4;
  static constexpr size_t kReservedOffset = 5;
  static constexpr size_t kNonceOffset = 8;
  static constexpr size_t kLengthOffset = 24;
  static constexpr size_t kCrcOffset = 28;
  static constexpr uint8_t kVersion = 2;
  static constexpr uint8_t kVersionLegacy = 1;

  // Derives the per-device key from the NVS device secret. Domain-separated
  // from every other use of the secret. Reference-to-array parameters make a
  // wrong-size buffer a compile error instead of a silent over-read/over-write;
  // an earlier pointer-based signature let a 4-argument call bind by accident.
  static bool deriveBookKey(const uint8_t (&secret)[kSecretLen], const uint8_t (&mac)[kMacLen],
                            const uint8_t (&salt)[kSaltLen], uint8_t (&key)[kKeyLen]);

  // The version-1 KDF. Only for reading caches written by the first release;
  // new blobs must use deriveBookKey.
  static bool deriveLegacyBookKey(const uint8_t (&mac)[kMacLen], const uint8_t (&salt)[kSaltLen],
                                  uint8_t (&key)[kKeyLen]);

  // Returns kVersion, kVersionLegacy, or 0 when `in` is not a valid JJX1 header
  // (short input, bad magic, non-zero reserved bytes, unknown version). Use this
  // to pick the matching key before calling decryptBookBlob.
  static uint8_t blobVersion(const uint8_t* in, size_t inLen);

  // Fills a 32-byte v2 header. `plainLen` and `plainCrc` may be zero while a
  // stream is still being written; a later call with the final values patches
  // the header in place. Exposed so ChapterWriter does not duplicate the layout.
  static void writeHeader(uint8_t out[kHeaderLen], const uint8_t nonce[kNonceLen], uint32_t plainLen,
                          uint32_t plainCrc);

  // Writes kHeaderLen + len bytes into `out` and sets outLen (0 on failure).
  // Returns false if `outCap` is too small, `len` exceeds uint32, or a pointer
  // is null. `out` must not overlap `plain`. `nonce` is generated by the caller
  // (esp_fill_random on the device); it is stored in the header.
  static bool encryptBookBlob(const uint8_t (&key)[kKeyLen], const uint8_t (&nonce)[kNonceLen], const uint8_t* plain,
                              size_t len, uint8_t* out, size_t outCap, size_t& outLen);

  // Validates magic/version/reserved/length, decrypts, then verifies the CRC.
  // Accepts both kVersion and kVersionLegacy; the caller must pass the key that
  // matches the blob version (see blobVersion). Sets outLen = 0 and returns
  // false on any mismatch or short buffer. On CRC failure the already-decrypted
  // bytes are zeroed so a caller that ignores the return value cannot display a
  // wrong-key garbage page.
  //
  // `out` may equal `in` (same buffer): the ciphertext starts 32 bytes into the
  // buffer, so forward in-place decryption is safe and compacts the plaintext to
  // the front. It must not partially overlap in any other way.
  static bool decryptBookBlob(const uint8_t (&key)[kKeyLen], const uint8_t* in, size_t inLen, uint8_t* out,
                              size_t outCap, size_t& outLen);

  // Streaming AES-128-CTR state. One instance per file; init() expands the key
  // once, xorBytes() may be called with any chunk sizes. Supports in == out.
  struct CtrStream {
    static constexpr size_t kRoundKeys = 176;
    uint8_t roundKeys[kRoundKeys];
    uint8_t counter[kNonceLen];
    uint8_t keystream[kNonceLen];
    uint8_t pos;  // 0..16; 16 means the next byte regenerates the keystream
    static void init(CtrStream& state, const uint8_t (&key)[kKeyLen], const uint8_t (&nonce)[kNonceLen]);
    static void xorBytes(CtrStream& state, const uint8_t* in, uint8_t* out, size_t len);
  };

  // Exposed so the host test can run the FIPS-197 block known-answer test; the
  // CTR wrappers below are what production code uses. `out` may equal `in`.
  static void aes128EncryptBlock(const uint8_t key[kKeyLen], const uint8_t in[16], uint8_t out[16]);

  // AES-128-CTR over a full 128-bit big-endian counter that starts at
  // `initialCounter`. Supports in == out. Exposed for the NIST SP 800-38A
  // known-answer test and for future streaming wiring.
  static void aes128CtrXor(const uint8_t key[kKeyLen], const uint8_t initialCounter[16], const uint8_t* in, size_t len,
                           uint8_t* out);

  // AES-128-CBC encryption of a whole number of blocks, with the key schedule
  // expanded once. The caller owns padding (the protected-EPUB writer appends
  // PKCS#7 bytes). Supports in == out. Returns false for a null pointer or a
  // length that is not a multiple of 16.
  static bool aes128CbcEncrypt(const uint8_t key[kKeyLen], const uint8_t iv[16], const uint8_t* in, size_t len,
                               uint8_t* out);

  // CRC-32/ISO-HDLC with explicit state, so a streaming writer can feed chunks:
  // start with 0xFFFFFFFF, feed data, then XOR the final value with 0xFFFFFFFF.
  static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);

  // Exposed for NIST/FIPS known-answer tests. Production code only uses the
  // KDF above.
  static void sha256Digest(const uint8_t* data, size_t len, uint8_t out[32]);
  static void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen, uint8_t out[32]);
};

}  // namespace jjwxc
