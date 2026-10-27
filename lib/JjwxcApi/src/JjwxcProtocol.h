#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <StreamingJsonParser.h>

#include "JjwxcCrypto.h"
#include "JjwxcTransport.h"

namespace jjwxc {

// ---------------------------------------------------------------------------
// Endpoint constants. Every one of these was verified against live traffic; see
// docs/06-jjwxc-verified-endpoints.md. Do not "clean up" the spelling of
// kUserAgent: `JINJIANG` is JJWXC's own typo and the server matches on it. A UA
// of `Mobile <timestamp>` makes chapterContent fail with code 2016.
// ---------------------------------------------------------------------------

// Free content needs no token, and app-cdn does not validate the UA at all.
// It still has to be HTTPS: the firmware's HTTP client is wolfSSL-backed and
// cannot speak plain HTTP, so an http:// URL fails immediately with a transport
// error rather than falling back. Anything carrying a token uses kHostApi.
constexpr const char* kHostFree = "https://app-cdn.jjwxc.net";
constexpr const char* kHostApi = "https://app.jjwxc.net";
constexpr const char* kHostLogin = "https://app.jjwxc.org";

constexpr const char* kUserAgent = "JINJIANG-Android/381 CrossMux-JJWXC/1.0";
constexpr const char* kReferer = "http://android.jjwxc.net?v=381";

// 381 is the "legacy, signature-free" channel. Larger values (e.g. 99999) make
// the server demand a `sign` parameter that is an encrypted bundle requiring an
// APK reverse; 381 with a token does not.
constexpr const char* kVersionCode = "381";
constexpr const char* kLoginVersionCode = "402";
constexpr const char* kDeviceCodeVersionCode = "401";

// Hardcoded in the official Android client; used for the login password/sign
// envelope and for the inner `content` field encryption.
constexpr const char* kDesKey = "KW8Dvm2N";
constexpr const char* kDesIv = "1ae2c94b";

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

enum class Status : uint8_t {
  Ok = 0,
  // 1004. AMBIGUOUS BY SERVER DESIGN: chapterContent uses it both for a rejected
  // token and for a VIP chapter this account has not bought, and the two replies
  // are byte-identical. Callers that must tell them apart need probeTokenAlive().
  NeedLogin,
  NeedDeviceCode,   // 221003: login requires an email/SMS verification code
  BadCredentials,   // wrong account or password
  BadUserAgent,     // 2016
  BadVersion,       // 231101
  WrongCredentialSource,  // 231102: a WAP sid was used against the app API
  SignError,        // 1088 / 1002
  EmptyResult,      // 1019 / 1055
  // 70200 and friends. Observed on the `buy` endpoint only
  // (docs/00-feasibility-report.md: `buy` -> `70200 对不起，请先输入支付密码`);
  // chapterContent has never been seen to return it, so an unpurchased chapter
  // arrives as NeedLogin, not as PurchaseRequired.
  PurchaseRequired,
  Server,           // any other non-zero JJWXC code
  Network,          // transport failure
  Protocol,         // malformed JSON / unexpected shape
  Decrypt,          // envelope or field decryption failed
  TooLarge,         // response exceeded the caller's cap
  NoNetwork,        // Wi-Fi not ready
  Cancelled,        // user aborted
};

const char* statusText(Status s);

// Maps a JJWXC numeric `code` onto Status. `code == 0` means success.
Status statusFromCode(long code);

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

struct Credentials {
  // "<8-digit readerId>_<32 hex>" -- 41 chars, passed as a URL query parameter
  // (not a cookie). Lifetime is not published; treat 1004 as "re-login".
  char token[48] = {};
  // "<20 digits>:<32 hex>d4:" device fingerprint. Persisted and reused: sending
  // a fresh one every login makes the server treat each attempt as a new device
  // and demand a verification code every time.
  char identifiers[64] = {};
  char nickName[64] = {};
  char readerId[16] = {};

