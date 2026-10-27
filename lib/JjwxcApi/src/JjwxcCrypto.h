// Self-contained MD5 + DES-CBC/PKCS7 + JJWXC dynamic-envelope decoding.
// No external crypto dependency: wolfSSL's DES may be disabled by build flags,
// and this keeps the Flash cost predictable (~4 KB).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

// ---------------------------------------------------------------- MD5 ------
struct Md5 {
  static constexpr size_t kDigestLen = 16;
  static constexpr size_t kHexLen = 32;

  // One-shot. `hexOut` must hold kHexLen+1 bytes; lowercase hex, no NUL padding
  // beyond the terminator.
  static void hex(const uint8_t* data, size_t len, char hexOut[kHexLen + 1]);
};

// ---------------------------------------------------------------- DES ------
// DES-CBC with PKCS#7 padding, 8-byte key and IV. Matches the reference
// implementation in exp/jjapp.py (verified against live JJWXC responses).
struct Des {
  static constexpr size_t kBlockSize = 8;
  static constexpr size_t kKeyLen = 8;

  // Decrypts `inLen` bytes (must be a multiple of kBlockSize) in place-safe
  // fashion into `out`. Strips PKCS#7 padding. Returns plaintext length, or -1
  // if `inLen` is not block-aligned or padding is invalid.
  static int32_t cbcDecrypt(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen],
                            const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap);

  // Encrypts with PKCS#7 padding. `outCap` must be >= inLen + kBlockSize.
  // Returns ciphertext length.
  static size_t cbcEncrypt(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen],
                           const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap);

  // In-place CBC decrypt with PKCS#7 strip. Each block's ciphertext is saved
  // before the block is overwritten, so `buf` may be decrypted over itself.
  // Returns plaintext length, or -1 on bad alignment/padding.
  static int32_t cbcDecryptInPlace(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen],
                                   uint8_t* buf, size_t bufLen);
};

// ---------------------------------------------------------------- AES ------
// AES-128-CBC, decrypt only, PKCS#7. The WAP VIP pages key their body with this
// (CryptoJS.AES.decrypt, which defaults to CBC+PkCS7 and -- because the
// ciphertext does not start with "Salted__" -- applies no KDF and no salt, so the
// key and IV are the raw 16 UTF-8 bytes the page's own KDF produced).
//
// Decrypt-only on purpose: nothing in this project encrypts with AES, and dropping
// the forward direction keeps the code and its tables to roughly half. wolfSSL's
// aes.c is already linked into the firmware image, but reaching for it would put a
// wolfSSL include in this library, which is what keeps JjwxcApi compilable and
// testable on a host.
struct Aes128 {
  static constexpr size_t kBlockSize = 16;
  static constexpr size_t kKeyLen = 16;

  // Decrypts `inLen` bytes (a multiple of kBlockSize) into `out` and strips PKCS#7
  // padding. Returns the plaintext length, or -1 on bad alignment or padding.
  // `out` and `in` must not overlap; use cbcDecryptInPlace to work over a buffer.
  static int32_t cbcDecrypt(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen], const uint8_t* in, size_t inLen,
                            uint8_t* out, size_t outCap);

  // Same, overwriting `buf`. Each block's ciphertext is copied out before the block
  // is decrypted, so the CBC chain survives. That matters here: the caller's buffer
  // is a slice of one shared PSRAM scratch region, and a 36 KB VIP body has nowhere
  // else to go.
  static int32_t cbcDecryptInPlace(const uint8_t key[kKeyLen], const uint8_t iv[kKeyLen], uint8_t* buf, size_t bufLen);
};

// ------------------------------------------------------------- Base64 ------
struct Base64 {
  // Standard alphabet with '=' padding. Returns decoded length, or -1 on
  // malformed input. Tolerates a missing padding tail.
  //
  // Overlap-safe: after consuming k input chars at most 3k/4 output bytes have
  // been written, so `out` may point at or before `in` within the same buffer.
  // That lets an envelope body be Base64-decoded over itself.
  static int32_t decode(const char* in, size_t inLen, uint8_t* out, size_t outCap);

