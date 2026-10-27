#include "JjwxcWap.h"

#include <stdio.h>
#include <string.h>

#include "JjwxcCrypto.h"

namespace jjwxc {
namespace {

// --------------------------------------------------------------- scanning ---

// Case-insensitive prefix match, because the page mixes `id="contentvars"` with
// uppercase tag names in places and a strict match would silently find nothing.
bool iStartsWith(const char* s, size_t len, size_t at, const char* lit) {
  const size_t n = strlen(lit);
  if (at + n > len) return false;
  for (size_t i = 0; i < n; ++i) {
    const char a = s[at + i];
    const char b = lit[i];
    const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
    const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
    if (la != lb) return false;
  }
  return true;
}

// Index of `lit` at or after `from`, case-insensitively. len on miss.
size_t findCi(const char* s, size_t len, size_t from, const char* lit) {
  const size_t n = strlen(lit);
  if (n == 0 || from >= len) return len;
  for (size_t i = from; i + n <= len; ++i) {
    if (iStartsWith(s, len, i, lit)) return i;
  }
  return len;
}

// Copies a bounded run into a NUL-terminated field, truncating rather than
// overflowing. Every field here is short and its bound is generous, so truncation
// means the page changed shape -- which then fails a later, more specific check.
void copyField(char* dst, size_t dstCap, const char* src, size_t srcLen) {
  if (dstCap == 0) return;
  const size_t n = srcLen < dstCap - 1 ? srcLen : dstCap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

uint32_t parseUint(const char* s, size_t len) {
  uint32_t v = 0;
  for (size_t i = 0; i < len; ++i) {
    if (s[i] < '0' || s[i] > '9') return 0;
    v = v * 10 + static_cast<uint32_t>(s[i] - '0');
  }
  return v;
}

// ------------------------------------------------------------- UTF-8 walk ---

// Length of the sequence starting at s[i], or 0 if it is not a valid lead byte.
// Deliberately does not validate continuation bytes beyond counting them: the
// input came out of our own AES decrypt and is either well-formed UTF-8 or the
// whole chapter is already lost, and a stricter check would only cost cycles on
// every character of every chapter.
size_t utf8Len(const uint8_t c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 0;
}

uint32_t utf8Decode(const char* s, size_t len, size_t& advance) {
  advance = 1;
  const uint8_t c0 = static_cast<uint8_t>(s[0]);
  const size_t n = utf8Len(c0);
  if (n == 0 || n > len) return 0xFFFD;
  advance = n;
  uint32_t cp = 0;
  switch (n) {
    case 1: return c0;
    case 2: cp = c0 & 0x1Fu; break;
    case 3: cp = c0 & 0x0Fu; break;
    default: cp = c0 & 0x07u; break;
  }
  for (size_t i = 1; i < n; ++i) {
    const uint8_t ci = static_cast<uint8_t>(s[i]);
    if ((ci & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (ci & 0x3Fu);
  }
  return cp;
}

size_t utf8Encode(const uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

// --------------------------------------------------------------- entities ---

// Named entities that actually occur in this content. Not a table of HTML: an
// unknown name is left verbatim, which is what a browser does and what keeps a
// stray "&" in prose from eating the following word.
struct NamedEntity {
  const char* name;  // without the leading '&' or trailing ';'
  uint32_t cp;
};
constexpr NamedEntity kNamedEntities[] = {
    {"amp", 0x26}, {"lt", 0x3C},   {"gt", 0x3E},   {"quot", 0x22}, {"apos", 0x27},
    {"nbsp", 0xA0}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F},
};

bool parseNamedEntity(const char* s, size_t len, size_t& consumed, uint32_t& cp) {
  for (const auto& e : kNamedEntities) {
    const size_t n = strlen(e.name);
    if (len < n + 2) continue;  // '&' + name + ';'
    if (memcmp(s + 1, e.name, n) != 0) continue;
    if (s[1 + n] != ';') continue;
    cp = e.cp;
    consumed = n + 2;
    return true;
  }
  return false;
}

// '&#xHHHH' / '&#NNNN', with or without the closing semicolon. The semicolon-less
// form is not a corner case here: the watermark emits `&#xe709` bare, and a decoder
// that required ';' would leave the raw text in the chapter.
bool parseNumericEntity(const char* s, size_t len, size_t& consumed, uint32_t& cp) {
  if (len < 3 || s[0] != '&' || s[1] != '#') return false;
  const bool hex = (s[2] == 'x' || s[2] == 'X');
  size_t i = hex ? 3 : 2;
  if (i >= len) return false;

  // Greedy, then back off. Browsers take up to six digits and reject an out-of-range
  // value; backing off one digit at a time matches that for every input that can
  // actually occur, and avoids rejecting `&#xe709` merely because prose follows it.
  for (size_t take = 6; take >= 1; --take) {
    if (i + take > len) continue;
    uint32_t v = 0;
    bool ok = true;
    for (size_t k = 0; k < take; ++k) {
      const char c = s[i + k];
      uint32_t d;
      if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
      else if (hex && c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
      else if (hex && c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
      else { ok = false; break; }
      v = v * (hex ? 16u : 10u) + d;
      if (v > 0x10FFFF) { ok = false; break; }
    }
    if (!ok || v == 0) continue;
    const size_t end = i + take;
    cp = v;
    consumed = (end < len && s[end] == ';') ? end + 1 : end;
    return true;
  }
  return false;
}

// ------------------------------------------------------------ CSS content: --

// One `.c_xxx:before`/`:after` rule. The page ships roughly seven of them, each
// naming two classes of which only one is used anywhere in the chapter -- the
// other is a decoy -- so the map is keyed by class and position and the decoys
// simply never get looked up.
struct CssFragment {
  char cls[4];
  bool after;
  const char* text;
  size_t len;
};
constexpr size_t kMaxCssFragments = 64;

// Finds `.c_abc:before` / `.c_abc:after` occurrences inside one selector list and
// the `content:'...'` that follows it.
size_t collectCssFragments(const char* page, size_t pageLen, CssFragment* out, size_t maxOut) {
  size_t count = 0;
  size_t at = 0;
  while (count < maxOut) {
    const size_t brace = findCi(page, pageLen, at, "{content:'");
    if (brace == pageLen) break;
    // The selector list is what precedes the brace. Bounded because walking back
    // without a limit would scan the whole page for every rule.
    size_t selStart = brace;
    const size_t floor = brace > 200 ? brace - 200 : 0;
    while (selStart > floor && page[selStart - 1] != '}' && page[selStart - 1] != '>') --selStart;

    const size_t textStart = brace + 10;  // strlen("{content:'")
    size_t textEnd = textStart;
    while (textEnd < pageLen && page[textEnd] != '\'') {
      // A CSS string may escape its own quote; keep it verbatim, matching the
      // reference implementation, which does not unescape either.
      if (page[textEnd] == '\\' && textEnd + 1 < pageLen) textEnd += 2;
      else ++textEnd;
    }
    if (textEnd >= pageLen) break;

    for (size_t i = selStart; i + 6 <= brace && count < maxOut;) {
      if (page[i] != '.' || page[i + 1] != 'c' || page[i + 2] != '_') { ++i; continue; }
      const size_t clsAt = i + 3;
      if (clsAt + 3 > pageLen) break;
      bool after = false;
      size_t next = clsAt + 3;
      if (iStartsWith(page, pageLen, next, ":after")) { after = true; next += 6; }
      else if (iStartsWith(page, pageLen, next, ":before")) { next += 7; }
      else { ++i; continue; }
      out[count].cls[0] = page[clsAt];
      out[count].cls[1] = page[clsAt + 1];
      out[count].cls[2] = page[clsAt + 2];
      out[count].cls[3] = '\0';
      out[count].after = after;
      out[count].text = page + textStart;
      out[count].len = textEnd - textStart;
      ++count;
      i = next;
    }
    at = textEnd + 1;
  }
  return count;
}

const CssFragment* findFragment(const CssFragment* frags, size_t n, const char cls[3], bool after) {
  for (size_t i = 0; i < n; ++i) {
    if (frags[i].after == after && memcmp(frags[i].cls, cls, 3) == 0) return &frags[i];
  }
  return nullptr;
}

// One pass, right to left, so each insertion only shifts text that has already
// been handled. Bounded per pass because the fragment positions are held in a
// fixed array; the caller loops until a pass finds nothing, and every pass removes
// the spans it processed, so it terminates.
constexpr size_t kSpansPerPass = 32;

struct SpanHit {
  size_t open;      // index of '<'
  size_t innerStart;
  size_t innerEnd;  // index of '<' in "</span>"
  size_t closeEnd;  // one past '>'
  char cls[3];
};

size_t collectSpans(const char* frag, size_t len, size_t from, SpanHit* out, size_t maxOut) {
  size_t count = 0;
  size_t at = from;
  while (count < maxOut) {
    const size_t open = findCi(frag, len, at, "<span class='c_");
    if (open == len) break;
    const size_t clsAt = open + 15;  // strlen("<span class='c_")
    if (clsAt + 3 + 1 >= len || frag[clsAt + 3] != '\'') { at = open + 1; continue; }
    const size_t tagEnd = findCi(frag, len, clsAt, ">");
    if (tagEnd == len) break;
    const size_t close = findCi(frag, len, tagEnd, "</span>");
    if (close == len) break;
    out[count].open = open;
    out[count].innerStart = tagEnd + 1;
    out[count].innerEnd = close;
    out[count].closeEnd = close + 7;
    out[count].cls[0] = frag[clsAt];
    out[count].cls[1] = frag[clsAt + 1];
    out[count].cls[2] = frag[clsAt + 2];
    ++count;
    at = out[count - 1].closeEnd;
  }
  return count;
}

// ------------------------------------------------------------------- JSON ---

// cryptInfo decrypts to a second Base64 layer whose payload is a fixed three-field
// object. Scanning for the keys rather than running the streaming parser keeps this
// dependency-free and costs nothing: the object is ~50 bytes and its shape is
// pinned by the version check.
bool jsonFindString(const char* json, size_t len, const char* key, char* out, size_t outCap) {
  const size_t k = findCi(json, len, 0, key);
  if (k == len) return false;
  size_t i = k + strlen(key);
  while (i < len && (json[i] == ' ' || json[i] == '"')) ++i;
  if (i >= len || json[i] != ':') return false;
  ++i;
  while (i < len && (json[i] == ' ' || json[i] == '"')) ++i;
  const size_t start = i;
  while (i < len && json[i] != '"' && json[i] != ',' && json[i] != '}') ++i;
  copyField(out, outCap, json + start, i - start);
  return out[0] != '\0';
}

bool jsonFindNumber(const char* json, size_t len, const char* key, uint64_t& out) {
  const size_t k = findCi(json, len, 0, key);
  if (k == len) return false;
  size_t i = k + strlen(key);
  while (i < len && (json[i] == ' ' || json[i] == '"')) ++i;
  if (i >= len || json[i] != ':') return false;
  ++i;
  while (i < len && json[i] == ' ') ++i;
  uint64_t v = 0;
  size_t digits = 0;
  while (i < len && json[i] >= '0' && json[i] <= '9') {
    v = v * 10 + static_cast<uint64_t>(json[i] - '0');
    ++i;
    ++digits;
  }
  if (digits == 0) return false;
  out = v;
  return true;
}

void rotateHex32(const char hex[32], uint32_t n, char out[33]) {
  n %= 32;
  memcpy(out, hex + n, 32 - n);
  memcpy(out + (32 - n), hex, n);
  out[32] = '\0';
}

// What Python's str.strip() removes, which is what the reference implementation
// ends with. U+3000 matters here specifically: it is how this content indents a
// paragraph, so a trim that only knew ASCII whitespace would leave two of them on
// the front of every chapter and the port would differ from the reference by six
// bytes that look like nothing.
bool isUnicodeSpace(const uint32_t cp) {
  if (cp <= 0x7F) return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x0B || cp == 0x0C;
  switch (cp) {
    case 0x00A0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000: return true;
    default: break;
  }
  return cp >= 0x2000 && cp <= 0x200A;
}

}  // namespace

const char* wapStatusText(const WapStatus s) {
  switch (s) {
    case WapStatus::Ok: return "ok";
    case WapStatus::MissingVars: return "contentvars missing or incomplete";
    case WapStatus::JsVerMismatch: return "algorithm version changed";
    case WapStatus::BadBase64: return "malformed base64";
    case WapStatus::Decrypt: return "decryption failed";
    case WapStatus::BadCryptInfo: return "cryptInfo was not the expected JSON";
    case WapStatus::Buffer: return "buffer too small";
  }
  return "unknown";
}

bool parseWapVars(const char* page, const size_t pageLen, WapVars& out) {
  out = WapVars{};
  if (page == nullptr || pageLen == 0) return false;

  // Restrict the input scan to the contentvars div. Outside it the page carries
  // other forms, and a `name="content"` field belonging to one of those would be
  // indistinguishable from the real one.
  size_t lo = 0;
  size_t hi = pageLen;
  const size_t div = findCi(page, pageLen, 0, "id=\"contentvars\"");
  if (div != pageLen) {
    lo = div;
    const size_t close = findCi(page, pageLen, div, "</div>");
    if (close != pageLen) hi = close;
  }

  // Walks `<input ... name="X" ... value="Y">` inside [lo, hi). Attribute order is
  // not fixed, so name and value are located independently within each tag.
  size_t at = lo;
  while (at < hi) {
    const size_t tag = findCi(page, hi, at, "<input");
    if (tag == hi) break;
    size_t tagEnd = tag;
    while (tagEnd < hi && page[tagEnd] != '>') ++tagEnd;
    if (tagEnd >= hi) break;

    const size_t nameAt = findCi(page, tagEnd, tag, "name=\"");
    const size_t valueAt = findCi(page, tagEnd, tag, "value=\"");
    if (nameAt < tagEnd && valueAt < tagEnd) {
      const size_t ns = nameAt + 6;
      size_t ne = ns;
      while (ne < tagEnd && page[ne] != '"') ++ne;
      const size_t vs = valueAt + 7;
      size_t ve = vs;
      while (ve < tagEnd && page[ve] != '"') ++ve;
      const size_t nameLen = ne - ns;
      const char* name = page + ns;
      const char* value = page + vs;
      const size_t valueLen = ve - vs;

      // First occurrence wins, matching the reference: a repeated name outside the
      // div must not overwrite the real field.
      if (nameLen == 7 && memcmp(name, "novelid", 7) == 0 && out.novelId == 0) {
        out.novelId = parseUint(value, valueLen);
      } else if (nameLen == 9 && memcmp(name, "chapterid", 9) == 0 && out.chapterId == 0) {
        out.chapterId = parseUint(value, valueLen);
      } else if (nameLen == 8 && memcmp(name, "readerid", 8) == 0 && out.readerId == 0) {
        out.readerId = parseUint(value, valueLen);
      } else if (nameLen == 9 && memcmp(name, "accessKey", 9) == 0 && out.accessKey[0] == '\0') {
        copyField(out.accessKey, sizeof(out.accessKey), value, valueLen);
      } else if (nameLen == 5 && memcmp(name, "jsver", 5) == 0 && out.jsVer[0] == '\0') {
        copyField(out.jsVer, sizeof(out.jsVer), value, valueLen);
      } else if (nameLen == 4 && memcmp(name, "type", 4) == 0 && out.type[0] == '\0') {
        copyField(out.type, sizeof(out.type), value, valueLen);
      } else if (nameLen == 9 && memcmp(name, "cryptInfo", 9) == 0 && out.cryptInfo == nullptr) {
        out.cryptInfo = value;
        out.cryptInfoLen = valueLen;
      } else if (nameLen == 7 && memcmp(name, "content", 7) == 0 && out.content == nullptr) {
        out.content = value;
        out.contentLen = valueLen;
      }
    }
    at = tagEnd + 1;
  }
  return out.complete();
}

uint32_t wapCharCodeSum(const char* s, const size_t len) {
  uint32_t total = 0;
  for (size_t i = 0; i < len;) {
    size_t advance = 1;
    const uint32_t cp = utf8Decode(s + i, len - i, advance);
    i += advance;
    if (cp > 0xFFFF) {
      // JavaScript would see a surrogate pair here, and charCodeAt sums both halves.
      const uint32_t v = cp - 0x10000;
      total += 0xD800 + (v >> 10);
      total += 0xDC00 + (v & 0x3FF);
    } else {
      total += cp;
    }
  }
  return total;
}

void wapStage1KeyIv(const uint32_t novelId, const uint32_t chapterId, const uint32_t readerId, const char* accessKey,
                    const int scheme, char keyOut[17], char ivOut[17]) {
  char seed[96];
  const int n = (scheme == 1)
                    ? snprintf(seed, sizeof(seed), "%s-%u-%u-%u", accessKey, (unsigned)novelId, (unsigned)chapterId,
                               (unsigned)readerId)
                    : snprintf(seed, sizeof(seed), "%u.%u.%u.%s", (unsigned)novelId, (unsigned)chapterId,
                               (unsigned)readerId, accessKey);
  char digest[Md5::kHexLen + 1];
  Md5::hex(reinterpret_cast<const uint8_t*>(seed), static_cast<size_t>(n > 0 ? n : 0), digest);

  // Scheme B rotates modulo 33, scheme A modulo 32 -- an off-by-one in the service's
  // own source, reproduced here because the server derives its key the same way.
  const uint32_t acc = wapCharCodeSum(accessKey, strlen(accessKey));
  char rot[33];
  rotateHex32(digest, acc % (scheme == 1 ? 33u : 32u), rot);

  // The service swaps key and iv after slicing, and the swap is unconditional
  // (`if (md5.charCodeAt())`, which is always truthy), so what reaches AES is
  // rot[16:32] as the key and rot[0:16] as the iv.
  memcpy(keyOut, rot + 16, 16);
  keyOut[16] = '\0';
  memcpy(ivOut, rot, 16);
  ivOut[16] = '\0';
}

void wapStage2KeyIv(const char* infoKey, const uint64_t infoTime, const uint32_t readerId, const char* accessKey,
                    char keyOut[17], char ivOut[17]) {
  char seed[64];
  // JavaScript string+number concatenation: no separator, time and readerId in
  // decimal. Getting the order wrong yields a valid-looking key and garbage text.
  const int n = snprintf(seed, sizeof(seed), "%s%llu%u", infoKey, static_cast<unsigned long long>(infoTime),
                         (unsigned)readerId);
  char digest[Md5::kHexLen + 1];
  Md5::hex(reinterpret_cast<const uint8_t*>(seed), static_cast<size_t>(n > 0 ? n : 0), digest);

  const uint32_t acc = wapCharCodeSum(accessKey, strlen(accessKey));
  char rot[33];
  rotateHex32(digest, acc % 32u, rot);
  // No swap in this stage -- the asymmetry with stage 1 is real and deliberate.
  memcpy(keyOut, rot, 16);
  keyOut[16] = '\0';
  memcpy(ivOut, rot + 16, 16);
  ivOut[16] = '\0';
}

WapStatus wapDecryptCryptInfo(const WapVars& v, char* work, const size_t workCap, WapCryptInfo& out) {
  out = WapCryptInfo{};
  if (work == nullptr || workCap == 0) return WapStatus::Buffer;

  char key[17], iv[17];
  wapStage1KeyIv(v.novelId, v.chapterId, v.readerId, v.accessKey, static_cast<int>((v.novelId + v.chapterId) % 2), key,
                 iv);

  const int32_t ctLen = Base64::decode(v.cryptInfo, v.cryptInfoLen, reinterpret_cast<uint8_t*>(work), workCap);
  if (ctLen < 0) return WapStatus::BadBase64;
  const int32_t plainLen = Aes128::cbcDecryptInPlace(reinterpret_cast<const uint8_t*>(key),
                                                     reinterpret_cast<const uint8_t*>(iv),
                                                     reinterpret_cast<uint8_t*>(work), static_cast<size_t>(ctLen));
  if (plainLen < 0) return WapStatus::Decrypt;

  // The plaintext is itself Base64; decode it over the same buffer, which is safe
  // because decoding only ever shrinks.
  const int32_t jsonLen =
      Base64::decode(work, static_cast<size_t>(plainLen), reinterpret_cast<uint8_t*>(work), workCap);
  if (jsonLen < 0) return WapStatus::BadBase64;
  work[jsonLen] = '\0';

  char keyStr[16] = {};
  char verStr[16] = {};
  uint64_t time = 0;
  // Quoted keys: the JSON is machine-generated and always quotes them.
  if (!jsonFindString(work, static_cast<size_t>(jsonLen), "\"key\"", keyStr, sizeof(keyStr))) return WapStatus::BadCryptInfo;
  if (!jsonFindNumber(work, static_cast<size_t>(jsonLen), "\"time\"", time)) return WapStatus::BadCryptInfo;
  jsonFindString(work, static_cast<size_t>(jsonLen), "\"ver\"", verStr, sizeof(verStr));

  out.time = time;
  copyField(out.key, sizeof(out.key), keyStr, strlen(keyStr));
  copyField(out.ver, sizeof(out.ver), verStr, strlen(verStr));
  return WapStatus::Ok;
}

WapStatus wapDecryptContent(const WapVars& v, const WapCryptInfo& info, char* body, const size_t bodyCap,
                            size_t& outLen) {
  outLen = 0;
  if (body == nullptr || bodyCap == 0) return WapStatus::Buffer;

  char key[17], iv[17];
  wapStage2KeyIv(info.key, info.time, v.readerId, v.accessKey, key, iv);

  // Base64 decoding shrinks by 25%, so decoding into the front of `body` and then
  // decrypting in place never needs a second buffer the size of the ciphertext.
  const int32_t ctLen =
      Base64::decode(v.content, v.contentLen, reinterpret_cast<uint8_t*>(body), bodyCap);
  if (ctLen < 0) return WapStatus::BadBase64;
  const int32_t plainLen = Aes128::cbcDecryptInPlace(reinterpret_cast<const uint8_t*>(key),
                                                     reinterpret_cast<const uint8_t*>(iv),
                                                     reinterpret_cast<uint8_t*>(body), static_cast<size_t>(ctLen));
  if (plainLen < 0) return WapStatus::Decrypt;
  outLen = static_cast<size_t>(plainLen);
  body[outLen] = '\0';
  return WapStatus::Ok;
}

size_t wapUndoCssDisplacement(char* frag, const size_t fragLen, const size_t fragCap, const char* page,
                              const size_t pageLen) {
  CssFragment frags[kMaxCssFragments];
  const size_t fragCount = collectCssFragments(page, pageLen, frags, kMaxCssFragments);
  if (fragCount == 0) return fragLen;

  size_t len = fragLen;
  SpanHit hits[kSpansPerPass];
  // Repeated passes because the hit array is fixed-size and a chapter can carry more
  // displaced spans than it holds. Each pass consumes the spans it processes, so the
  // loop terminates; right-to-left order means an insertion only ever shifts bytes
  // that this pass has already finished with.
  for (;;) {
    const size_t found = collectSpans(frag, len, 0, hits, kSpansPerPass);
    if (found == 0) break;
    for (size_t idx = found; idx-- > 0;) {
      const SpanHit h = hits[idx];
      const CssFragment* before = findFragment(frags, fragCount, h.cls, false);
      const CssFragment* after = findFragment(frags, fragCount, h.cls, true);
      const size_t bl = before ? before->len : 0;
      const size_t al = after ? after->len : 0;
      const size_t inner = h.innerEnd - h.innerStart;
      const size_t oldLen = h.closeEnd - h.open;
      const size_t newLen = bl + inner + al;
      const size_t tailLen = len - h.closeEnd;
      if (len - oldLen + newLen > fragCap) {
        // No room to grow. Leaving this span wrapped is wrong but visible -- the tag
        // stripper removes the markup and the reader loses a few characters -- while
        // writing past fragCap would corrupt whatever the caller put after it.
        continue;
      }
      if (newLen > oldLen) {
        // Grow: move the tail out of the way before writing anything into the gap.
        memmove(frag + h.open + newLen, frag + h.closeEnd, tailLen);
      }
      // The inner text moves FIRST. The displaced fragment it is being wrapped in is
      // routinely longer than the 20-byte tag it replaces -- one span in the captured
      // page has a 12-character (36-byte) `:before` -- so writing that fragment first
      // would run past innerStart and corrupt the very bytes about to be moved. The
      // reference implementation builds a new string and cannot hit this.
      memmove(frag + h.open + bl, frag + h.innerStart, inner);
      // The CSS text lives in `page`, never in `frag`, so these two are plain copies.
      if (bl != 0) memcpy(frag + h.open, before->text, bl);
      if (al != 0) memcpy(frag + h.open + bl + inner, after->text, al);
      if (newLen < oldLen) memmove(frag + h.open + newLen, frag + h.closeEnd, tailLen);
      len = len - oldLen + newLen;
    }
  }
  return len;
}

size_t wapDecodeEntities(char* s, size_t len) {
  size_t r = 0, w = 0;
  while (r < len) {
    if (s[r] != '&') {
      s[w++] = s[r++];
      continue;
    }
    uint32_t cp = 0;
    size_t consumed = 0;
    if (parseNumericEntity(s + r, len - r, consumed, cp) || parseNamedEntity(s + r, len - r, consumed, cp)) {
      char enc[4];
      const size_t n = utf8Encode(cp, enc);
      if (w + n > len) {
        // Cannot happen: every entity is at least 3 source bytes and encodes to at
        // most 4, and the numeric forms that reach 4 bytes are 8+ source bytes.
        return w;
      }
      memcpy(s + w, enc, n);
      w += n;
      r += consumed;
      continue;
    }
    s[w++] = s[r++];
  }
  return w;
}

size_t wapApplyPuaMap(char* s, const size_t len, const PuaLookup pua, void* ctx, uint32_t& total,
                      uint32_t& unresolved) {
  total = 0;
  unresolved = 0;
  size_t r = 0, w = 0;
  while (r < len) {
    size_t advance = 1;
    const uint32_t cp = utf8Decode(s + r, len - r, advance);
    const bool zeroWidth = (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF);
    if (zeroWidth) {
      r += advance;
      continue;
    }
    if (cp >= 0xE000 && cp <= 0xF8FF) {
      ++total;
      char mapped[4];
      const size_t n = pua ? pua(ctx, cp, mapped) : 0;
      if (n == 0) {
        // Reported, never silently dropped: an unmapped code point means the font
        // changed, and the reader would otherwise see a box with no explanation.
        ++unresolved;
        mapped[0] = static_cast<char>(0xEF);
        mapped[1] = static_cast<char>(0xBF);
        mapped[2] = static_cast<char>(0xBD);
        if (w + 3 <= len) {
          memcpy(s + w, mapped, 3);
          w += 3;
        }
      } else if (w + n <= len) {
        memcpy(s + w, mapped, n);
        w += n;
      }
      r += advance;
      continue;
    }
    if (w + advance <= len) {
      memmove(s + w, s + r, advance);
      w += advance;
    }
    r += advance;
  }
  return w;
}

size_t wapHtmlToText(char* frag, size_t fragLen) {
  size_t w = 0;
  for (size_t r = 0; r < fragLen;) {
    if (frag[r] == '<') {
      size_t e = r;
      while (e < fragLen && frag[e] != '>') ++e;
      if (e >= fragLen) {  // unterminated: treat the rest as text
        frag[w++] = frag[r++];
        continue;
      }
      const size_t tagLen = e - r;
      const bool isBr = (tagLen >= 3 && iStartsWith(frag, fragLen, r + 1, "br"));
      const bool isCloseP = (tagLen >= 3 && iStartsWith(frag, fragLen, r + 1, "/p"));
      if (isBr || isCloseP) frag[w++] = '\n';
      // Every other tag -- the <div> wrappers, the leftover markup -- disappears.
      r = e + 1;
      continue;
    }
    const char c = frag[r];
    if (c == '\r') {
      frag[w++] = '\n';
      ++r;
      if (r < fragLen && frag[r] == '\n') ++r;
      continue;
    }
    frag[w++] = c;
    ++r;
  }

  // Trailing spaces/tabs before a newline, then collapse 3+ newlines to 2. Done in
  // one pass over what the tag strip produced rather than as two more sweeps.
  size_t v = 0;
  size_t run = 0;
  for (size_t i = 0; i < w;) {
    if (frag[i] == '\n') {
      // Drop the horizontal whitespace that preceded it.
      while (v > 0 && (frag[v - 1] == ' ' || frag[v - 1] == '\t')) --v;
      ++run;
      if (run <= 2) frag[v++] = '\n';
      ++i;
      continue;
    }
    run = 0;
    frag[v++] = frag[i++];
  }
  // Trim the ends only. Interior U+3000 indentation is meaningful prose and the
  // reference keeps it; only the leading and trailing whitespace goes.
  size_t start = 0;
  while (start < v) {
    size_t advance = 1;
    if (!isUnicodeSpace(utf8Decode(frag + start, v - start, advance))) break;
    start += advance;
  }
  size_t end = v;
  while (end > start) {
    // Step back over continuation bytes to the lead byte of the last sequence.
    size_t b = end;
    while (b > start && (static_cast<uint8_t>(frag[b - 1]) & 0xC0) == 0x80) --b;
    if (b == start) break;
    --b;
    size_t advance = 1;
    if (!isUnicodeSpace(utf8Decode(frag + b, end - b, advance))) break;
    end = b;
  }
  if (start > 0) memmove(frag, frag + start, end - start);
  return end - start;
}

WapResult wapPageToText(const char* page, const size_t pageLen, const PuaLookup pua, void* puaCtx,
                        WapBuffers& bufs) {
  WapResult res;
  WapVars vars;
  if (!parseWapVars(page, pageLen, vars)) {
    res.status = WapStatus::MissingVars;
    return res;
  }
  copyField(res.pageJsVer, sizeof(res.pageJsVer), vars.jsVer, strlen(vars.jsVer));
  if (vars.jsVer[0] != '\0' && strcmp(vars.jsVer, kWapJsVer) != 0) {
    res.status = WapStatus::JsVerMismatch;
    return res;
  }
  if (bufs.work == nullptr || bufs.body == nullptr || bufs.text == nullptr) {
    res.status = WapStatus::Buffer;
    return res;
  }

  WapCryptInfo info;
  res.status = wapDecryptCryptInfo(vars, bufs.work, bufs.workCap, info);
  if (res.status != WapStatus::Ok) return res;
  if (info.ver[0] != '\0' && strcmp(info.ver, kWapJsVer) != 0) {
    res.status = WapStatus::JsVerMismatch;
    copyField(res.pageJsVer, sizeof(res.pageJsVer), info.ver, strlen(info.ver));
    return res;
  }

  size_t bodyLen = 0;
  res.status = wapDecryptContent(vars, info, bufs.body, bufs.bodyCap - 1, bodyLen);
  if (res.status != WapStatus::Ok) return res;

  bodyLen = wapUndoCssDisplacement(bufs.body, bodyLen, bufs.bodyCap - 1, page, pageLen);
  bodyLen = wapDecodeEntities(bufs.body, bodyLen);
  bodyLen = wapApplyPuaMap(bufs.body, bodyLen, pua, puaCtx, res.puaTotal, res.puaUnresolved);
  bodyLen = wapHtmlToText(bufs.body, bodyLen);

  if (bodyLen + 1 > bufs.textCap) {
    res.status = WapStatus::Buffer;
    return res;
  }
  memcpy(bufs.text, bufs.body, bodyLen);
  bufs.text[bodyLen] = '\0';
  res.textLen = bodyLen;
  return res;
}

}  // namespace jjwxc
