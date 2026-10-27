#include "JjwxcProtocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <new>
#include <string.h>
#include <time.h>

namespace jjwxc {
namespace {

// ------------------------------------------------------------- helpers -----

bool keyIs(const char* key, size_t len, const char* name) {
  return strlen(name) == len && memcmp(key, name, len) == 0;
}

// Copies one JSON string field into a fixed buffer, NUL-terminating it.
//
// The truncation backs off to a code-point boundary. Cutting at `dstSize - 1`
// bytes instead leaves half a UTF-8 sequence in the buffer whenever a Chinese
// title, tag or class name is longer than the field -- 3-byte characters make that
// a 2-in-3 chance at the boundary, not a corner case -- and every one of these
// buffers ends up handed straight to renderer.drawText(), which would emit the
// orphaned lead byte as a garbage glyph. Losing one character is invisible;
// losing the sequence it belongs to is not.
//
// A continuation byte is 0b10xxxxxx, so stepping back while the first DROPPED byte
// is one lands on a lead byte after at most three steps. Only a real truncation is
// examined: when the whole value fits, src[srcLen] is past the end of what the JSON
// parser handed over and must not be read.
void copyField(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  if (dstSize == 0) return;
  size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  if (srcLen > n) {
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

uint32_t toUint(const char* v, size_t len) {
  uint32_t out = 0;
  for (size_t i = 0; i < len; ++i) {
    if (v[i] < '0' || v[i] > '9') break;  // values arrive as "123" or "12.92万"
    out = out * 10 + static_cast<uint32_t>(v[i] - '0');
  }
  return out;
}

long toLong(const char* v, size_t len) {
  long out = 0;
  bool neg = false;
  size_t i = 0;
  if (len && v[0] == '-') {
    neg = true;
    i = 1;
  }
  for (; i < len; ++i) {
    if (v[i] < '0' || v[i] > '9') break;
    out = out * 10 + (v[i] - '0');
  }
  return neg ? -out : out;
}

// Appends "&name=value" with the value percent-encoded.
bool appendParam(char* url, size_t urlSize, const char* name, const char* value) {
  char encoded[512];
  if (!urlEncode(value, encoded, sizeof(encoded))) return false;
  const size_t used = strlen(url);
  const int n = snprintf(url + used, urlSize - used, "&%s=%s", name, encoded);
  return n > 0 && static_cast<size_t>(n) < urlSize - used;
}

bool appendRawParam(char* url, size_t urlSize, const char* name, const char* value) {
  const size_t used = strlen(url);
  const int n = snprintf(url + used, urlSize - used, "&%s=%s", name, value);
  return n > 0 && static_cast<size_t>(n) < urlSize - used;
}

// DES-CBC/PKCS7 encrypt, Base64-encoded. Used for the login password and sign.
bool desEncryptToBase64(const char* plain, char* out, size_t outSize) {
  const size_t plainLen = strlen(plain);
  const size_t ctCap = plainLen + Des::kBlockSize;
  uint8_t* ct = static_cast<uint8_t*>(malloc(ctCap));
  if (!ct) return false;
  const size_t ctLen = Des::cbcEncrypt(reinterpret_cast<const uint8_t*>(kDesKey),
                                       reinterpret_cast<const uint8_t*>(kDesIv),
                                       reinterpret_cast<const uint8_t*>(plain), plainLen, ct, ctCap);
  bool ok = false;
  if (ctLen > 0) {
    const size_t need = 4 * ((ctLen + 2) / 3) + 1;
    ok = need <= outSize && Base64::encode(ct, ctLen, out, outSize) > 0;
  }
  free(ct);
  return ok;
}

// --------------------------------------------------- response buffering ----

struct HeaderCapture {
  char accessKey[24] = {};
  char keyString[320] = {};
};

void headerCaptureSink(void* ctx, const char* name, size_t nameLen, const char* value, size_t valueLen) {
  auto* h = static_cast<HeaderCapture*>(ctx);
  // JJWXC sends these lowercase; match case-insensitively to be safe.
  char lower[24];
  if (nameLen >= sizeof(lower)) return;
  for (size_t i = 0; i < nameLen; ++i) {
    lower[i] = static_cast<char>((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
  }
  lower[nameLen] = '\0';
  if (strcmp(lower, "accesskey") == 0) copyField(h->accessKey, sizeof(h->accessKey), value, valueLen);
  else if (strcmp(lower, "keystring") == 0) copyField(h->keyString, sizeof(h->keyString), value, valueLen);
}

// The three request headers every JJWXC app-API call needs.
constexpr size_t kHeaderCount = 3;
RequestHeader apiHeaders[kHeaderCount] = {{"User-Agent", kUserAgent},
                                          {"Referer", kReferer},
                                          {"Accept", "application/json"}};

// -------------------------------------------------- field decoder ----------
//
// A `content` / `sayBody` string arrives either as plaintext UTF-8 or as Base64
// DES ciphertext. Which one it is is *told to us*, never guessed:
//
//   * the response's own `encryptType:"jj"` + `encryptField:[...]` declaration,
//     which is authoritative whenever it has been parsed before the value, and
//   * failing that, the request: the server returns the encrypted form exactly
//     when `versionCode` is present (verified live for both hosts, with and
//     without a token -- see docs/04-jjwxc-api-spec.md §3.4 forms A/B).
//
// The previous charset heuristic ("all Base64 and at least 32 chars") was wrong
// in both directions and is gone. Its false negative is the data-integrity bug
// this decoder now prevents: a chapter the account may not read comes back as
// `{"code":1004,...,"content":"zXEQsqjWQ0w=","encryptType":"jj"}`, where
// `content` is a 12-char server constant -- one DES block of pure PKCS#7
// padding, i.e. the encryption of the empty string. At 12 chars it fell below
// the heuristic's 32-char floor, so it was classified as plaintext and those 12
// Base64 characters were written into the book under that chapter's heading.
class FieldDecoder {
 public:
  // `encrypted` is decided by the caller from the server's declaration (or the
  // request contract), so the mode is fixed before the first byte arrives and
  // cannot flip mid-value on a chunk boundary.
  void begin(TextSink sink, void* ctx, const bool encrypted) {
    sink_ = sink;
    ctx_ = ctx;
    bytes_ = 0;
    failed_ = false;
    mode_ = encrypted ? DesBase64 : Raw;
    if (encrypted) {
      stream_.begin(reinterpret_cast<const uint8_t*>(kDesKey), reinterpret_cast<const uint8_t*>(kDesIv), sink_, ctx_);
    }
  }

  void feed(const char* value, size_t len, const bool finalChunk) {
    if (failed_ || len == 0) {
      if (finalChunk) end();
      return;
    }
    if (mode_ == DesBase64) {
      // DesBase64Stream rejects any non-Base64 byte, so a mis-declared field
      // fails loudly instead of silently writing ciphertext into the book.
      if (!stream_.feed(value, len)) failed_ = true;
    } else if (sink_) {
      if (!sink_(ctx_, value, len)) failed_ = true;
      bytes_ += len;
    }
    if (finalChunk) end();
  }

  void end() {
    if (mode_ == DesBase64 && !failed_) {
      if (!stream_.finish()) failed_ = true;
      else bytes_ = stream_.written();
    }
    mode_ = Done;
  }

  size_t bytes() const { return bytes_; }
  bool failed() const { return failed_; }
  bool sawData() const { return mode_ != Undecided; }

 private:
  enum Mode { Undecided, Raw, DesBase64, Done };

  Mode mode_ = Undecided;
  DesBase64Stream stream_;
  TextSink sink_ = nullptr;
  void* ctx_ = nullptr;
  size_t bytes_ = 0;
  bool failed_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

const char* statusText(Status s) {
  switch (s) {
    case Status::Ok: return "OK";
    case Status::NeedLogin: return "需要登录";
    case Status::NeedDeviceCode: return "需要设备验证码";
    case Status::BadCredentials: return "账号或密码错误";
    case Status::BadUserAgent: return "浏览器标识异常";
    case Status::BadVersion: return "客户端版本过旧";
    case Status::WrongCredentialSource: return "凭据来源不被接受";
    case Status::SignError: return "参数签名错误";
    case Status::EmptyResult: return "没有结果";
    case Status::PurchaseRequired: return "需要购买";
    case Status::Server: return "服务端错误";
    case Status::Network: return "网络错误";
    case Status::Protocol: return "响应格式异常";
    case Status::Decrypt: return "解密失败";
    case Status::TooLarge: return "响应过大";
    case Status::NoNetwork: return "Wi-Fi 未连接";
    case Status::Cancelled: return "已取消";
  }
  return "未知错误";
}

Status statusFromCode(long code) {
  switch (code) {
    // 0 is the common success code; getDeviceSecurityCode instead reports 200
    // with the human-readable text in data.message (verified against the live
    // service). Treating 200 as an error made a successful code send look like a
    // server failure.
    case 0:
    case 200: return Status::Ok;
    case 1004: return Status::NeedLogin;
    // Two different codes both mean "supply a verification code", and only the
    // first one was known. Measured live on app.jjwxc.org/androidapi/login:
    //   221003  the documented "unknown device" answer, carries the offered
    //           channels (email/phone) in data[]
    //   6018    message "6018:请校验验证码~", carries no channel list
    // Leaving 6018 in the `default` arm below reported "晋江服务端错误" and, worse,
    // never set the flag that drives getDeviceSecurityCode -- so the user was
    // told the server was broken instead of being asked for the code they were
    // already expecting to type. With no channel list the caller falls back to
    // email, which is what the account used for 221003.
    case 221003:
    case 6018: return Status::NeedDeviceCode;
    case 2016: return Status::BadUserAgent;
    case 231101: return Status::BadVersion;
    case 231102: return Status::WrongCredentialSource;
    case 1088:
    case 1002: return Status::SignError;
    case 1019:
    case 1055: return Status::EmptyResult;
    // Every "you may not read this chapter" answer the service has been seen to
    // give. 70200 belongs to `buy`; the other two come from `chapterContent` when
    // the account's auto-subscription could not complete, measured live on
    // novelId=4465668 with a valid token:
    //   1013  对不起，您的余额不足以购买下五章，自动购买失败。
    //   1010  对不起,您提交的订单中的章节【74】之前可能订阅过，或尚在交易处理中…
    // (1010 arrives as the STRING "1010"; toLong() handles both wire forms.)
    // Leaving them in the `default` arm below classified a per-chapter purchase
    // question as Status::Server, which is a whole-run failure: a book with an
    // unpurchased VIP tail aborted at its first such chapter and told the user the
    // server was broken.
    case 70200:
    case 1013:
    case 1010: return Status::PurchaseRequired;
    case 1005:
    case 1006: return Status::BadCredentials;
    default: return Status::Server;
  }
}

bool codeIsClientLevel(const long code) {
  switch (code) {
    // None of these is a property of one chapter. Retrying the next chapter would
    // only turn a broken client, a rejected credential source or a bad signature
    // into a book full of placeholders, so a batch run must stop at the first one.
    case 2016:    // BadUserAgent
    case 231101:  // BadVersion
    case 231102:  // WrongCredentialSource
    case 1088:    // SignError
    case 1002:    // SignError
    case 1005:    // BadCredentials
    case 1006:    // BadCredentials
    case 221003:  // NeedDeviceCode
    case 6018:    // NeedDeviceCode
      return true;
    default: return false;
  }
}

bool codeIsChapterRefusal(const long code) {
  switch (code) {
    // 1004 is deliberately NOT here: chapterContent uses it both for a rejected
    // token and for an unpurchased VIP chapter, so it needs classifyChapterRefusal
    // and a liveness probe rather than a flat answer.
    case 70200:
    case 1013:
    case 1010: return true;
    default: return false;
  }
}

// ---------------------------------------------------------------------------
// URL building
// ---------------------------------------------------------------------------

size_t urlEncode(const char* in, char* out, size_t outSize) {
  static const char kHex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p; ++p) {
    const unsigned char c = *p;
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '_' || c == '.' || c == '~';
    const size_t need = unreserved ? 1 : 3;
    if (o + need + 1 > outSize) return 0;
    if (unreserved) {
      out[o++] = static_cast<char>(c);
    } else {
      out[o++] = '%';
      out[o++] = kHex[c >> 4];
      out[o++] = kHex[c & 0x0f];
    }
  }
  out[o] = '\0';
  return o;
}

bool buildLoginUrl(const char* loginName, const char* password, const char* identifiers, const char* checkType,
                   const char* deviceCode, char* out, size_t outSize) {
  char encPass[128];
  if (!desEncryptToBase64(password, encPass, sizeof(encPass))) return false;

  // sign = DES("<millis>_<identifiers>_"). No server nonce is involved, so the
  // value is only a shape check; a fresh timestamp is enough.
  char signPlain[160];
  snprintf(signPlain, sizeof(signPlain), "%lu_%s_", static_cast<unsigned long>(time(nullptr)), identifiers);
  char encSign[256];
  if (!desEncryptToBase64(signPlain, encSign, sizeof(encSign))) return false;

  const int n = snprintf(out, outSize, "%s/androidapi/login?versionCode=%s&encode=1&autologin=1", kHostLogin,
                         kLoginVersionCode);
  if (n <= 0 || static_cast<size_t>(n) >= outSize) return false;
  if (!appendParam(out, outSize, "loginName", loginName)) return false;
  if (!appendParam(out, outSize, "loginPassword", encPass)) return false;
  if (!appendParam(out, outSize, "sign", encSign)) return false;
  if (!appendParam(out, outSize, "identifiers", identifiers)) return false;
  if (checkType && checkType[0] && deviceCode && deviceCode[0]) {
    if (!appendParam(out, outSize, "checktype", checkType)) return false;
    if (!appendParam(out, outSize, "checkdevicecode", deviceCode)) return false;
  }
  return true;
}

bool buildDeviceCodeUrl(const char* loginName, const char* checkType, char* out, size_t outSize) {
  (void)loginName;
  (void)checkType;
  const int n = snprintf(out, outSize, "%s/appDevicesecurityAndroid/getDeviceSecurityCode", kHostApi);
  return n > 0 && static_cast<size_t>(n) < outSize;
}

bool buildDeviceCodeBody(const char* loginName, const char* checkType, char* out, size_t outSize) {
  const int n = snprintf(out, outSize, "versionCode=%s&checktype=", kDeviceCodeVersionCode);
  if (n <= 0 || static_cast<size_t>(n) >= outSize) return false;
  const size_t used = strlen(out);
  if (used + strlen(checkType) + 1 > outSize) return false;
  memcpy(out + used, checkType, strlen(checkType) + 1);
  return appendParam(out, outSize, "username", loginName);
}

bool buildChapterContentUrl(const char* host, const char* novelId, uint32_t chapterId, const char* token, char* out,
                            size_t outSize) {
  const int n = snprintf(out, outSize, "%s/androidapi/chapterContent?novelId=%s&chapterId=%lu", host, novelId,
                         static_cast<unsigned long>(chapterId));
  if (n <= 0 || static_cast<size_t>(n) >= outSize) return false;
  // Free chapters omit versionCode entirely, which is what makes the server
  // return plaintext instead of demanding a signature.
  if (token && token[0]) {
    if (!appendRawParam(out, outSize, "versionCode", kVersionCode)) return false;
    if (!appendParam(out, outSize, "token", token)) return false;
  }
  return true;
}

bool buildChapterListUrl(const char* host, const char* novelId, const char* token, char* out, size_t outSize) {
  // whole=1 is required: without it the server returns only the first and last
  // five chapters as a preview.
  const int n = snprintf(out, outSize, "%s/androidapi/chapterList?novelId=%s&more=0&whole=1", host, novelId);
  if (n <= 0 || static_cast<size_t>(n) >= outSize) return false;
  if (token && token[0]) return appendParam(out, outSize, "token", token);
  return true;
}

bool buildNovelInfoUrl(const char* host, const char* novelId, const char* token, char* out, size_t outSize) {
  const int n = snprintf(out, outSize, "%s/androidapi/novelbasicinfo?novelId=%s", host, novelId);
  if (n <= 0 || static_cast<size_t>(n) >= outSize) return false;
  if (token && token[0]) return appendParam(out, outSize, "token", token);
  return true;
}

bool buildShelfUrl(const char* token, char* out, size_t outSize) {
  const int n = snprintf(out, outSize, "%s/androidapi/favorite?", kHostApi);
  if (n <= 0 || static_cast<size_t>(n) >= outSize) return false;
  if (!appendRawParam(out, outSize, "versionCode", kVersionCode)) return false;
  return appendParam(out, outSize, "token", token);
}

bool buildSearchUrl(const char* keyword, const char* token, char* out, size_t outSize) {
  const int n = snprintf(out, outSize, "%s/androidapi/search?", kHostApi);
  if (n <= 0 || static_cast<size_t>(n) >= outSize) return false;
  if (!appendParam(out, outSize, "keyword", keyword)) return false;
  if (!appendRawParam(out, outSize, "versionCode", kVersionCode)) return false;
  return appendParam(out, outSize, "token", token);
}

void makeIdentifiers(uint32_t (*random32)(), char out[64]) {
  static const char kHexDigits[] = "0123456789abcdef";
  char* p = out;
  for (int i = 0; i < 20; ++i) *p++ = static_cast<char>('0' + (random32() % 10));
  *p++ = ':';
  for (int i = 0; i < 32; ++i) *p++ = kHexDigits[random32() & 0xf];
  *p++ = 'd';
  *p++ = '4';
  *p++ = ':';
  *p = '\0';
}

// ---------------------------------------------------------------------------
// ChapterListParser
//
// Response shape:
//   {"chapterlist":[{...},{...}], "count":186, "vipChapterid":"17", ...}
// `count` and `vipChapterid` arrive *after* the array, so they are only
// available once finish() has run.
// ---------------------------------------------------------------------------

namespace {

enum ClField : uint8_t {
  CL_None = 0,
  CL_chapterid,
  CL_chaptername,
  CL_chapterdate,
  CL_chaptersize,
  CL_isvip,
  CL_chaptertype,
  CL_point,
  CL_originalPrice,
  CL_islock,
  CL_count,
  CL_vipChapterid,
};

uint8_t chapterListFieldFor(const char* k, size_t len, int depth) {
  if (depth == 2) {
    if (keyIs(k, len, "chapterid")) return CL_chapterid;
    if (keyIs(k, len, "chaptername")) return CL_chaptername;
    if (keyIs(k, len, "chapterdate")) return CL_chapterdate;
    if (keyIs(k, len, "chaptersize")) return CL_chaptersize;
    if (keyIs(k, len, "isvip")) return CL_isvip;
    if (keyIs(k, len, "chaptertype")) return CL_chaptertype;
    if (keyIs(k, len, "point")) return CL_point;
    if (keyIs(k, len, "originalPrice")) return CL_originalPrice;
    if (keyIs(k, len, "islock")) return CL_islock;
  } else if (depth == 1) {
    if (keyIs(k, len, "count")) return CL_count;
    if (keyIs(k, len, "vipChapterid")) return CL_vipChapterid;
  }
  return CL_None;
}

}  // namespace

void ChapterListParser::begin() {
  JsonCallbacks cb{};
  cb.ctx = this;
  cb.onKey = &ChapterListParser::onKey;
  cb.onString = &ChapterListParser::onString;
  cb.onNumber = &ChapterListParser::onNumber;
  cb.onBool = nullptr;
  cb.onNull = nullptr;
  cb.onObjectStart = &ChapterListParser::onObjectStart;
  cb.onObjectEnd = &ChapterListParser::onObjectEnd;
  cb.onArrayStart = &ChapterListParser::onArrayStart;
  cb.onArrayEnd = &ChapterListParser::onArrayEnd;
  parser_ = std::unique_ptr<StreamingJsonParser>(new (std::nothrow) StreamingJsonParser(cb));
  depth_ = 0;
  arrayDepth_ = 0;
  listArrayDepth_ = -1;
  inList_ = false;
  inItem_ = false;
  failed_ = (parser_ == nullptr);
  stop_ = false;
  emitted_ = 0;
  totalCount_ = 0;
  vipChapterId_ = 0;
  pendingArray_ = false;
}

bool ChapterListParser::feed(const char* data, size_t len) {
  if (failed_ || stop_ || !parser_) return !failed_ && !stop_;
  parser_->feed(data, len);
  if (parser_->hasError()) failed_ = true;
  return !failed_ && !stop_;
}

bool ChapterListParser::finish() {
  if (!parser_) return false;
  if (inItem_) emit();  // tolerate a truncated tail record
  if (parser_->hasError()) failed_ = true;
  parser_.reset();
  return !failed_ && !stop_;
}

void ChapterListParser::emit() {
  if (cur_.chapterId == 0) {
    inItem_ = false;
    cur_ = ChapterEntry{};
    return;
  }
  if (sink_ && !sink_(ctx_, cur_, emitted_)) stop_ = true;
  ++emitted_;
  inItem_ = false;
  cur_ = ChapterEntry{};
}

void ChapterListParser::onKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ChapterListParser*>(ctx);
  self->field_ = chapterListFieldFor(key, len, self->depth_);
  if (self->depth_ == 1 && keyIs(key, len, "chapterlist")) self->pendingArray_ = true;
}

void ChapterListParser::onString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ChapterListParser*>(ctx);
  switch (self->field_) {
    case CL_chaptername: copyField(self->cur_.name, sizeof(self->cur_.name), value, len); break;
    case CL_chapterdate: copyField(self->cur_.date, sizeof(self->cur_.date), value, len); break;
    case CL_chapterid: self->cur_.chapterId = toUint(value, len); break;
    case CL_chaptersize: self->cur_.size = toUint(value, len); break;
    case CL_isvip: self->cur_.isVip = static_cast<uint8_t>(toUint(value, len)); break;
    case CL_chaptertype: self->cur_.chapterType = static_cast<uint8_t>(toUint(value, len)); break;
    case CL_point: self->cur_.price = toUint(value, len); break;
    case CL_originalPrice: self->cur_.listPrice = toUint(value, len); break;
    case CL_islock: self->cur_.isLock = (len > 0 && value[0] != '0'); break;
    case CL_count: self->totalCount_ = toUint(value, len); break;
    case CL_vipChapterid: self->vipChapterId_ = toUint(value, len); break;
    default: break;
  }
  self->field_ = 0;
}

void ChapterListParser::onNumber(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ChapterListParser*>(ctx);
  switch (self->field_) {
    case CL_chapterid: self->cur_.chapterId = static_cast<uint32_t>(toLong(value, len)); break;
    case CL_chaptersize: self->cur_.size = static_cast<uint32_t>(toLong(value, len)); break;
    case CL_isvip: self->cur_.isVip = static_cast<uint8_t>(toLong(value, len)); break;
    case CL_chaptertype: self->cur_.chapterType = static_cast<uint8_t>(toLong(value, len)); break;
    case CL_point: self->cur_.price = static_cast<uint32_t>(toLong(value, len)); break;
    case CL_originalPrice: self->cur_.listPrice = static_cast<uint32_t>(toLong(value, len)); break;
    case CL_count: self->totalCount_ = static_cast<uint32_t>(toLong(value, len)); break;
    case CL_vipChapterid: self->vipChapterId_ = static_cast<uint32_t>(toLong(value, len)); break;
    default: break;
  }
  self->field_ = 0;
}

void ChapterListParser::onObjectStart(void* ctx) {
  auto* self = static_cast<ChapterListParser*>(ctx);
  ++self->depth_;
  if (self->depth_ == 2 && self->inList_) {
    self->inItem_ = true;
    self->cur_ = ChapterEntry{};
  }
}

void ChapterListParser::onObjectEnd(void* ctx) {
  auto* self = static_cast<ChapterListParser*>(ctx);
  if (self->inItem_ && self->depth_ == 2) self->emit();
  if (self->depth_ > 0) --self->depth_;
}

void ChapterListParser::onArrayStart(void* ctx) {
  auto* self = static_cast<ChapterListParser*>(ctx);
  // Arrays do not participate in depth_ (object nesting only); see the header.
  ++self->arrayDepth_;
  if (self->pendingArray_) {
    self->inList_ = true;
    self->listArrayDepth_ = self->arrayDepth_;
    self->pendingArray_ = false;
  }
}

void ChapterListParser::onArrayEnd(void* ctx) {
  auto* self = static_cast<ChapterListParser*>(ctx);
  if (self->inList_ && self->arrayDepth_ == self->listArrayDepth_) self->inList_ = false;
  if (self->arrayDepth_ > 0) --self->arrayDepth_;
}

// ---------------------------------------------------------------------------
// ShelfParser -- response shape: {"count":12,"data":[{novelId,...}]}
// Note the camelCase field names here, versus the all-lowercase ones the
// search endpoint uses. Both spellings were confirmed against live traffic.
// ---------------------------------------------------------------------------

namespace {

enum ShField : uint8_t {
  SH_None = 0,
  SH_novelId,
  SH_novelName,
  SH_authorName,
  SH_novelCover,
  SH_lastChapterName,
  SH_chapterCount,
  SH_lastChapterId,
  SH_lastChapterTime,
  SH_renewStatus,
  // Top level only. `favorite` answers a rejected credential with
  // {"code":1004,"message":"1004:登入验证失败!"} and NO data array, so without
  // this the reply is indistinguishable from an account with zero favourites --
  // which is also why probeTokenAlive() can use this endpoint as its oracle.
  SH_code,
};

uint8_t shelfFieldFor(const char* k, size_t len, int depth) {
  if (depth != 2) return SH_None;
  if (keyIs(k, len, "novelId")) return SH_novelId;
  if (keyIs(k, len, "novelName")) return SH_novelName;
  if (keyIs(k, len, "authorName")) return SH_authorName;
  if (keyIs(k, len, "novelCover")) return SH_novelCover;
  if (keyIs(k, len, "lastChapterName")) return SH_lastChapterName;
  if (keyIs(k, len, "chapterCount")) return SH_chapterCount;
  if (keyIs(k, len, "lastChapterId")) return SH_lastChapterId;
  if (keyIs(k, len, "lastChapterTime")) return SH_lastChapterTime;
  if (keyIs(k, len, "renewStatus")) return SH_renewStatus;
  return SH_None;
}

enum SrField : uint8_t {
  SR_None = 0,
  SR_novelid,
  SR_novelname,
  SR_authorname,
  SR_novelsize,
  SR_novelintroshort,
  SR_count,
};

uint8_t searchFieldFor(const char* k, size_t len, int depth) {
  if (depth == 2) {
    if (keyIs(k, len, "novelid")) return SR_novelid;
    if (keyIs(k, len, "novelname")) return SR_novelname;
    if (keyIs(k, len, "authorname")) return SR_authorname;
    if (keyIs(k, len, "novelsize")) return SR_novelsize;
    if (keyIs(k, len, "novelintroshort")) return SR_novelintroshort;
  } else if (depth == 1) {
    if (keyIs(k, len, "count")) return SR_count;
  }
  return SR_None;
}

}  // namespace

void ShelfParser::begin() {
  JsonCallbacks cb{};
  cb.ctx = this;
  cb.onKey = &ShelfParser::onKey;
  cb.onString = &ShelfParser::onString;
  cb.onNumber = &ShelfParser::onNumber;
  cb.onObjectStart = &ShelfParser::onObjectStart;
  cb.onObjectEnd = &ShelfParser::onObjectEnd;
  cb.onArrayStart = &ShelfParser::onArrayStart;
  cb.onArrayEnd = &ShelfParser::onArrayEnd;
  parser_ = std::unique_ptr<StreamingJsonParser>(new (std::nothrow) StreamingJsonParser(cb));
  depth_ = 0;
  arrayDepth_ = 0;
  listArrayDepth_ = -1;
  inList_ = false;
  inItem_ = false;
  pendingArray_ = false;
  failed_ = (parser_ == nullptr);
  stop_ = false;
  emitted_ = 0;
  code_ = 0;
  hasCode_ = false;
}

bool ShelfParser::feed(const char* data, size_t len) {
  if (failed_ || stop_ || !parser_) return !failed_ && !stop_;
  parser_->feed(data, len);
  if (parser_->hasError()) failed_ = true;
  return !failed_ && !stop_;
}

bool ShelfParser::finish() {
  if (!parser_) return false;
  if (inItem_) emit();
  if (parser_->hasError()) failed_ = true;
  parser_.reset();
  return !failed_ && !stop_;
}

void ShelfParser::emit() {
  if (cur_.novelId[0] != '\0') {
    if (sink_ && !sink_(ctx_, cur_, emitted_)) stop_ = true;
    ++emitted_;
  }
  inItem_ = false;
  cur_ = ShelfEntry{};
}

void ShelfParser::onKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ShelfParser*>(ctx);
  self->field_ = shelfFieldFor(key, len, self->depth_);
  if (self->depth_ == 1) {
    if (keyIs(key, len, "data")) self->pendingArray_ = true;
    // Only at depth 1: a favourite record may carry its own unrelated `code`.
    else if (keyIs(key, len, "code")) self->field_ = SH_code;
  }
}

void ShelfParser::onString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ShelfParser*>(ctx);
  switch (self->field_) {
    case SH_novelId: copyField(self->cur_.novelId, sizeof(self->cur_.novelId), value, len); break;
    case SH_novelName: copyField(self->cur_.novelName, sizeof(self->cur_.novelName), value, len); break;
    case SH_authorName: copyField(self->cur_.authorName, sizeof(self->cur_.authorName), value, len); break;
    case SH_novelCover: copyField(self->cur_.coverUrl, sizeof(self->cur_.coverUrl), value, len); break;
    case SH_lastChapterName:
      copyField(self->cur_.lastChapterName, sizeof(self->cur_.lastChapterName), value, len);
      break;
    case SH_chapterCount: self->cur_.chapterCount = toUint(value, len); break;
    case SH_lastChapterId: self->cur_.lastChapterId = toUint(value, len); break;
    case SH_lastChapterTime: self->cur_.lastChapterTime = toUint(value, len); break;
    case SH_renewStatus: self->cur_.renewStatus = static_cast<uint8_t>(toUint(value, len)); break;
    // Both wire forms occur: `"code":1004` and `"code":"1004"`.
    case SH_code:
      self->code_ = toLong(value, len);
      self->hasCode_ = true;
      break;
    default: break;
  }
  self->field_ = 0;
}

void ShelfParser::onNumber(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ShelfParser*>(ctx);
  const uint32_t v = static_cast<uint32_t>(toLong(value, len));
  switch (self->field_) {
    case SH_chapterCount: self->cur_.chapterCount = v; break;
    case SH_lastChapterId: self->cur_.lastChapterId = v; break;
    case SH_lastChapterTime: self->cur_.lastChapterTime = v; break;
    case SH_renewStatus: self->cur_.renewStatus = static_cast<uint8_t>(v); break;
    case SH_code:
      self->code_ = toLong(value, len);
      self->hasCode_ = true;
      break;
    default: break;
  }
  self->field_ = 0;
}

void ShelfParser::onObjectStart(void* ctx) {
  auto* self = static_cast<ShelfParser*>(ctx);
  ++self->depth_;
  if (self->depth_ == 2 && self->inList_) {
    self->inItem_ = true;
    self->cur_ = ShelfEntry{};
  }
}

void ShelfParser::onObjectEnd(void* ctx) {
  auto* self = static_cast<ShelfParser*>(ctx);
  if (self->inItem_ && self->depth_ == 2) self->emit();
  if (self->depth_ > 0) --self->depth_;
}

void ShelfParser::onArrayStart(void* ctx) {
  auto* self = static_cast<ShelfParser*>(ctx);
  ++self->arrayDepth_;
  if (self->pendingArray_) {
    self->inList_ = true;
    self->listArrayDepth_ = self->arrayDepth_;
    self->pendingArray_ = false;
  }
}

void ShelfParser::onArrayEnd(void* ctx) {
  auto* self = static_cast<ShelfParser*>(ctx);
  if (self->inList_ && self->arrayDepth_ == self->listArrayDepth_) self->inList_ = false;
  if (self->arrayDepth_ > 0) --self->arrayDepth_;
}

// ---------------------------------------------------------------------------
// SearchParser -- response shape: {"items":[{novelid,...}],"count":5669,...}
// ---------------------------------------------------------------------------

void SearchParser::begin() {
  JsonCallbacks cb{};
  cb.ctx = this;
  cb.onKey = &SearchParser::onKey;
  cb.onString = &SearchParser::onString;
  cb.onNumber = &SearchParser::onNumber;
  cb.onObjectStart = &SearchParser::onObjectStart;
  cb.onObjectEnd = &SearchParser::onObjectEnd;
  cb.onArrayStart = &SearchParser::onArrayStart;
  cb.onArrayEnd = &SearchParser::onArrayEnd;
  parser_ = std::unique_ptr<StreamingJsonParser>(new (std::nothrow) StreamingJsonParser(cb));
  depth_ = 0;
  arrayDepth_ = 0;
  listArrayDepth_ = -1;
  inList_ = false;
  inItem_ = false;
  pendingArray_ = false;
  failed_ = (parser_ == nullptr);
  stop_ = false;
  emitted_ = 0;
  totalCount_ = 0;
}

bool SearchParser::feed(const char* data, size_t len) {
  if (failed_ || stop_ || !parser_) return !failed_ && !stop_;
  parser_->feed(data, len);
  if (parser_->hasError()) failed_ = true;
  return !failed_ && !stop_;
}

bool SearchParser::finish() {
  if (!parser_) return false;
  if (inItem_) emit();
  if (parser_->hasError()) failed_ = true;
  parser_.reset();
  return !failed_ && !stop_;
}

void SearchParser::emit() {
  if (cur_.novelId[0] != '\0') {
    if (sink_ && !sink_(ctx_, cur_, emitted_)) stop_ = true;
    ++emitted_;
  }
  inItem_ = false;
  cur_ = SearchItem{};
}

void SearchParser::onKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<SearchParser*>(ctx);
  self->field_ = searchFieldFor(key, len, self->depth_);
  if (self->depth_ == 1 && keyIs(key, len, "items")) self->pendingArray_ = true;
}

void SearchParser::onString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<SearchParser*>(ctx);
  switch (self->field_) {
    case SR_novelid: copyField(self->cur_.novelId, sizeof(self->cur_.novelId), value, len); break;
    case SR_novelname: copyField(self->cur_.novelName, sizeof(self->cur_.novelName), value, len); break;
    case SR_authorname: copyField(self->cur_.authorName, sizeof(self->cur_.authorName), value, len); break;
    case SR_novelsize: copyField(self->cur_.sizeText, sizeof(self->cur_.sizeText), value, len); break;
    case SR_novelintroshort:
      copyField(self->cur_.introShort, sizeof(self->cur_.introShort), value, len);
      break;
    case SR_count: self->totalCount_ = toUint(value, len); break;
    default: break;
  }
  self->field_ = 0;
}

