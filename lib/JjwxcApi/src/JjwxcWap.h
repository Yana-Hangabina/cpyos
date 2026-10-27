// JJWXC WAP (m.jjwxc.net) VIP chapter decoding.
//
// A WAP VIP page carries its body in <div id="contentvars"> as hidden inputs:
// the ciphertext, a per-page accessKey, an encrypted cryptInfo blob, and the
// reader id. Getting prose out of it takes two AES-128-CBC decryptions whose keys
// are derived from those fields, then three text repairs the page's own JavaScript
// would otherwise do in a browser: numeric HTML entities (including the
// semicolon-less form), CSS `content:` displacement, and the PUA font watermark.
//
// The reference implementation is exp/jjvip_decrypt.py, verified against the live
// service; this is a port of it, and src/JjwxcWapTest.cpp pins both to the same
// captured page.
//
// Two rules this file exists to enforce:
//
//   - No heap. Every buffer is caller-owned. The decrypted body of one chapter is
//     tens of kilobytes and the app layer keeps exactly one PSRAM scratch region,
//     sliced up per stage; internal RAM has ~50 KB free and the display needs 45 KB
//     of it contiguous.
//   - A version mismatch is a hard stop, never a warning. `jsver` gates the key
//     derivation. If JJWXC changes it, decoding still "succeeds" and yields text
//     that looks plausible and is wrong, which on an e-reader is indistinguishable
//     from the author's prose. Refusing is the only safe answer.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

// The algorithm version this build implements, compared against the page's own
// `jsver` field and against the `ver` inside cryptInfo.
constexpr const char* kWapJsVer = "20250926";

enum class WapStatus : uint8_t {
  Ok = 0,
  MissingVars,     // contentvars absent or incomplete: not logged in, or not purchased
  JsVerMismatch,   // the service changed the algorithm; the caller must say so
  BadBase64,
  Decrypt,         // AES failed, which in practice means a wrong readerId or accessKey
  BadCryptInfo,    // cryptInfo decrypted but was not the expected JSON
  Buffer,          // a caller buffer was too small
};

const char* wapStatusText(WapStatus s);

// The contentvars fields, as offsets into the page rather than copies: `content`
// alone is ~49 KB of Base64 and copying it would need a second buffer the size of
// the ciphertext for no benefit. The page must outlive this struct.
struct WapVars {
  uint32_t novelId = 0;
  uint32_t chapterId = 0;
  // Present in the page itself, so the reader id does not have to be recovered from
  // the sid cookie. It is the account's own number and it feeds the stage-2 key.
  uint32_t readerId = 0;
  char accessKey[24] = {};
  char jsVer[16] = {};
  char type[8] = {};
  const char* cryptInfo = nullptr;
  size_t cryptInfoLen = 0;
  const char* content = nullptr;
  size_t contentLen = 0;

  bool complete() const {
    return novelId != 0 && chapterId != 0 && readerId != 0 && accessKey[0] != '\0' && cryptInfo != nullptr &&
           cryptInfoLen != 0 && content != nullptr && contentLen != 0;
  }
};

// Decoded cryptInfo: {"time":<unix>,"key":"<6-8 chars>","ver":"<jsver>"}.
struct WapCryptInfo {
  uint64_t time = 0;
  char key[16] = {};
  char ver[16] = {};
};

// PUA watermark resolution, injected so this file needs no font parsing and the
// font map stays independently testable. Returns the UTF-8 length written to `out`
// (1-4 bytes, not NUL-terminated), or 0 when the code point is not in the map.
//
// A 0 return is counted and reported, never papered over: an unmapped code point
// means the font changed, and emitting U+FFFD or the raw PUA would show the reader
// a box they cannot distinguish from a rare character.
using PuaLookup = size_t (*)(void* ctx, uint32_t codePoint, char out[4]);

// Caller-owned scratch. The stages do not overlap in lifetime, so `work` and `body`
// may be slices of one region; `text` must be separate because it is what survives.
struct WapBuffers {
  char* work = nullptr;
  size_t workCap = 0;  // >= cryptInfoLen (Base64 decode of it, plus the JSON)
  char* body = nullptr;
  size_t bodyCap = 0;  // >= 3/4 * contentLen, the decrypted HTML fragment
  char* text = nullptr;
  size_t textCap = 0;  // final UTF-8 prose, NUL-terminated on success
};

struct WapResult {
  WapStatus status = WapStatus::Ok;
  size_t textLen = 0;
  uint32_t puaTotal = 0;
  uint32_t puaUnresolved = 0;
  // Set when the page's jsver or cryptInfo's ver disagreed with kWapJsVer. Reported
  // separately from status because a mismatch is the one failure worth showing the
  // user verbatim: it means "upgrade the firmware", not "try again".
  char pageJsVer[16] = {};
};

// Parses <div id="contentvars"> out of a UTF-8 page. The page must already have
// been transcoded from GBK; that happens in the app layer, which is where the
// firmware's transcoder lives.
bool parseWapVars(const char* page, size_t pageLen, WapVars& out);

// Stage 1: cryptInfo -> WapCryptInfo. `work` holds the Base64 decode and the JSON.
WapStatus wapDecryptCryptInfo(const WapVars& v, char* work, size_t workCap, WapCryptInfo& out);

// Stage 2: content -> HTML fragment written to `body`.
WapStatus wapDecryptContent(const WapVars& v, const WapCryptInfo& info, char* body, size_t bodyCap, size_t& outLen);

// Whole chain: parse, check jsver, decrypt twice, undo the CSS displacement, decode
// entities, resolve the watermark, strip tags, normalise newlines.
WapResult wapPageToText(const char* page, size_t pageLen, PuaLookup pua, void* puaCtx, WapBuffers& bufs);

// --- exposed for tests -------------------------------------------------------
// The key derivation is the part that cannot be debugged from its output: a wrong
// rotation or a swapped key/IV yields plausible-looking garbage, so the
// intermediates are pinned directly instead.

// Sum of UTF-16 code units, matching JavaScript's charCodeAt accumulation.
uint32_t wapCharCodeSum(const char* s, size_t len);

// scheme 0 = A, 1 = B, from (novelId + chapterId) % 2. Fills two 16-char keys.
void wapStage1KeyIv(uint32_t novelId, uint32_t chapterId, uint32_t readerId, const char* accessKey, int scheme,
                    char keyOut[17], char ivOut[17]);
void wapStage2KeyIv(const char* infoKey, uint64_t infoTime, uint32_t readerId, const char* accessKey, char keyOut[17],
                    char ivOut[17]);

// Repairs a decrypted HTML fragment in place. Each returns the new length.
// `wapUndoCssDisplacement` INSERTS the displaced text, so it needs `fragCap` to be
// larger than `fragLen` -- a few hundred bytes of slack covers a whole chapter.
size_t wapUndoCssDisplacement(char* frag, size_t fragLen, size_t fragCap, const char* page, size_t pageLen);
size_t wapDecodeEntities(char* s, size_t len);
size_t wapApplyPuaMap(char* s, size_t len, PuaLookup pua, void* ctx, uint32_t& total, uint32_t& unresolved);
size_t wapHtmlToText(char* frag, size_t fragLen);

}  // namespace jjwxc