  bool valid() const { return token[0] != '\0'; }
  bool hasIdentifiers() const { return identifiers[0] != '\0'; }
};

// What one `novelbasicinfo` reply carries, minus the blurb.
//
// The service returns 66 keys (measured 2026-10-15, novelId=<novelId>, anonymous,
// 5,494 bytes); these are the ones a reader UI can do something with. Every field
// is optional on the wire: a blocked or half-populated book omits keys rather than
// sending them empty, so each one is copied through copyField() and callers must
// treat '\0' as "not available" rather than as a value.
//
// The long blurb (`novelIntro`, 928 characters / 1,868 UTF-8 bytes for that same
// book) is deliberately NOT a member. A fixed buffer big enough for it would sit in
// internal RAM for the whole life of the activity, which is the one thing this
// project's memory discipline forbids; it arrives through NovelIntroSink instead.
struct NovelInfo {
  char novelId[16] = {};
  char novelName[96] = {};
  char authorName[48] = {};
  char authorId[16] = {};
  char novelClass[48] = {};     // "原创-言情-古色古香-爱情-女主"
  char novelTags[96] = {};      // comma-separated, e.g. "灵异神怪,种田文,爽文"
  char sizeText[24] = {};       // novelsizeformat, e.g. "12.92万"
  char lastChapterName[64] = {};
  char renewDate[24] = {};
  char coverUrl[192] = {};  // novelCover, the _300_420 thumbnail; originalCover is 426 KB
  // `novelIntroShort`: the author's one-line tagline ("警惕修仙陷阱，从你我做起。"),
  // NOT an abbreviation of novelIntro. The two are independent fields and a book can
  // have either, both or neither.
  char introShort[96] = {};
  uint32_t chapterCount = 0;
  uint32_t maxChapterId = 0;
  uint32_t vipChapterId = 0;  // first VIP chapter; 0 when the book is all-free
  uint32_t renewChapterId = 0;
  bool isVip = false;
};

struct ChapterEntry {
  uint32_t chapterId = 0;
  char name[96] = {};
  char date[24] = {};
  uint32_t size = 0;
  uint8_t isVip = 0;  // 0 = free, 2 = VIP
  uint32_t price = 0;      // `point`, discounted price in JJWXC coins
  uint32_t listPrice = 0;  // `originalPrice`
  bool isLock = false;
};

struct ShelfEntry {
  char novelId[16] = {};
  char novelName[96] = {};
  char authorName[48] = {};
  char coverUrl[192] = {};
  char lastChapterName[64] = {};
  uint32_t chapterCount = 0;
  uint32_t lastChapterId = 0;
  uint32_t lastChapterTime = 0;  // unix seconds
  uint8_t renewStatus = 0;       // non-zero => the book has unread updates
};

struct SearchItem {
  char novelId[16] = {};
  char novelName[96] = {};
  char authorName[48] = {};
  char sizeText[24] = {};
  char introShort[96] = {};
  bool isVipMonth = false;
};

struct LoginResult {
  Status status = Status::Ok;
  Credentials credentials;
  // Populated when status == NeedDeviceCode: which channels the account offers.
  bool offersEmail = false;
  bool offersPhone = false;
  // The raw business code, exactly as the service sent it. `status` is lossy:
  // statusFromCode() folds every code it does not know into Status::Server, so
  // without this a rejection the library has not seen before is indistinguishable
  // from "the server is broken" -- which has now sent four debugging sessions in
  // the wrong direction (code 200, then 1013, then the string "1010", then this).
  long code = 0;
  char message[128] = {};
};

struct DeviceCodeResult {
  Status status = Status::Ok;
  char message[160] = {};
};

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------

// `index` is 0-based. The response's own `count` field arrives after the
// array, so the total is only available via ChapterListParser::totalCount().
using ChapterSink = bool (*)(void* ctx, const ChapterEntry& entry, size_t index);
using ShelfSink = bool (*)(void* ctx, const ShelfEntry& entry, size_t index);
using SearchSink = bool (*)(void* ctx, const SearchItem& item, size_t index);

// The long blurb, `novelIntro`. Delivered raw and at most once per reply, so the
// caller decides where the bytes live (PSRAM, in this project) instead of this
// struct carrying a multi-kilobyte buffer in internal RAM.
//
// RAW means HTML-escaped exactly as the service sent it: the line breaks are the
// seven-character sequence `&lt;br/&gt;`, and `&amp;`/`&nbsp;` appear as themselves.
// Unescaping is the caller's job and the two functions that already do it for WAP
// chapter bodies are the right ones, in this order:
//   len = wapDecodeEntities(buf, len);   // &lt;br/&gt; -> <br/>
//   len = wapHtmlToText(buf, len);       // <br/> -> '\n', tags dropped, ends trimmed
// Both work in place and never grow the buffer, so one allocation covers it.
//
// Returning false aborts the fetch, which surfaces as Status::Cancelled.
using NovelIntroSink = bool (*)(void* ctx, const char* value, size_t len);

// ---------------------------------------------------------------------------
// URL building (exposed for tests)
// ---------------------------------------------------------------------------

size_t urlEncode(const char* in, char* out, size_t outSize);

// `identifiers` is generated once and persisted by the caller.
bool buildLoginUrl(const char* loginName, const char* password, const char* identifiers, const char* checkType,
                   const char* deviceCode, char* out, size_t outSize);

bool buildDeviceCodeUrl(const char* loginName, const char* checkType, char* out, size_t outSize);
bool buildDeviceCodeBody(const char* loginName, const char* checkType, char* out, size_t outSize);

// `token` may be empty for free chapters, in which case `host` should be
// kHostFree; the request still uses TLS, it just carries no credential.
bool buildChapterContentUrl(const char* host, const char* novelId, uint32_t chapterId, const char* token, char* out,
                            size_t outSize);
bool buildChapterListUrl(const char* host, const char* novelId, const char* token, char* out, size_t outSize);
bool buildNovelInfoUrl(const char* host, const char* novelId, const char* token, char* out, size_t outSize);
bool buildShelfUrl(const char* token, char* out, size_t outSize);
bool buildSearchUrl(const char* keyword, const char* token, char* out, size_t outSize);

// ---------------------------------------------------------------------------
// Streaming helpers
// ---------------------------------------------------------------------------

// Parses a `chapterList` response body incrementally. Feed it raw bytes; it
// emits one ChapterEntry per element of the `chapterlist` array.
class ChapterListParser {
 public:
  ChapterListParser(ChapterSink sink, void* ctx) : sink_(sink), ctx_(ctx) {}
  void begin();
  bool feed(const char* data, size_t len);
  bool finish();
  uint32_t totalCount() const { return totalCount_; }
  uint32_t vipChapterId() const { return vipChapterId_; }
  size_t emitted() const { return emitted_; }
  bool failed() const { return failed_; }

