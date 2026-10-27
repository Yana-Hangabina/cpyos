// Device-bound encryption for cached book data (JJWXC).
//
// WHAT THIS IS
//   A small, self-contained AES-128-CTR container that makes the on-SD cache
//   unreadable when the files are copied to another device. The key is derived
//   from two inputs that belong to the device:
//
//     * the 6-byte factory MAC (HalSystem::getDeviceId, which wraps
//       esp_efuse_mac_get_default), and
//     * a 16-byte random salt generated once per device and stored next to the
//       cache (planned location: /.crosspoint/jjwxc/bookkey; this module never
//       touches the filesystem).
//
//   key       = MD5("JJXBOOK/key/v1" || mac || salt)        -> 16 bytes
//   nonceSeed = MD5("JJXBOOK/iv/v1"  || mac || salt)[0..7]  ->  8 bytes
//
//   The MAC is the eFuse MAC, not the WiFi STA MAC; it is stable across
//   reflashes and SD cards, and it does not change when the user edits WiFi
//   settings. Deleting bookkey therefore invalidates every cached book (the
//   cache can always be rebuilt from the network) -- that is intentional.
//
// WHAT THIS IS NOT
//   This is *device-bound obfuscation*, not DRM and not protection against a
//   determined attacker. The eFuse MAC is printed by `esptool chip_id`, and the
//   salt lives on the same SD card as the ciphertext. Someone who copies both
//   and knows the MAC can re-derive the key. There is no signature, no
//   revocation, no key server, and no attempt to resist side channels. It stops
//   the cheap cases: copy the cache to another Pico, or open it on a PC, and the
//   content is noise. Do not describe it to users as "copy protection" that
//   survives a motivated adversary.
//
// FILE FORMAT (all integers little-endian)
//
//   offset  size  field
//   0       4     magic "JJX1"
//   4       1     version = 1
//   5       3     reserved, must be 0 (checked; also rounds the header to 32 B)
//   8       16    nonce = initial AES-CTR counter block (public)
//   24      4     plaintext length in bytes
//   28      4     CRC-32 (ISO-HDLC, i.e. zlib/Python binascii.crc32) of plaintext
//   32      N     ciphertext, N = plaintext length
//
//   The nonce is *derived*, not random:
//
//     nonce = MD5("JJXBOOK/ctr/v1" || nonceSeed || MD5hex(plaintext))
//
//   so encrypting the same plaintext twice yields byte-identical output (the
//   caller can use that to skip rewriting unchanged cache files) while two
//   different plaintexts never share a keystream. The construction uses nested
//   MD5 because that is the only hash already linked into JjwxcApi; see the
//   threat-model note above before relying on it for anything adversarial.
//
//   The CRC detects truncation, bit rot, a stale salt and the wrong device. It
//   is an accident detector, not a MAC. A deliberate forger cannot produce a
//   valid blob without the key, but this module makes no stronger promise.
//
// MEMORY AND COST
//   No heap, no exceptions, no RTTI, no external crypto dependency. The only
//   static data is a 256-byte AES S-box. Both operations are one-shot over
//   caller-provided buffers. A blob costs one AES-CTR pass plus one MD5 pass
//   on encrypt (decrypt needs no MD5). Throughput has NOT been measured on the
//   device; this is a straightforward byte-oriented AES, so measure before
//   wiring it into a path where latency is visible. Chapter fetches are
//   network-bound so the cost should disappear, but "should" is not a
//   measurement.
//
//   Until this is wired, `-Wl,--gc-sections` drops every symbol here from the
//   image: adding these files changes neither Flash nor RAM.
//
// WIRING (deliberately not done here)
//   When this is connected, derive key+nonceSeed once per session, then wrap
//   the per-chapter XHTML files under
//   /.crosspoint/jjwxc/<novelId>/chapters/ and the reader's section/page cache
//   with encryptBookBlob/decryptBookBlob. Read paths decrypt into the existing
//   PSRAM scratch buffer; a failed decrypt means "cache unusable" and should
//   fall back to rebuilding it from the network, never to showing the bytes.
//   The EPUB under /JJWXC/ is the user-visible artifact and stays plaintext.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

struct BookCrypto {
  static constexpr size_t kMacLen = 6;
  static constexpr size_t kSaltLen = 16;
  static constexpr size_t kKeyLen = 16;
  static constexpr size_t kNonceSeedLen = 8;
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
  static constexpr uint8_t kVersion = 1;

  // Derives the per-device key and nonce seed. Domain-separated, so key and
  // nonceSeed are not prefixes of the same digest. All pointers must be valid;
  // returns false on a null pointer.
  static bool deriveBookKey(const uint8_t mac[kMacLen], const uint8_t salt[kSaltLen], uint8_t key[kKeyLen],
                            uint8_t nonceSeed[kNonceSeedLen]);

  // Writes kHeaderLen + len bytes into `out` and sets outLen (0 on failure).
  // Returns false if `outCap` is too small, `len` exceeds uint32, or a pointer
  // is null. `out` must not overlap `plain`. Output is deterministic for a
  // given (key, nonceSeed, plaintext).
  static bool encryptBookBlob(const uint8_t key[kKeyLen], const uint8_t nonceSeed[kNonceSeedLen], const uint8_t* plain,
                              size_t len, uint8_t* out, size_t outCap, size_t& outLen);

  // Validates magic/version/reserved/length, decrypts, then verifies the CRC.
  // Sets outLen = 0 and returns false on any mismatch or short buffer. On CRC
  // failure the already-decrypted bytes are zeroed so a caller that ignores the
  // return value cannot display a wrong-key garbage page.
  //
  // `out` may equal `in` (same buffer): the ciphertext starts 32 bytes into the
  // buffer, so forward in-place decryption is safe and compacts the plaintext to
  // the front. It must not partially overlap in any other way.
  static bool decryptBookBlob(const uint8_t key[kKeyLen], const uint8_t* in, size_t inLen, uint8_t* out,
                              size_t outCap, size_t& outLen);

  // Exposed so the host test can run the FIPS-197 block known-answer test; the
  // CTR wrappers below are what production code uses. `out` may equal `in`.
  static void aes128EncryptBlock(const uint8_t key[kKeyLen], const uint8_t in[16], uint8_t out[16]);

  // AES-128-CTR over a full 128-bit big-endian counter that starts at
  // `initialCounter`. Supports in == out. Exposed for the NIST SP 800-38A
  // known-answer test and for future streaming wiring.
  static void aes128CtrXor(const uint8_t key[kKeyLen], const uint8_t initialCounter[16], const uint8_t* in, size_t len,
                           uint8_t* out);
};

}  // namespace jjwxc