  // Returns encoded length written (excluding NUL). `outCap` must be >=
  // 4*ceil(inLen/3)+1.
  static size_t encode(const uint8_t* in, size_t inLen, char* out, size_t outCap);
};

// ------------------------------------------------------------ Envelope -----
// JJWXC's "dynamic envelope": for some chapterContent responses the *entire*
// body is one Base64 DES ciphertext whose key is derived per-response from the
// `accesskey` / `keystring` HTTP response headers.
//
// Derivation (verified against live traffic, see docs/00-feasibility-report.md
// section 5.2):
//   sum     = Σ bytes(accesskey)
//   start   = sum % len(keystring)
//   finish  = min(len(keystring), start + sum / 65)
//   keypart = keystring[start:finish]
//   if last byte of accesskey is odd: salt = body[-12:], cipher = body[:-12]
//   else:                             salt = body[:12],  cipher = body[12:]
//   key = md5hex(keypart + salt)[:8]
//   iv  = md5hex(salt)[:8]
//   plaintext = des_cbc_pkcs7_decrypt(base64decode(cipher), key, iv)
struct Envelope {
  // Writes the decoded JSON text into `out` (NUL-terminated).
  // Returns true on success. `body` is the raw response text.
  static bool decode(const char* body, size_t bodyLen, const char* accessKey,
                     const char* keyString, char* out, size_t outCap);

  // In-place variant for memory-constrained targets: `body` is overwritten with
  // the decoded JSON (NUL-terminated) and `*outLen` set to its length. Safe
  // because Base64 decoding shrinks by 25% (reads always run ahead of writes)
  // and CBC decryption only needs the previous ciphertext block, which is saved
  // before each block is overwritten. The 12-byte salt is copied out first.
  static bool decodeInPlace(char* body, size_t bodyLen, const char* accessKey,
                            const char* keyString, size_t* outLen);

  // Exposed for tests: fills key/iv (8 chars each, NUL-terminated).
  static bool deriveKeyIv(const char* accessKey, const char* keyString,
                          const char* body, size_t bodyLen, char keyOut[9],
                          char ivOut[9], const char** cipherStart,
                          size_t* cipherLen);
};

// ------------------------------------------------------------ Streaming ----

// Sink for decoded UTF-8 text. Return false to abort the operation.
using TextSink = bool (*)(void* ctx, const char* utf8, size_t len);

// Incrementally turns a Base64 DES-CBC/PKCS7 stream into plaintext, emitting
// through `sink` as blocks complete. Accepts arbitrary chunk boundaries, which
// matters because StreamingJsonParser delivers long strings in
// TOKEN_BUF_SIZE-1 (2047 byte) pieces that are not Base64-aligned.
//
// Lives here rather than in the protocol layer so the DES key schedule is
// expanded once per chapter instead of once per 8-byte block. Memory is O(1):
// roughly 40 bytes of state regardless of payload size.
class DesBase64Stream {
 public:
  void begin(const uint8_t key[Des::kKeyLen], const uint8_t iv[Des::kKeyLen], TextSink sink, void* ctx);
  bool feed(const char* base64, size_t len);
  // Validates and strips PKCS#7 padding, then emits the final block.
  bool finish();
  size_t written() const { return totalOut_; }
  void cancel() { cancelled_ = true; }

  // Opaque storage for the internal DES key schedule (16 rounds x 6 bytes).
  // Kept inline so the stream needs no heap at all.
  static constexpr size_t kScheduleBytes = 16 * 6;

 private:
  bool flushCipherBlock();

  alignas(8) uint8_t schedule_[kScheduleBytes] = {};

  uint8_t prev_[8] = {};  // CBC chaining: previous ciphertext block
  uint8_t block_[8] = {};
  size_t blockLen_ = 0;
  uint32_t b64Acc_ = 0;
  uint8_t b64Bits_ = 0;
  size_t totalOut_ = 0;
  uint8_t lastPlain_[8] = {};
  bool haveLast_ = false;
  TextSink sink_ = nullptr;
  void* ctx_ = nullptr;
  bool started_ = false;
  bool cancelled_ = false;
};

}  // namespace jjwxc