 private:
  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);

  void emit();

  ChapterSink sink_;
  void* ctx_;
  std::unique_ptr<StreamingJsonParser> parser_;
  ChapterEntry cur_{};
  // depth_ counts *objects* only: arrays are tracked via inList_. With that
  // convention the root object is depth 1 and each chapter record is depth 2,
  // which is what chapterListFieldFor() matches against. Nested objects inside
  // a record (draft, manageExplain) push depth to 3 and are ignored.
  int depth_ = 0;
  // Records contain their own arrays (`manageExplain: []`, `yellowcard`, ...),
  // so inList_ must only clear when the *chapterlist* array itself closes --
  // hence the separate array depth and the depth it was opened at.
  int arrayDepth_ = 0;
  int listArrayDepth_ = -1;
  bool inList_ = false;
  bool inItem_ = false;
  bool pendingArray_ = false;
  bool failed_ = false;
  bool stop_ = false;
  uint32_t totalCount_ = 0;
  uint32_t vipChapterId_ = 0;
  size_t emitted_ = 0;
  // Key of the value currently being delivered.
  uint8_t field_ = 0;
};

// Same shape for `favorite` (shelf) and `search`.
class ShelfParser {
 public:
  ShelfParser(ShelfSink sink, void* ctx) : sink_(sink), ctx_(ctx) {}
  void begin();
  bool feed(const char* data, size_t len);
  bool finish();
  size_t emitted() const { return emitted_; }
  bool failed() const { return failed_; }
  // The response's own top-level business code. `favorite` reports a rejected
  // credential as `{"code":1004,"message":"1004:登入验证失败!"}` with no data
  // array at all, which a parser that only ever looked at entries would report as
  // "an empty shelf" -- indistinguishable from an account with no favourites.
  // False when the reply carried no code, which a successful one may not.
  bool hasCode() const { return hasCode_; }
  long businessCode() const { return code_; }