void SearchParser::onNumber(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<SearchParser*>(ctx);
  if (self->field_ == SR_count) self->totalCount_ = static_cast<uint32_t>(toLong(value, len));
  self->field_ = 0;
}

void SearchParser::onObjectStart(void* ctx) {
  auto* self = static_cast<SearchParser*>(ctx);
  ++self->depth_;
  if (self->depth_ == 2 && self->inList_) {
    self->inItem_ = true;
    self->cur_ = SearchItem{};
  }
}

void SearchParser::onObjectEnd(void* ctx) {
  auto* self = static_cast<SearchParser*>(ctx);
  if (self->inItem_ && self->depth_ == 2) self->emit();
  if (self->depth_ > 0) --self->depth_;
}

void SearchParser::onArrayStart(void* ctx) {
  auto* self = static_cast<SearchParser*>(ctx);
  ++self->arrayDepth_;
  if (self->pendingArray_) {
    self->inList_ = true;
    self->listArrayDepth_ = self->arrayDepth_;
    self->pendingArray_ = false;
  }
}

void SearchParser::onArrayEnd(void* ctx) {
  auto* self = static_cast<SearchParser*>(ctx);
  if (self->inList_ && self->arrayDepth_ == self->listArrayDepth_) self->inList_ = false;
  if (self->arrayDepth_ > 0) --self->arrayDepth_;
}

