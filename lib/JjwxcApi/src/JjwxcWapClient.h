// Cookie-aware HTTP for JJWXC's WAP endpoints (m.jjwxc.net).
//
// This exists because the firmware's HTTP client has neither cookie handling nor
// redirect following -- WeRead's API is token-in-query and answers 200, so nothing
// upstream ever needed them. WAP needs both: the session IS a `sid` cookie, and
// every interesting outcome of a login is expressed as a 302.
//
// It sits in the library rather than in JjwxcHttpTransport so the redirect loop and
// the cookie plumbing can be pinned by a host test against a mock Transport. That
// matters more here than elsewhere in this project: the flow it serves is a login,
// the account is under escalated risk control, and each wrong guess costs a real
// attempt against a real account.
//
// Redirects are OFF by default. That is deliberate, not a missing feature: a dead
// `sid` makes /my/fav answer 302 -> /my/login, and a client that silently followed
// would hand the caller a 200 login page that parses as an empty shelf. The caller
// has to be able to see the 302 to tell "no books" apart from "logged out", so it
// opts in per request.
//
// No heap, no STL containers, no Arduino/ESP/wolfSSL headers: same rules as the rest
// of this library, so it builds and runs on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "JjwxcCookies.h"
#include "JjwxcTransport.h"

namespace jjwxc {

// The WAP site is a browser page, not an API, and it is served differently to a
// browser than to anything else. These are the three headers the captured requests
// carried; the UA is mobile Chrome because that is what the login was verified with.
constexpr const char* kWapUserAgent =
    "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile "
    "Safari/537.36";
constexpr const char* kWapAccept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
constexpr const char* kWapAcceptLanguage = "zh-CN,zh;q=0.9";
constexpr const char* kWapFormContentType = "application/x-www-form-urlencoded";

// Sized for what a WAP request actually carries. The cookie header is the only one
// that varies, and it has to hold every cookie the login flow collects at once:
// `sid` (133) plus `JJSESS` (measured 387) plus the small companions, so 1024. A
// header that does not fit is reported through WapResponse::cookieHeaderTruncated
// rather than sent partially -- a request missing `sid` gets a 302 to the login page,
// which reads as "session expired" and sends the user through a login they did not
// need.
struct WapIo {
  // 2048, and the arithmetic behind it: sid (133) + JJSESS (387, measured -- it was 376
  // on an earlier day, so it grows) + JJEVER (62) is already 606 bytes with separators,
  // leaving 418 at the old 1024 for four more cookies including a percent-encoded CJK
  // nickname. buildHeader() STOPS at the first cookie that does not fit, so an overflow
  // here silently drops whichever cookies come after it -- and if sid is one of them,
  // every authenticated request fails in a way that looks like a dead session or a
  // broken network rather than a short buffer. PSRAM has megabytes spare; the risk does
  // not pay for 1 KB.
  static constexpr size_t kCookieCap = 2048;
  // 1024 was enough for the read endpoints (base64(sid) + ids + paging). A new
  // paragraph comment is submitted as a GET with the body encoded twice, so the
  // site's 300-character allowance expands to roughly 4.5 KB of URL for pure
  // CJK (each byte becomes "%25XX"), and the request would otherwise fail with
  // kTransportTooLarge before leaving the device. 12 KiB leaves a wide margin
  // for the author/device fields and the prefix. WapIo lives in PSRAM in both
  // activities that own one; the extra 11 KB is free there.
  static constexpr size_t kUrlCap = 12 * 1024;
  static constexpr size_t kLocationCap = 512;  // a redirect target is also a URL

  char cookie[kCookieCap] = {};
  // Holds the URL of the request in flight, which on a redirect is not the URL the
  // caller passed. Kept here rather than on the stack so the caller can see where the
  // chain ended up, and because a redirect chain needs somewhere to put each hop.
  char url[kUrlCap] = {};
};

// What came back. `status` is the final HTTP status, or a negative TransportError
// from JjwxcTransport.h when the request never completed.
struct WapResponse {
  int status = 0;
  // The Location carried by the response that produced `status`, resolved against
  // that response's own URL. Empty when the final response had no Location, including
  // after a chain that was followed to a 200 -- a stale value here would report a
  // redirect that never happened, and a caller keying "session expired" off it would
  // send the user to log in for no reason.
  char location[WapIo::kLocationCap] = {};
  // The URL that produced `status`. Equals the request URL when nothing
  // redirects. Deliberately NOT sized by WapIo::kUrlCap: the write-comment URL
  // is ~4.5 KB and WapResponse is a stack local in every caller, including the
  // 24 KB PSRAM worker. Tying this to the request buffer put a 12 KB array on
  // that stack (and another in the chained refresh), which overflowed it.
  // 1 KB is enough to spot "/my/login"; a long request URL may be truncated.
  static constexpr size_t kFinalUrlCap = 1024;
  char finalUrl[kFinalUrlCap] = {};
  uint8_t redirectsFollowed = 0;
  uint8_t cookiesStored = 0;
  // Set when a 3xx was seen at any point in the chain, whether or not it was followed.
  // This, not `location`, is the durable "we were redirected" signal: `location`
  // describes only the final response.
  bool sawRedirect = false;
  bool cookieHeaderTruncated = false;
  // The response declared a charset the caller must transcode from. WAP answers
  // GB18030; the app layer transcodes, because that is where the firmware's
  // transcoder and its scratch buffer live.
  bool isGbk = false;
};

struct WapOptions {
  // How many redirects to follow. 0 reports the 3xx instead.
  uint8_t maxRedirects = 0;
  const char* referer = nullptr;
  // Sent as Content-Type on a POST. Null omits the header.
  const char* contentType = nullptr;
  // Delivered the response headers as they arrive, after the jar has taken its
  // Set-Cookie lines. WAP needs none of them today; this is here so a caller that
  // does need one is not forced to reimplement the request.
  HeaderSink onHeader = nullptr;
  void* headerCtx = nullptr;
};

class WapClient {
 public:
  // The jar is borrowed, not owned. It outlives any single request because that is
  // the whole point of a session, and on the device it lives in PSRAM -- 2.3 KB that
  // internal RAM cannot spare.
  WapClient(Transport& transport, CookieJar& jar, WapIo& io) : transport_(transport), jar_(jar), io_(io) {}

  // Unix seconds used for cookie expiry. 0 means "the clock is not set", which the
  // jar treats as "nothing has expired" so a cold boot before NTP is not a logout.
  void setNowUnix(const uint64_t t) { nowUnix_ = t; }
  uint64_t nowUnix() const { return nowUnix_; }
  CookieJar& jar() { return jar_; }
  const CookieJar& jar() const { return jar_; }

  // Returns `out.status`: a positive HTTP status, or a negative TransportError.
  // The body is streamed to `sink` as it arrives; on a followed redirect the
  // intermediate bodies are discarded, so the sink only ever sees the final one.
  int get(const char* url, const WapOptions& opt, BodySink sink, void* sinkCtx, WapResponse& out);
  int post(const char* url, const char* body, size_t bodyLen, const WapOptions& opt, BodySink sink, void* sinkCtx,
           WapResponse& out);

 private:
  int perform(const char* method, const char* url, const char* body, size_t bodyLen, const WapOptions& opt,
              BodySink sink, void* sinkCtx, WapResponse& out);

  Transport& transport_;
  CookieJar& jar_;
  WapIo& io_;
  uint64_t nowUnix_ = 0;
};

}  // namespace jjwxc