 private:
  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);

  ShelfSink sink_;
  void* ctx_;
  std::unique_ptr<StreamingJsonParser> parser_;
  void emit();

  ShelfEntry cur_{};
  // See ChapterListParser: depth_ counts objects only, arrays are tracked
  // separately because records contain nested arrays of their own.
  int depth_ = 0;
  int arrayDepth_ = 0;
  int listArrayDepth_ = -1;
  bool inList_ = false;
  bool inItem_ = false;
  bool pendingArray_ = false;
  bool failed_ = false;
  bool stop_ = false;
  size_t emitted_ = 0;
  uint8_t field_ = 0;
  long code_ = 0;
  bool hasCode_ = false;
};

class SearchParser {
 public:
  SearchParser(SearchSink sink, void* ctx) : sink_(sink), ctx_(ctx) {}
  void begin();
  bool feed(const char* data, size_t len);
  bool finish();
  size_t emitted() const { return emitted_; }
  uint32_t totalCount() const { return totalCount_; }
  bool failed() const { return failed_; }

 private:
  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);

  SearchSink sink_;
  void* ctx_;
  std::unique_ptr<StreamingJsonParser> parser_;
  void emit();

  SearchItem cur_{};
  // See ChapterListParser: depth_ counts objects only, arrays are tracked
  // separately because records contain nested arrays of their own.
  int depth_ = 0;
  int arrayDepth_ = 0;
  int listArrayDepth_ = -1;
  bool inList_ = false;
  bool inItem_ = false;
  bool pendingArray_ = false;
  bool failed_ = false;
  bool stop_ = false;
  size_t emitted_ = 0;
  uint32_t totalCount_ = 0;
  uint8_t field_ = 0;
};

// Parses the small single-object responses (novelbasicinfo, login).
// The intro sink is the buffer-fed twin of fetchNovelInfo's; a refusal makes this
// return false rather than reporting a cancellation, since there is no download to
// stop.
bool parseNovelInfo(const char* json, size_t len, NovelInfo& out, NovelIntroSink introSink = nullptr,
                    void* introCtx = nullptr);
bool parseLogin(const char* json, size_t len, LoginResult& out);
bool parseDeviceCode(const char* json, size_t len, DeviceCodeResult& out);

// ---------------------------------------------------------------------------
// High-level operations
// ---------------------------------------------------------------------------

// Fills `identifiers` with a fresh "<20 digits>:<32 hex>d4:" fingerprint.
// `random32` should be a hardware RNG when available (esp_random on device).
void makeIdentifiers(uint32_t (*random32)(), char out[64]);

// Every operation takes a caller-owned scratch buffer instead of allocating
// one, because the internal SRAM budget is tiny (~30 KB free in practice) while
// PSRAM has megabytes. The app layer allocates ONE PSRAM buffer (512 KB is
// ample) and shares it across calls -- only one request is ever in flight.
//
// Scratch is used only when a response arrives in the "dynamic envelope" shape;
// plain responses are streamed straight into the JSON parser and touch no
// buffer at all.
struct Scratch {
  uint8_t* data = nullptr;
  size_t cap = 0;
  bool usable() const { return data != nullptr && cap >= 8192; }
};

Status login(Transport& t, const char* loginName, const char* password, const char* identifiers,
             const char* checkType, const char* deviceCode, const Scratch& scratch, LoginResult& out);

Status sendDeviceCode(Transport& t, const char* loginName, const char* checkType, const Scratch& scratch,
                      DeviceCodeResult& out);

Status fetchNovelInfo(Transport& t, const Credentials& cred, const char* novelId, const Scratch& scratch,
                      NovelInfo& out, NovelIntroSink introSink = nullptr, void* introCtx = nullptr);

// Streams the full catalogue. `maxEntries` caps how many are delivered.
Status fetchChapterList(Transport& t, const Credentials& cred, const char* novelId, ChapterSink sink, void* ctx,
                        size_t maxEntries, const Scratch& scratch, uint32_t& totalCount, uint32_t& vipChapterId);

Status fetchShelf(Transport& t, const Credentials& cred, ShelfSink sink, void* ctx, size_t maxEntries,
                  const Scratch& scratch);

Status fetchSearch(Transport& t, const Credentials& cred, const char* keyword, SearchSink sink, void* ctx,
                   size_t maxEntries, const Scratch& scratch, uint32_t& totalCount);