// ---------------------------------------------------------------------------
// Small single-object responses: novelbasicinfo, login, getDeviceSecurityCode.
// These are a few KB at most, so the caller supplies the whole buffer.
// ---------------------------------------------------------------------------

namespace {

struct NovelInfoCtx {
  NovelInfo* out;
  uint8_t field;
  // Optional: where `novelIntro` goes. Kept here rather than in NovelInfo because
  // the blurb is kilobytes and NovelInfo lives in internal RAM (see the header).
  NovelIntroSink introSink;
  void* introCtx;
  // Set when the intro sink refuses the value. niStr() returns void, so this is how
  // a refusal reaches the feed callback that can actually stop the download.
  bool abort;
};

enum NiField : uint8_t {
  NI_None = 0, NI_novelId, NI_novelName, NI_authorName, NI_authorId, NI_novelClass, NI_novelTags,
  NI_sizeFormat, NI_renewChapterName, NI_renewDate, NI_novelCover, NI_introShort, NI_chapterCount,
  NI_maxChapterId, NI_vipChapterId, NI_isVip, NI_renewChapterId, NI_code, NI_novelIntro
};

uint8_t novelInfoFieldFor(const char* k, size_t len) {
  if (keyIs(k, len, "novelId")) return NI_novelId;
  if (keyIs(k, len, "novelName")) return NI_novelName;
  if (keyIs(k, len, "authorName")) return NI_authorName;
  if (keyIs(k, len, "authorId")) return NI_authorId;
  if (keyIs(k, len, "novelClass")) return NI_novelClass;
  if (keyIs(k, len, "novelTags")) return NI_novelTags;
  if (keyIs(k, len, "novelsizeformat")) return NI_sizeFormat;
  if (keyIs(k, len, "renewChapterName")) return NI_renewChapterName;
  if (keyIs(k, len, "renewDate")) return NI_renewDate;
  if (keyIs(k, len, "novelCover")) return NI_novelCover;
  if (keyIs(k, len, "novelIntroShort")) return NI_introShort;
  // After the Short variant, and matched on length as well as bytes, so the two
  // cannot be confused: keyIs() compares strlen(name) == len first.
  if (keyIs(k, len, "novelIntro")) return NI_novelIntro;
  if (keyIs(k, len, "novelChapterCount")) return NI_chapterCount;
  if (keyIs(k, len, "maxChapterId")) return NI_maxChapterId;
  if (keyIs(k, len, "vipChapterid")) return NI_vipChapterId;
  if (keyIs(k, len, "isVip")) return NI_isVip;
  if (keyIs(k, len, "renewChapterId")) return NI_renewChapterId;
  if (keyIs(k, len, "code")) return NI_code;
  return NI_None;
}

void niKey(void* c, const char* k, size_t len) { static_cast<NovelInfoCtx*>(c)->field = novelInfoFieldFor(k, len); }
void niStr(void* c, const char* v, size_t len) {
  auto* ctx = static_cast<NovelInfoCtx*>(c);
  NovelInfo& o = *ctx->out;
  switch (ctx->field) {
    case NI_novelId: copyField(o.novelId, sizeof(o.novelId), v, len); break;
    case NI_novelName: copyField(o.novelName, sizeof(o.novelName), v, len); break;
    case NI_authorName: copyField(o.authorName, sizeof(o.authorName), v, len); break;
    case NI_authorId: copyField(o.authorId, sizeof(o.authorId), v, len); break;
    case NI_novelClass: copyField(o.novelClass, sizeof(o.novelClass), v, len); break;
    case NI_novelTags: copyField(o.novelTags, sizeof(o.novelTags), v, len); break;
    case NI_sizeFormat: copyField(o.sizeText, sizeof(o.sizeText), v, len); break;
    case NI_renewChapterName: copyField(o.lastChapterName, sizeof(o.lastChapterName), v, len); break;
    case NI_renewDate: copyField(o.renewDate, sizeof(o.renewDate), v, len); break;
    case NI_novelCover: copyField(o.coverUrl, sizeof(o.coverUrl), v, len); break;
    case NI_introShort: copyField(o.introShort, sizeof(o.introShort), v, len); break;
    case NI_novelIntro:
      // Straight to the caller: no fixed buffer here to truncate into, and no
      // unescaping -- see NovelIntroSink in the header for why the value is raw.
      if (ctx->introSink && !ctx->introSink(ctx->introCtx, v, len)) ctx->abort = true;
      break;
    case NI_chapterCount: o.chapterCount = toUint(v, len); break;
    case NI_maxChapterId: o.maxChapterId = toUint(v, len); break;
    case NI_vipChapterId: o.vipChapterId = toUint(v, len); break;
    case NI_renewChapterId: o.renewChapterId = toUint(v, len); break;
    case NI_isVip: o.isVip = (len > 0 && v[0] != '0'); break;
    default: break;
  }
  ctx->field = NI_None;
}
void niNum(void* c, const char* v, size_t len) {
  auto* ctx = static_cast<NovelInfoCtx*>(c);
  NovelInfo& o = *ctx->out;
  const uint32_t n = static_cast<uint32_t>(toLong(v, len));
  switch (ctx->field) {
    case NI_chapterCount: o.chapterCount = n; break;
    case NI_maxChapterId: o.maxChapterId = n; break;
    case NI_vipChapterId: o.vipChapterId = n; break;
    case NI_renewChapterId: o.renewChapterId = n; break;
    case NI_isVip: o.isVip = (n != 0); break;
    default: break;
  }
  ctx->field = NI_None;
}

struct LoginCtx {
  LoginResult* out;
  uint8_t field;
  bool inDataArray;
  int arrayDepth;
  int listDepth;
};

enum LgField : uint8_t { LG_None = 0, LG_token, LG_readerId, LG_nickName, LG_code, LG_message, LG_type };

void lgKey(void* c, const char* k, size_t len) {
  auto* ctx = static_cast<LoginCtx*>(c);
  if (keyIs(k, len, "token")) ctx->field = LG_token;
  else if (keyIs(k, len, "readerId")) ctx->field = LG_readerId;
  else if (keyIs(k, len, "nickName")) ctx->field = LG_nickName;
  else if (keyIs(k, len, "code")) ctx->field = LG_code;
  else if (keyIs(k, len, "message")) ctx->field = LG_message;
  else if (keyIs(k, len, "type")) ctx->field = LG_type;
  else ctx->field = LG_None;
  if (keyIs(k, len, "data")) ctx->inDataArray = true;  // pending; confirmed on array start
}
void lgStr(void* c, const char* v, size_t len) {
  auto* ctx = static_cast<LoginCtx*>(c);
  LoginResult& o = *ctx->out;
  switch (ctx->field) {
    case LG_token: copyField(o.credentials.token, sizeof(o.credentials.token), v, len); break;
    case LG_readerId: copyField(o.credentials.readerId, sizeof(o.credentials.readerId), v, len); break;
    case LG_nickName: copyField(o.credentials.nickName, sizeof(o.credentials.nickName), v, len); break;
    case LG_code:
      o.code = toLong(v, len);
      o.status = statusFromCode(o.code);
      break;
    case LG_message: copyField(o.message, sizeof(o.message), v, len); break;
    case LG_type:
      if (keyIs(v, len, "email")) o.offersEmail = true;
      else if (keyIs(v, len, "phone")) o.offersPhone = true;
      break;
    default: break;
  }
  ctx->field = LG_None;
}
void lgNum(void* c, const char* v, size_t len) {
  auto* ctx = static_cast<LoginCtx*>(c);
  if (ctx->field == LG_code) {
    ctx->out->code = toLong(v, len);
    ctx->out->status = statusFromCode(ctx->out->code);
  }
  ctx->field = LG_None;
}
void lgArrStart(void* c) {
  auto* ctx = static_cast<LoginCtx*>(c);
  ++ctx->arrayDepth;
  if (ctx->inDataArray) { ctx->listDepth = ctx->arrayDepth; ctx->inDataArray = false; }
}
void lgArrEnd(void* c) {
  auto* ctx = static_cast<LoginCtx*>(c);
  if (ctx->arrayDepth > 0) --ctx->arrayDepth;
}

struct DevCodeCtx {
  DeviceCodeResult* out;
  uint8_t field;  // 1 = outer message, 2 = data.message (preferred)
  int depth;
};

void dcKey(void* c, const char* k, size_t len) {
  auto* ctx = static_cast<DevCodeCtx*>(c);
  if (keyIs(k, len, "message")) ctx->field = (ctx->depth >= 2) ? 2 : 1;
  else if (keyIs(k, len, "code")) ctx->field = 3;
  else ctx->field = 0;
}
void dcStr(void* c, const char* v, size_t len) {
  auto* ctx = static_cast<DevCodeCtx*>(c);
  DeviceCodeResult& o = *ctx->out;
  if (ctx->field == 2 || (ctx->field == 1 && o.message[0] == '\0')) {
    copyField(o.message, sizeof(o.message), v, len);
  } else if (ctx->field == 3) {
    o.status = statusFromCode(toLong(v, len));
  }
  ctx->field = 0;
}
void dcNum(void* c, const char* v, size_t len) {
  auto* ctx = static_cast<DevCodeCtx*>(c);
  if (ctx->field == 3) ctx->out->status = statusFromCode(toLong(v, len));
  ctx->field = 0;
}
void dcObjStart(void* c) { ++static_cast<DevCodeCtx*>(c)->depth; }
void dcObjEnd(void* c) {
  auto* ctx = static_cast<DevCodeCtx*>(c);
  if (ctx->depth > 0) --ctx->depth;
}

// Runs a StreamingJsonParser over an in-memory buffer.
bool runParser(JsonCallbacks& cb, const char* json, size_t len) {
  StreamingJsonParser parser(cb);
  parser.feed(json, len);
  return !parser.hasError();
}

}  // namespace

