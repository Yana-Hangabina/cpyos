// A cookie jar for JJWXC's WAP endpoints.
//
// The firmware's HTTP client has no cookie handling at all: WeRead's API is
// token-in-query, so nothing upstream ever needed to parse Set-Cookie or emit a
// Cookie header. WAP authenticates with a `sid` cookie, so both halves have to
// exist, and they live here rather than in the transport because they are pure
// string work that a host test can pin without a radio.
//
// Measured, and the reason the capacities below are what they are: a request
// carrying only `sid` -- no JJSESS, no nickname, no wap_reader_openid -- is enough
// for /my/fav to return the full shelf. Reading therefore needs one cookie. Logging
// in is a different question and is not answerable without spending a real attempt,
// so the jar carries what the proven-working client carried and drops only what
// genuinely does not fit. Dropping is reported to the caller; silently truncating a
// cookie would send a corrupt one instead, and the server answers that with a
// redirect to the login page -- indistinguishable from an expired session.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

struct Cookie {
  static constexpr size_t kNameCap = 32;
  // Sized by the largest cookie JJWXC actually sets, not by the one that matters.
  // `sid` is 133 and is the only cookie needed to READ, which is what a first
  // measurement of /my/fav suggests -- but the login warm-up sets `JJSESS`, measured
  // at 387 characters (and recorded as 376 on an earlier day, so it varies), and the
  // one script proven to log in successfully sent it back. Dropping it would be an
  // unforced experiment against a risk-controlled account, and it costs only PSRAM.
  static constexpr size_t kValueCap = 512;
  static constexpr size_t kDomainCap = 48;
  static constexpr size_t kPathCap = 24;

  char name[kNameCap] = {};
  char value[kValueCap] = {};
  char domain[kDomainCap] = {};  // no leading dot; empty means host-only
  char path[kPathCap] = "/";
  bool secure = false;
  bool hostOnly = true;
  // Unix seconds, or 0 for a session cookie. Expiry is honoured on read rather
  // than by a timer: the device clock can jump when it syncs, and a cookie that
  // was valid when stored must not become invalid because the clock moved back.
  uint64_t expiresAt = 0;

  size_t valueLen() const;
};

class CookieJar {
 public:
  // sid plus the handful of companions JJWXC sets. Eight is generous; the login
  // response sets seven.
  static constexpr size_t kMaxCookies = 8;

  enum class SetResult : uint8_t {
    Stored = 0,
    Removed,     // an expired or max-age=0 entry, which is a deletion request
    Rejected,    // malformed, or the value did not fit
    Unchanged,   // already stored with the same value
  };

  // Parses one Set-Cookie header value (the part after "Set-Cookie: ").
  // `requestHost` supplies the default domain when the header carries none, which
  // is what makes such a cookie host-only per RFC 6265.
  SetResult setFromHeader(const char* value, size_t len, const char* requestHost, uint64_t nowUnix);

  // Writes the Cookie header for a request. Returns the length written, or 0 when
  // no cookie applies. `https` gates the Secure ones.
  //
  // `truncated` is set when a cookie that should have been sent did not fit
  // `outCap`. Callers that authenticate with cookies must treat that as a hard
  // error rather than sending a partial set: the server answers a missing `sid`
  // with a redirect to the login page, which reads as "session expired" and sends
  // the user through a login they did not need.
  size_t buildHeader(const char* host, const char* path, bool https, char* out, size_t outCap, uint64_t nowUnix,
                     bool* truncated = nullptr) const;

  bool has(const char* name, uint64_t nowUnix) const;
  // Length of the value, or 0 when absent or it did not fit `outCap`.
  size_t get(const char* name, char* out, size_t outCap, uint64_t nowUnix) const;
  // The reader id is the eight digits ahead of the underscore in sid. Recovering it
  // here saves a caller from knowing the format; 0 when there is no usable sid.
  uint32_t readerIdFromSid(uint64_t nowUnix) const;

  void clear();
  size_t count(uint64_t nowUnix) const;

  // Persistence, so a reboot does not mean logging in again -- which matters here
  // because a login attempt is not free: the account is under escalated risk
  // control and each attempt is counted. The format is one cookie per line,
  // "name<TAB>value<TAB>domain<TAB>path<TAB>secure<TAB>expires", and the caller
  // stores it obfuscated like every other credential in this app.
  size_t serialize(char* out, size_t outCap) const;
  bool deserialize(const char* text, size_t len);

 private:
  int findSlot(const char* name, const char* domain, const char* path) const;
  bool liveAt(size_t index, uint64_t nowUnix) const;
  bool domainMatches(size_t index, const char* host) const;
  bool pathMatches(size_t index, const char* path) const;

  Cookie cookies_[kMaxCookies];
  size_t count_ = 0;  // high-water mark; slots can be empty in the middle
};

// Joins a possibly-relative Location onto the request URL. WAP login answers with
// both forms ("http://m.jjwxc.net/my" and "/my"), and getting this wrong sends the
// follow-up request to the wrong host, which then fails as a network error with no
// clue that a redirect was involved.
//
// An absolute http:// Location is upgraded to https:// when the base URL was https.
// JJWXC really does answer a cookieless /my/fav with `location:
// http://m.jjwxc.net/my/login?ctime=...`, and the firmware's client cannot speak
// plaintext HTTP at all, so passing that through would fail in zero milliseconds with
// nothing in the log to say a redirect was involved.
//
// Returns false when the Location is unusable.
bool resolveRedirect(const char* baseUrl, const char* location, char* out, size_t outCap);

// Pulls the host (lower-cased, port stripped) and the path (query removed, never
// empty) out of an absolute URL. These are the two pieces cookie matching needs,
// and there is no URL class in this library to get them from.
// Returns false when `url` has no usable host.
bool splitRequestUrl(const char* url, char* host, size_t hostCap, char* path, size_t pathCap);

}  // namespace jjwxc