// Token-liveness oracle, for callers that must tell "this account has not bought
// this chapter" apart from "this session is over".
//
// `chapterContent` cannot answer that question on its own. Both cases come back
// as the SAME response -- `code:1004`, `message:"该章节为VIP章节，请登入继续操作!"`,
// a 12-character constant in `content` (docs/04-jjwxc-api-spec.md §"VIP 未购/未登录"
// treats the two as one shape, and the reference client can only separate them by
// matching the message text, never by a code). Nor does a dead token make
// `chapterContent` refuse a FREE chapter: re-requesting a free chapter with an
// expired token still returns the full body with no `code` at all, so a free
// chapter is not a liveness probe either.
//
// An authed non-chapter endpoint does answer it: `favorite` replies
// `1004 登入验证失败!` for a credential the server rejects and `0` for one it
// accepts. This asks for a single entry, so the cap aborts the transfer at once.
//
// Returns Ok when the server accepted the credential, NeedLogin when it rejected
// it, and the transport/server status when the reply proves nothing either way --
// a caller must not guess in that case, because guessing "not purchased" stamps a
// wrong placeholder into the cache and guessing "expired" sends the user to
// re-login with a working token.
Status probeTokenAlive(Transport& t, const Credentials& cred, const Scratch& scratch);

// What a caller concluded from probeTokenAlive(). Kept in the library so the
// refusal rule below is a pure function that the host tests can pin down without
// the firmware's Activity, HAL or storage.
enum class TokenLiveness : uint8_t {
  Alive,          // the server accepted the credential
  Dead,           // the server rejected it
  Inconclusive,   // the probe itself failed; its status is the one to report
};

// What to do about ONE chapter that fetchChapter refused.
enum class RefusalVerdict : uint8_t {
  // Not ours to read: write the placeholder, record the chapter, and CONTINUE with
  // the next one. A whole-book cache must never fail because of these.
  Unpurchased,
  // The session is over: stop the run and tell the user to sign in again. Reporting
  // this as "not purchased" would fill the cache with placeholders for chapters the
  // user already owns.
  NeedLogin,
  // The liveness probe proved nothing. Stop the run and report the probe's own
  // status rather than guessing in either direction.
  Inconclusive,
  // A business code this library does not recognise. Per-chapter is the likelier
  // reading -- 1013 and 1010 were both unknown codes that turned out to mean "not
  // purchased" -- so the caller skips the chapter, but it must count consecutive
  // ones and give up after a few: an unknown code that is really a rate limit or a
  // client rejection would otherwise stamp a whole book with wrong placeholders.
  UnknownRefusal,
  // Not a refusal at all (transport, decrypt, protocol, ...): stop the run and
  // report the chapter's own status.
  Other,
};

// True when a business code cannot be a property of one chapter, so a batch run
// must stop at the first one instead of skipping it.
bool codeIsClientLevel(long code);
// True for the codes the service uses to say "you may not read this chapter" and
// that need no further interpretation. 1004 is excluded: it is ambiguous.
bool codeIsChapterRefusal(long code);

// Pure decision rule. `status` is what fetchChapter returned, `businessCode` is the
// raw code from ChapterMeta (0 when the reply carried none), `chapterIsVip` is
// whether the catalogue says the chapter is paid, and `liveness` is the verdict
// from probeTokenAlive. `liveness` is only consulted for 1004 on a paid chapter;
// pass Alive as the don't-care value when no probe was run.
//
// The rule follows from facts measured against the live service:
//   * chapterContent refuses an unpurchased VIP chapter and a dead-token VIP
//     chapter with the SAME code and body (1004), so `status` alone cannot decide;
//   * chapterContent still serves a FREE chapter to a dead token, so a refusal on a
//     free chapter is never a purchase question;
//   * `favorite` rejects a dead token and accepts a live one, so it can decide;
//   * "not purchased" is not one code: 1013 (auto-subscription failed, balance too
//     low) and 1010 (already subscribed / order in flight) were both observed with
//     a valid token, and 1010 arrives as a string.
RefusalVerdict classifyChapterRefusal(Status status, long businessCode, bool chapterIsVip, TokenLiveness liveness);