bool parseNovelInfo(const char* json, size_t len, NovelInfo& out, NovelIntroSink introSink, void* introCtx) {
  out = NovelInfo{};
  NovelInfoCtx ctx{&out, 0, introSink, introCtx, false};
  JsonCallbacks cb{};
  cb.ctx = &ctx;
  cb.onKey = niKey;
  cb.onString = niStr;
  cb.onNumber = niNum;
  return runParser(cb, json, len) && !ctx.abort && out.novelId[0] != '\0';
}

bool parseLogin(const char* json, size_t len, LoginResult& out) {
  out = LoginResult{};
  out.status = Status::Protocol;
  LoginCtx ctx{&out, 0, false, 0, -1};
  JsonCallbacks cb{};
  cb.ctx = &ctx;
  cb.onKey = lgKey;
  cb.onString = lgStr;
  cb.onNumber = lgNum;
  cb.onArrayStart = lgArrStart;
  cb.onArrayEnd = lgArrEnd;
  if (!runParser(cb, json, len)) return false;
  if (out.credentials.token[0] != '\0') {
    out.status = Status::Ok;
  } else if (out.status == Status::Protocol) {
    out.status = Status::Server;
  }
  return true;
}

bool parseDeviceCode(const char* json, size_t len, DeviceCodeResult& out) {
  out = DeviceCodeResult{};
  DevCodeCtx ctx{&out, 0, 0};
  JsonCallbacks cb{};
  cb.ctx = &ctx;
  cb.onKey = dcKey;
  cb.onString = dcStr;
  cb.onNumber = dcNum;
  cb.onObjectStart = dcObjStart;
  cb.onObjectEnd = dcObjEnd;
  return runParser(cb, json, len);
}

// ---------------------------------------------------------------------------
// Chapter body parser. Drives a FieldDecoder per text field so the decoded
// prose streams straight to the caller (ultimately the SD card) instead of
// being buffered: peak memory is one response body, not body + plaintext.
// ---------------------------------------------------------------------------

namespace {

enum ChField : uint8_t {
  CH_None = 0, CH_chapterName, CH_chapterDate, CH_chapterSize, CH_isvip,
  CH_content, CH_sayBody, CH_sayBodyV2, CH_code, CH_message,
  CH_encryptType, CH_encryptField, CH_chapterId
};

// Which field a bit of `declaredMask` refers to. The response names its own
// encrypted fields in `encryptField`, so the decoder never has to guess.
enum ChEncBit : uint8_t {
  ENC_content = 1u << 0,
  ENC_sayBody = 1u << 1,
  ENC_sayBodyV2 = 1u << 2,
};

uint8_t encBitFor(const uint8_t field) {
  switch (field) {
    case CH_content: return ENC_content;
    case CH_sayBody: return ENC_sayBody;
    case CH_sayBodyV2: return ENC_sayBodyV2;
    default: return 0;
  }
}

struct ChapterParseCtx {
  ChapterMeta* meta;
  TextSink textSink;
  void* textCtx;
  TextSink saySink;
  void* sayCtx;
  FieldDecoder content;
  FieldDecoder sayBody;
  uint8_t field = CH_None;
  long code = 0;
  bool hasCode = false;
  char message[128] = {};
  bool failed = false;