// Second half of the same decision, and a pure function for the same reason: the
// host tests pin it instead of a copy of it. Only consulted for a verdict that
// classifyChapterRefusal() called per-chapter (Unpurchased / UnknownRefusal) --
// every other verdict already ends the run on its own.
//
// `servedFromWap` is the path the body was actually requested from, i.e. the
// catalogue's own isVip flag, which is what selects between /vip/<nid>/<cid> and
// chapterContent. It is NOT a user preference and NOT chapterLooksVip(): the
// question is where the request went, because only one of the two paths can spend
// money.
//
//   * WAP declines an unpurchased VIP chapter with 302 -> /buy/<nid>/<cid>. It
//     serves a pointer to the purchase page and nothing else; no request the device
//     can make buys anything. Such a refusal is per-chapter information, so the run
//     writes a placeholder and carries on. Ending it there instead made a
//     selectively purchased book uncachable (measured: 176 of 195 chapters never
//     asked about, including ones the account had paid for).
//   * chapterContent, by contrast, runs the account's own server-side
//     auto-subscription: requesting the chapter IS the purchase attempt (measured:
//     code 1013 "余额不足，自动购买失败", code "1010" already subscribed). A refusal
//     arriving on that path means the service tried to charge the account, so the
//     run stops and says where, rather than quietly firing the same attempt at every
//     remaining chapter.
//
// UnknownRefusal is deliberately excluded even on the App API path: an unknown code
// is not evidence of a purchase attempt, and the caller's bounded streak guard is
// what stops a rate limit from being stamped into the rest of the book.
bool refusalEndsRun(RefusalVerdict verdict, bool servedFromWap);

struct ChapterMeta {
  // The reply's own echo of the chapter it served, 0 when the reply did not carry one.
  //
  // This exists because a routing bug once wrote the wrong chapter's prose into a book
  // with no error anywhere: a tap on a scrolled list selected the row at the screen
  // slot instead of the absolute row, and since both test novels start their VIP
  // section at chapter 21 while only ~9 rows fit on screen, every mis-selected chapter
  // was an early free one -- which fetched fine and looked like success. Comparing this
  // against the requested id turns that class of bug from silent wrong text into a
  // refusal. It is not a substitute for fixing the routing; it is the check that would
  // have caught it.
  uint32_t chapterId = 0;
  char name[96] = {};
  char date[24] = {};
  uint32_t size = 0;
  bool isVip = false;
  // Bytes actually delivered to onText / onSayBody. Set on EVERY return path,
  // including failures: a caller that streams straight into a file must roll
  // back this many bytes when the status is not Ok, because the body is emitted
  // while the response is still being parsed and a rejected chapter's `content`
  // can therefore arrive before its `code` does.
  size_t textBytes = 0;
  size_t sayBodyBytes = 0;
  // The reply's own business code, kept raw. statusFromCode() maps every code it
  // does not know onto Status::Server, and that catch-all has repeatedly hidden the
  // real answer (200 read as a failure, then 1013/1010 read as "the server is
  // broken"). A caller that has to decide per chapter, or show a diagnosable
  // message, needs the number the service actually sent. Set on every return path,
  // like textBytes.
  long businessCode = 0;
  bool hasBusinessCode = false;
};

// Fetches one chapter and streams the decoded UTF-8 body through `onText`.
// `scratch`/`scratchCap` hold the raw response (PSRAM-backed on device); the
// decoded text is never buffered in full.
//
// Handles both response shapes seen in the wild: a plain JSON body, and the
// "dynamic envelope" where the whole body is one Base64 DES ciphertext keyed
// from the `accesskey`/`keystring` response headers. Also unwraps the inner
// per-field DES encryption flagged by `encryptType:"jj"`.
//
// A chapter the account may not read (VIP and unpurchased, or VIP with an
// expired token) answers with a business `code` -- 1004 in every case observed,
// with 70200 reserved for the `buy` endpoint -- plus a `content` field holding a
// server constant that decrypts to the empty string. Such a response is reported
// as NeedLogin (or PurchaseRequired for 70200) and delivers ZERO bytes to
// `onText`; the caller is expected to leave a placeholder instead. Because one
// code covers both "not bought" and "session over", a caller that has to react
// differently must disambiguate with probeTokenAlive() rather than with the
// status alone.
Status fetchChapter(Transport& t, const Credentials& cred, const char* novelId, uint32_t chapterId,
                    const Scratch& scratch, ChapterMeta& outMeta, TextSink onText, void* textCtx, TextSink onSayBody,
                    void* sayCtx);

}  // namespace jjwxc