  // --- encryption decision (server-declared, never guessed from the payload) --
  bool expectEncrypted = false;  // the request carried versionCode
  bool sawEncryptType = false;
  bool sawEncryptField = false;
  bool encryptIsJj = false;
  uint8_t declaredMask = 0;

  // --- error gate -----------------------------------------------------------
  // Once a non-zero business code is known, not one more body byte may reach the
  // caller. `code` is the FIRST key of every blocked response observed live
  // (with and without versionCode, on both hosts, two novels, four chapter ids),
  // so in practice nothing at all is emitted. If a future response shape ever
  // put `content` ahead of `code`, emittedText still reports exactly how many
  // bytes escaped -- see the ChapterMeta::textBytes contract in the header,
  // which is what a streaming caller discards or rolls back on a failed fetch.
  bool blocked = false;
  size_t emittedText = 0;
  size_t emittedSay = 0;

  // Records a business code and closes the gate on anything but success. Both 0
  // and 200 mean success (see statusFromCode); monotonic, so a later code can
  // never re-open a gate an earlier one closed.
  void setCode(const long c) {
    code = c;
    hasCode = true;
    if (statusFromCode(c) != Status::Ok) blocked = true;
  }

  // Is the field currently being delivered Base64+DES ciphertext?
  bool fieldEncrypted(const uint8_t f) const {
    const uint8_t bit = encBitFor(f);
    if (bit != 0 && (declaredMask & bit) != 0) return !sawEncryptType || encryptIsJj;
    if (sawEncryptField) return false;                 // list parsed, field not named in it
    if (sawEncryptType && !encryptIsJj) return false;  // explicitly declared plaintext
    return expectEncrypted;                            // declaration not parsed yet
  }
};

// Body text reaches the caller only through these gates, so a response that
// turns out to be an error cannot leak bytes into the book.
bool gateText(void* c, const char* s, size_t n) {
  auto* ctx = static_cast<ChapterParseCtx*>(c);
  if (ctx->blocked || ctx->textSink == nullptr) return true;
  if (!ctx->textSink(ctx->textCtx, s, n)) return false;
  ctx->emittedText += n;
  return true;
}

bool gateSay(void* c, const char* s, size_t n) {
  auto* ctx = static_cast<ChapterParseCtx*>(c);
  if (ctx->blocked || ctx->saySink == nullptr) return true;
  if (!ctx->saySink(ctx->sayCtx, s, n)) return false;
  ctx->emittedSay += n;
  return true;
}

void chKey(void* c, const char* k, size_t len) {
  auto* ctx = static_cast<ChapterParseCtx*>(c);
  if (keyIs(k, len, "chapterName")) ctx->field = CH_chapterName;
  else if (keyIs(k, len, "chapterId")) ctx->field = CH_chapterId;
  else if (keyIs(k, len, "chapterDate")) ctx->field = CH_chapterDate;
  else if (keyIs(k, len, "chapterSize")) ctx->field = CH_chapterSize;
  else if (keyIs(k, len, "isvip")) ctx->field = CH_isvip;
  else if (keyIs(k, len, "content")) ctx->field = CH_content;
  else if (keyIs(k, len, "sayBody")) ctx->field = CH_sayBody;
  else if (keyIs(k, len, "sayBodyV2")) ctx->field = CH_sayBodyV2;
  else if (keyIs(k, len, "code")) ctx->field = CH_code;
  else if (keyIs(k, len, "message")) ctx->field = CH_message;
  else if (keyIs(k, len, "encryptType")) {
    ctx->sawEncryptType = true;  // flag on the KEY: it may arrive after `content`
    ctx->field = CH_encryptType;
  } else if (keyIs(k, len, "encryptField")) {
    ctx->sawEncryptField = true;
    ctx->field = CH_encryptField;
  } else ctx->field = CH_None;
}

// Routes a value (whole, or one chunk of a long string) to the right decoder.
void chValue(void* c, const char* v, size_t len, bool finalChunk) {
  auto* ctx = static_cast<ChapterParseCtx*>(c);
  switch (ctx->field) {
    case CH_chapterName:
      if (finalChunk) copyField(ctx->meta->name, sizeof(ctx->meta->name), v, len);
      break;
    case CH_chapterDate:
      if (finalChunk) copyField(ctx->meta->date, sizeof(ctx->meta->date), v, len);
      break;
    case CH_chapterSize: ctx->meta->size = toUint(v, len); break;
    // The reply's own echo of which chapter it is serving. Captured so a caller can
    // refuse to write a body that is not the one it asked for -- see ChapterMeta.
    case CH_chapterId:
      if (finalChunk) ctx->meta->chapterId = static_cast<uint32_t>(toLong(v, len));
      break;
    case CH_isvip: ctx->meta->isVip = (toLong(v, len) != 0); break;
    case CH_code: ctx->setCode(toLong(v, len)); break;
    case CH_message:
      if (finalChunk) copyField(ctx->message, sizeof(ctx->message), v, len);
      break;
    case CH_encryptType:
      if (finalChunk) ctx->encryptIsJj = keyIs(v, len, "jj");
      break;
    case CH_encryptField:
      // `encryptField` is an ARRAY of field names, so every element arrives here
      // while `field` is still CH_encryptField. Return without clearing it or
      // only the first name would be recorded.
      if (keyIs(v, len, "content")) ctx->declaredMask |= ENC_content;
      else if (keyIs(v, len, "sayBody")) ctx->declaredMask |= ENC_sayBody;
      else if (keyIs(v, len, "sayBodyV2")) ctx->declaredMask |= ENC_sayBodyV2;
      return;
    case CH_content:
      // A rejected chapter's `content` is a server constant, not prose: skip it
      // entirely rather than decode-then-discard.
      if (ctx->blocked) break;
      if (!ctx->content.sawData()) {
        ctx->content.begin(&gateText, ctx, ctx->fieldEncrypted(CH_content));
      }
      ctx->content.feed(v, len, finalChunk);
      if (ctx->content.failed()) ctx->failed = true;
      break;
    case CH_sayBody:
    case CH_sayBodyV2:
      if (ctx->saySink && !ctx->blocked) {
        if (!ctx->sayBody.sawData()) {
          ctx->sayBody.begin(&gateSay, ctx, ctx->fieldEncrypted(ctx->field));
        }
        ctx->sayBody.feed(v, len, finalChunk);
        if (ctx->sayBody.failed()) ctx->failed = true;
      }
      break;
    default: break;
  }
  if (finalChunk) ctx->field = CH_None;
}

void chStr(void* c, const char* v, size_t len) { chValue(c, v, len, /*finalChunk=*/true); }
void chNum(void* c, const char* v, size_t len) {
  auto* ctx = static_cast<ChapterParseCtx*>(c);
  if (ctx->field == CH_code) ctx->setCode(toLong(v, len));
  else if (ctx->field == CH_chapterSize) ctx->meta->size = static_cast<uint32_t>(toLong(v, len));
  else if (ctx->field == CH_isvip) ctx->meta->isVip = (toLong(v, len) != 0);
  // The live service sends chapterId as a STRING, but `code` arrives both ways on this
  // endpoint (measured: 1013 numeric, "1010" quoted), so nothing here may assume one
  // spelling. A numeric id that parsed to 0 would disable the caller's mismatch gate
  // rather than fail loudly.
  else if (ctx->field == CH_chapterId) ctx->meta->chapterId = static_cast<uint32_t>(toLong(v, len));
  ctx->field = CH_None;
}
void chChunk(void* c, const char* v, size_t len, bool finalChunk) { chValue(c, v, len, finalChunk); }

Status transportStatus(int status, bool overflow) {
  if (overflow) return Status::TooLarge;
  if (status == kTransportNoNetwork) return Status::NoNetwork;
  if (status == kTransportAborted) return Status::Cancelled;
  if (status < 0) return Status::Network;
  if (status != 200) return Status::Network;
  return Status::Ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// Response routing
//
// JJWXC answers in one of two shapes and the choice is per-response, not
// per-endpoint: either plain JSON, or the "dynamic envelope" where the whole
// body is one Base64 DES ciphertext keyed from the accesskey/keystring
// response headers. HTTP delivers headers before the body, so the first body
// chunk can pick the path:
//
//   plain    -> stream straight into the SAX parser, zero buffering
//   envelope -> accumulate into the caller's PSRAM scratch, decode in place,
//               then hand the plaintext to the same consumer
// ---------------------------------------------------------------------------

namespace {

struct RouterCtx {
  uint8_t* scratch = nullptr;
  size_t scratchCap = 0;
  size_t len = 0;
  bool envelope = false;
  bool decided = false;
  bool overflow = false;
  HeaderCapture hdrs;
};

void routerHeader(void* c, const char* name, size_t nameLen, const char* value, size_t valueLen) {
  headerCaptureSink(&static_cast<RouterCtx*>(c)->hdrs, name, nameLen, value, valueLen);
}

// `FeedFn` is any callable with signature bool(const char*, size_t): it receives
// JSON text, either incrementally as it arrives or in one piece after envelope
// decryption.
template <typename FeedFn>
struct Bridge {
  RouterCtx* rc;
  FeedFn feed;

  static bool body(void* c, const uint8_t* data, size_t len) {
    auto* self = static_cast<Bridge*>(c);
    RouterCtx* rc = self->rc;
    if (!rc->decided) {
      rc->decided = true;
      rc->envelope = (rc->hdrs.accessKey[0] != '\0' && rc->hdrs.keyString[0] != '\0');
    }
    if (!rc->envelope) return self->feed(reinterpret_cast<const char*>(data), len);
    if (rc->len + len + 1 > rc->scratchCap) {
      rc->overflow = true;
      return false;
    }
    memcpy(rc->scratch + rc->len, data, len);
    rc->len += len;
    rc->scratch[rc->len] = '\0';
    return true;
  }
};

// Issues a GET and delivers the (possibly envelope-decrypted) JSON to `feed`.
template <typename FeedFn>
Status routedGet(Transport& t, const char* url, const RequestHeader* headers, size_t headerCount, const Scratch& scratch,
                 FeedFn feed, bool& usedEnvelope) {
  usedEnvelope = false;
  if (!t.networkReady()) return Status::NoNetwork;

  RouterCtx rc;
  rc.scratch = scratch.data;
  rc.scratchCap = scratch.cap ? scratch.cap - 1 : 0;
  Bridge<FeedFn> bridge{&rc, feed};

  const int status =
      t.request("GET", url, headers, headerCount, nullptr, 0, &Bridge<FeedFn>::body, &bridge, routerHeader, &rc);
  if (rc.overflow) return Status::TooLarge;

  if (rc.envelope) {
    usedEnvelope = true;
    if (!scratch.usable()) return Status::TooLarge;
    size_t plainLen = 0;
    if (!Envelope::decodeInPlace(reinterpret_cast<char*>(scratch.data), rc.len, rc.hdrs.accessKey, rc.hdrs.keyString,
                                 &plainLen)) {
      return Status::Decrypt;
    }
    if (!feed(reinterpret_cast<const char*>(scratch.data), plainLen)) return Status::Cancelled;
  }

  if (status == kTransportAborted) return Status::Cancelled;
  return transportStatus(status, false);
}

// Wraps a sink so it stops after `max` records. Hitting the cap is a normal
// end-of-stream, not an error: `capped` distinguishes it from a user cancel.
template <typename Entry, typename Sink>
struct CapSink {
  Sink inner;
  void* innerCtx;
  size_t max;
  size_t seen = 0;
  bool capped = false;

  static bool call(void* c, const Entry& e, size_t index) {
    auto* self = static_cast<CapSink*>(c);
    if (self->seen >= self->max) {
      self->capped = true;
      return false;
    }
    ++self->seen;
    return self->inner(self->innerCtx, e, index);
  }
};

// Login and device-code endpoints live on a different host and expect the
// "Mobile <ts>" UA rather than the JINJIANG one used by content endpoints.
const RequestHeader kLoginHeaders[] = {{"User-Agent", "Mobile 1700000000000"},
                                       {"Referer", "http://android.jjwxc.net?v=402"},
                                       {"Accept", "application/json"}};
const RequestHeader kDeviceCodeHeaders[] = {{"User-Agent", "Mobile 1700000000000"},
                                            {"Content-Type", "application/x-www-form-urlencoded"},
                                            {"Accept", "application/json"}};

Status routedPost(Transport& t, const char* url, const char* postBody, const Scratch& scratch,
                  StreamingJsonParser& parser) {
  if (!t.networkReady()) return Status::NoNetwork;
  RouterCtx rc;
  rc.scratch = scratch.data;
  rc.scratchCap = scratch.cap ? scratch.cap - 1 : 0;
  struct Ctx {
    RouterCtx* rc;
    StreamingJsonParser* parser;
    static bool body(void* c, const uint8_t* data, size_t len) {
      auto* self = static_cast<Ctx*>(c);
      RouterCtx* rc = self->rc;
      if (!rc->decided) {
        rc->decided = true;
        rc->envelope = (rc->hdrs.accessKey[0] != '\0' && rc->hdrs.keyString[0] != '\0');
      }
      if (!rc->envelope) {
        self->parser->feed(reinterpret_cast<const char*>(data), len);
        return !self->parser->hasError();
      }
      if (rc->len + len + 1 > rc->scratchCap) {
        rc->overflow = true;
        return false;
      }
      memcpy(rc->scratch + rc->len, data, len);
      rc->len += len;
      rc->scratch[rc->len] = '\0';
      return true;
    }
  } ctx{&rc, &parser};

  const int status = t.request("POST", url, kDeviceCodeHeaders, 3, postBody, strlen(postBody), Ctx::body, &ctx,
                               routerHeader, &rc);
  if (rc.overflow) return Status::TooLarge;
  if (rc.envelope) {
    if (!scratch.usable()) return Status::TooLarge;
    size_t plainLen = 0;
    if (!Envelope::decodeInPlace(reinterpret_cast<char*>(scratch.data), rc.len, rc.hdrs.accessKey, rc.hdrs.keyString,
                                 &plainLen)) {
      return Status::Decrypt;
    }
    parser.feed(reinterpret_cast<const char*>(scratch.data), plainLen);
  }
  if (status < 0) return (status == kTransportNoNetwork) ? Status::NoNetwork : Status::Network;
  if (status != 200) return Status::Network;
  return parser.hasError() ? Status::Protocol : Status::Ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// High-level operations
// ---------------------------------------------------------------------------

Status login(Transport& t, const char* loginName, const char* password, const char* identifiers,
             const char* checkType, const char* deviceCode, const Scratch& scratch, LoginResult& out) {
  out = LoginResult{};
  out.status = Status::Protocol;
  char url[1024];
  if (!buildLoginUrl(loginName, password, identifiers, checkType, deviceCode, url, sizeof(url))) {
    return (out.status = Status::Protocol);
  }

  LoginResult parsed;
  LoginCtx ctx{&parsed, 0, false, 0, -1};
  JsonCallbacks cb{};
  cb.ctx = &ctx;
  cb.onKey = lgKey;
  cb.onString = lgStr;
  cb.onNumber = lgNum;
  cb.onArrayStart = lgArrStart;
  cb.onArrayEnd = lgArrEnd;
  StreamingJsonParser parser(cb);

  bool enveloped = false;
  auto feed = [&parser](const char* d, size_t n) {
    parser.feed(d, n);
    return !parser.hasError();
  };
  const Status s = routedGet(t, url, kLoginHeaders, 3, scratch, feed, enveloped);
  if (s != Status::Ok) return (out.status = s);

  out = parsed;
  if (out.credentials.token[0] != '\0') {
    out.status = Status::Ok;
    copyField(out.credentials.identifiers, sizeof(out.credentials.identifiers), identifiers, strlen(identifiers));
  } else if (out.status == Status::Protocol) {
    out.status = Status::Server;
  }
  return out.status;
}

Status sendDeviceCode(Transport& t, const char* loginName, const char* checkType, const Scratch& scratch,
                      DeviceCodeResult& out) {
  out = DeviceCodeResult{};
  char url[256], body[256];
  if (!buildDeviceCodeUrl(loginName, checkType, url, sizeof(url))) return (out.status = Status::Protocol);
  if (!buildDeviceCodeBody(loginName, checkType, body, sizeof(body))) return (out.status = Status::Protocol);

  DeviceCodeResult parsed;
  DevCodeCtx ctx{&parsed, 0, 0};
  JsonCallbacks cb{};
  cb.ctx = &ctx;
  cb.onKey = dcKey;
  cb.onString = dcStr;
  cb.onNumber = dcNum;
  cb.onObjectStart = dcObjStart;
  cb.onObjectEnd = dcObjEnd;
  StreamingJsonParser parser(cb);

  const Status s = routedPost(t, url, body, scratch, parser);
  if (s != Status::Ok) return (out.status = s);
  out = parsed;
  return out.status;
}

Status fetchNovelInfo(Transport& t, const Credentials& cred, const char* novelId, const Scratch& scratch,
                      NovelInfo& out, NovelIntroSink introSink, void* introCtx) {
  out = NovelInfo{};
  (void)cred;  // metadata is public; the TLS-free host is used regardless
  char url[320];
  if (!buildNovelInfoUrl(kHostFree, novelId, nullptr, url, sizeof(url))) return Status::Protocol;

  NovelInfoCtx ctx{&out, 0, introSink, introCtx, false};
  JsonCallbacks cb{};
  cb.ctx = &ctx;
  cb.onKey = niKey;
  cb.onString = niStr;
  cb.onNumber = niNum;
  StreamingJsonParser parser(cb);

  bool enveloped = false;
  auto feed = [&parser, &ctx](const char* d, size_t n) {
    if (ctx.abort) return false;
    parser.feed(d, n);
    return !parser.hasError() && !ctx.abort;
  };
  const Status s = routedGet(t, url, apiHeaders, kHeaderCount, scratch, feed, enveloped);
  if (ctx.abort) return Status::Cancelled;
  if (s != Status::Ok) return s;
  if (parser.hasError()) return Status::Protocol;
  return out.novelId[0] != '\0' ? Status::Ok : Status::Protocol;
}

Status fetchChapterList(Transport& t, const Credentials& cred, const char* novelId, ChapterSink sink, void* ctx,
                        size_t maxEntries, const Scratch& scratch, uint32_t& totalCount, uint32_t& vipChapterId) {
  char url[320];
  if (!buildChapterListUrl(kHostFree, novelId, cred.valid() ? cred.token : nullptr, url, sizeof(url))) {
    return Status::Protocol;
  }
  CapSink<ChapterEntry, ChapterSink> cap{sink, ctx, maxEntries, 0, false};
  ChapterListParser listParser(CapSink<ChapterEntry, ChapterSink>::call, &cap);
  listParser.begin();
  if (listParser.failed()) return Status::Protocol;

  bool enveloped = false;
  auto feed = [&listParser](const char* d, size_t n) { return listParser.feed(d, n); };
  Status s = routedGet(t, url, apiHeaders, kHeaderCount, scratch, feed, enveloped);
  listParser.finish();
  totalCount = listParser.totalCount();
  vipChapterId = listParser.vipChapterId();

  if (s == Status::Cancelled) s = cap.capped ? Status::Ok : Status::Cancelled;
  if (s != Status::Ok) return s;
  return listParser.failed() ? Status::Protocol : Status::Ok;
}

Status fetchShelf(Transport& t, const Credentials& cred, ShelfSink sink, void* ctx, size_t maxEntries,
                  const Scratch& scratch) {
  if (!cred.valid()) return Status::NeedLogin;
  char url[320];
  if (!buildShelfUrl(cred.token, url, sizeof(url))) return Status::Protocol;

  CapSink<ShelfEntry, ShelfSink> cap{sink, ctx, maxEntries, 0, false};
  ShelfParser shelfParser(CapSink<ShelfEntry, ShelfSink>::call, &cap);
  shelfParser.begin();
  if (shelfParser.failed()) return Status::Protocol;

  bool enveloped = false;
  auto feed = [&shelfParser](const char* d, size_t n) { return shelfParser.feed(d, n); };
  Status s = routedGet(t, url, apiHeaders, kHeaderCount, scratch, feed, enveloped);
  shelfParser.finish();

  if (s == Status::Cancelled) s = cap.capped ? Status::Ok : Status::Cancelled;
  if (s != Status::Ok) return s;
  // A rejected credential arrives as a top-level code with no entries at all.
  // Without this the caller cannot tell it from an account with an empty shelf,
  // and probeTokenAlive() -- which uses exactly this reply as its oracle -- would
  // report a dead session as a live one.
  if (shelfParser.hasCode() && statusFromCode(shelfParser.businessCode()) != Status::Ok) {
    return statusFromCode(shelfParser.businessCode());
  }
  return shelfParser.failed() ? Status::Protocol : Status::Ok;
}

Status fetchSearch(Transport& t, const Credentials& cred, const char* keyword, SearchSink sink, void* ctx,
                   size_t maxEntries, const Scratch& scratch, uint32_t& totalCount) {
  if (!cred.valid()) return Status::NeedLogin;
  char url[512];
  if (!buildSearchUrl(keyword, cred.token, url, sizeof(url))) return Status::Protocol;

  CapSink<SearchItem, SearchSink> cap{sink, ctx, maxEntries, 0, false};
  SearchParser searchParser(CapSink<SearchItem, SearchSink>::call, &cap);
  searchParser.begin();
  if (searchParser.failed()) return Status::Protocol;

  bool enveloped = false;
  auto feed = [&searchParser](const char* d, size_t n) { return searchParser.feed(d, n); };
  Status s = routedGet(t, url, apiHeaders, kHeaderCount, scratch, feed, enveloped);
  searchParser.finish();
  totalCount = searchParser.totalCount();

  if (s == Status::Cancelled) s = cap.capped ? Status::Ok : Status::Cancelled;
  if (s != Status::Ok) return s;
  return searchParser.failed() ? Status::Protocol : Status::Ok;
}

namespace {

// The liveness probe wants the reply's verdict, not its contents, so the single
// entry it is allowed to deliver is dropped here. Returning `true` matters: the
// CapSink's own cap is what stops the transfer, and `capped` is what turns that
// stop into Status::Ok instead of Status::Cancelled (fetchShelf's tail).
bool discardShelfEntry(void*, const ShelfEntry&, size_t) { return true; }

}  // namespace

Status probeTokenAlive(Transport& t, const Credentials& cred, const Scratch& scratch) {
  if (!cred.valid()) return Status::NeedLogin;
  // One entry is enough for a verdict and the cap aborts the download there, so a
  // shelf with hundreds of favourites costs one small response rather than all of
  // them. `favorite` is on the same host and session as chapterContent, so the
  // probe reuses the warm TLS connection instead of paying a second handshake.
  const Status s = fetchShelf(t, cred, &discardShelfEntry, nullptr, 1, scratch);
  if (s == Status::Ok) return Status::Ok;
  // 1004 on an authed non-chapter endpoint is unambiguous: this is the "登入验证
  // 失败!" reply, not a per-chapter purchase question.
  if (s == Status::NeedLogin) return Status::NeedLogin;
  // Anything else (transport, server, decrypt, no network) proves nothing about
  // the credential, and the caller is documented to not guess.
  return s;
}

RefusalVerdict classifyChapterRefusal(const Status status, const long businessCode, const bool chapterIsVip,
                                      const TokenLiveness liveness) {
  // No business code at all. For the App API that means a transport, decrypt or
  // protocol failure -- not a property of this chapter, so the run stops and reports
  // the real cause.
  //
  // WAP has no business codes whatsoever: an unreadable VIP chapter is a 200 HTML page
  // with no contentvars in it, and the caller reports that as PurchaseRequired with
  // businessCode left at 0. Treating 0 as "always fatal" therefore aborted a whole-book
  // cache at the first unpurchased chapter -- the exact P0 this function was written to
  // fix, arriving again through the new path. So when there is no code, the status is
  // the only evidence available and it gets used.
  if (businessCode == 0) {
    switch (status) {
      // The service said this account may not read this chapter. Per-chapter.
      case Status::PurchaseRequired:
        return RefusalVerdict::Unpurchased;
      // The session is gone. Every remaining chapter would fail the same way, so
      // stopping and asking the user to sign in again is the only useful answer.
      case Status::NeedLogin:
        return RefusalVerdict::NeedLogin;
      default:
        return RefusalVerdict::Other;
    }
  }

  // A code that every chapter would trip over. Skipping it would only fill the
  // book with placeholders, so it stops the run.
  if (codeIsClientLevel(businessCode)) return RefusalVerdict::Other;

  if (businessCode == 1004) {
    // 1004 on a free chapter: chapterContent serves free chapters even to a dead
    // token (verified live), so this is never a purchase question. Reporting it as
    // "not purchased" would put a lie in the book; reporting the session as dead is
    // the honest reading of a refusal the service should not have issued.
    if (!chapterIsVip) return RefusalVerdict::NeedLogin;
    // 1004 on a paid chapter is the ambiguous case -- the same code and the same
    // body mean "session over" and "not bought" -- and the probe is the only thing
    // that resolves it.
    switch (liveness) {
      case TokenLiveness::Alive: return RefusalVerdict::Unpurchased;
      case TokenLiveness::Dead: return RefusalVerdict::NeedLogin;
      case TokenLiveness::Inconclusive: break;
    }
    return RefusalVerdict::Inconclusive;
  }

  // 70200 / 1013 / 1010: the service said, in so many words, that this account may
  // not read this chapter right now. Per-chapter by definition.
  if (codeIsChapterRefusal(businessCode)) return RefusalVerdict::Unpurchased;

  // Anything else is a code nobody has seen before. The two most recent surprises
  // (1013, 1010) were both purchase refusals wearing an unknown number, so
  // per-chapter is the likelier reading -- but the caller must bound the damage,
  // because an unknown rate-limit or client rejection would otherwise be stamped
  // into every remaining chapter's placeholder.
  return RefusalVerdict::UnknownRefusal;
}

bool refusalEndsRun(const RefusalVerdict verdict, const bool servedFromWap) {
  // Only a refusal the classifier read as "this account may not read this chapter"
  // is a purchase question at all. An unknown code is not: it may be a rate limit or
  // a client rejection wearing an unfamiliar number, and the caller bounds that with
  // its own streak guard instead of ending the run on the first one.
  if (verdict != RefusalVerdict::Unpurchased) return false;
  // WAP answers an unpurchased VIP chapter with 302 -> /buy/<nid>/<cid>: it declines
  // and points at the purchase page, so nothing was bought and nothing could have
  // been. Per-chapter, and the run carries on.
  //
  // chapterContent is the path that can charge the account, because requesting a VIP
  // chapter there runs the account's server-side auto-subscription (measured: 1013
  // "余额不足，自动购买失败" and the string "1010"). A purchase refusal on that path
  // ends the run rather than repeating the attempt for every remaining chapter.
  return !servedFromWap;
}

Status fetchChapter(Transport& t, const Credentials& cred, const char* novelId, uint32_t chapterId,
                    const Scratch& scratch, ChapterMeta& outMeta, TextSink onText, void* textCtx, TextSink onSayBody,
                    void* sayCtx) {
  outMeta = ChapterMeta{};
  if (!scratch.usable()) return Status::TooLarge;

  // Token-bearing requests must stay on HTTPS; free chapters use the plaintext
  // host and skip TLS entirely.
  const bool authed = cred.valid();
  const char* host = authed ? kHostApi : kHostFree;
  const char* token = authed ? cred.token : "";
  char url[384];
  if (!buildChapterContentUrl(host, novelId, chapterId, token, url, sizeof(url))) {
    return Status::Protocol;
  }

  ChapterParseCtx ctx;
  ctx.meta = &outMeta;
  ctx.textSink = onText;
  ctx.textCtx = textCtx;
  ctx.saySink = onSayBody;
  ctx.sayCtx = sayCtx;
  // buildChapterContentUrl adds `versionCode` exactly when the token is
  // non-empty, and the server returns the encryptType:"jj" form exactly when
  // `versionCode` is present (verified live on both hosts, with and without a
  // token). That makes our own request the authority for the response encoding
  // in the common case, where `encryptField`/`encryptType` are only parsed
  // *after* `content` has already streamed.
  ctx.expectEncrypted = (token[0] != '\0');

  JsonCallbacks cb{};
  cb.ctx = &ctx;
  cb.onKey = chKey;
  cb.onString = chStr;
  cb.onNumber = chNum;
  cb.onStringChunk = chChunk;
  StreamingJsonParser parser(cb);

  bool enveloped = false;
  auto feed = [&parser](const char* d, size_t n) {
    parser.feed(d, n);
    return !parser.hasError();
  };
  const Status s = routedGet(t, url, apiHeaders, kHeaderCount, scratch, feed, enveloped);

  // Report what actually reached the caller's sinks on EVERY path, including
  // the error ones: the download loop truncates the file back by this much, so
  // a rejected chapter cannot keep bytes that streamed in before its `code` was
  // parsed. Must be set before the early returns below.
  outMeta.textBytes = ctx.emittedText;
  outMeta.sayBodyBytes = ctx.emittedSay;
  // Same contract: the raw code is reported on every path so a caller can decide
  // per chapter and log a number instead of a catch-all label.
  outMeta.businessCode = ctx.hasCode ? ctx.code : 0;
  outMeta.hasBusinessCode = ctx.hasCode;

  if (s != Status::Ok) return s;
  if (parser.hasError() || ctx.failed) return Status::Decrypt;
  if (ctx.hasCode && ctx.code != 0) return statusFromCode(ctx.code);
  if (outMeta.textBytes == 0) return Status::Protocol;
  return Status::Ok;
}

}  // namespace jjwxc
