// JJWXC WAP login: POST /login/wapLogin and the page-shape classifier.
//
// WAP has no business status codes. Nothing in the response says "wrong password" or
// "supply a code" the way the App API's JSON does; the answer is which of three HTML
// pages came back. So the classifier below IS the protocol, and getting it wrong does
// not produce an error -- it produces a device that asks for a verification code
// forever, or one that reports success without a session.
//
// Three measured facts drive the design (all from the captures in cpyos/exp/, plus two
// anonymous probes of m.jjwxc.net; see cpyos/AGENTS.md sections 8 and 9):
//
//   - The discriminator is `<input name="checkdevicecode">`, NOT the substring
//     "checkdevicecode". The initial login form contains that substring 7 times, every
//     one of them inside jQuery (`$('input[name=checkdevicecode]')`) or a CSS class
//     (`<div class="checkdevicecode">`). A strstr-based check classifies EVERY login
//     page as needing a code. Quoting the attribute -- `name="checkdevicecode"` --
//     appears exactly once in the needs-code page and zero times in the initial form.
//
//   - Classification runs on the RAW GB18030 bytes, with no transcoding. GBK lead
//     bytes are 0x81..0xFE and trail bytes are 0x40..0xFE, so 0x22 (`"`) can be
//     neither: a quoted ASCII pattern is unambiguous in GBK. Every marker here is
//     quoted. Transcoding first would need a 48 KB buffer and add a way to fail.
//
//   - A refused login renders NO server-side error text. The page carries an empty
//     `<div id="errormessage"></div>` that jQuery fills in; an e-reader runs no
//     JavaScript. So "wrong password" and "the form was re-served for some other
//     reason" are genuinely indistinguishable, and callers must not claim a specific
//     reason they cannot see.
//
// The primary success signal is not the page at all -- it is whether the jar gained a
// `sid`. The body only decides between "needs a code" and "refused". That ordering
// means a truncated or unparseable page cannot turn a successful login into a failure.
//
// No heap, no STL, no Arduino/ESP/wolfSSL headers, per the rest of this library.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "JjwxcWapClient.h"

namespace jjwxc {

// The hosts the login flow touches. The verification-code endpoint is the SAME
// backend the App API uses, reached in a different shape (GET with a query string
// rather than POST), which is why it is not on the WAP host.
constexpr const char* kWapHost = "https://m.jjwxc.net";
constexpr const char* kWapLoginFormUrl = "https://m.jjwxc.net/login/wapLogin";
constexpr const char* kWapLoginPageUrl = "https://m.jjwxc.net/my/login?login_mode=jjwxc";
constexpr const char* kWapLoginPageReferer = "https://m.jjwxc.net/my/login";
constexpr const char* kWapDeviceCodeUrl = "https://app.jjwxc.net/appDevicesecurityAndroid/getDeviceSecurityCode";

// Shumei's own fallback value, used when its SDK has retried ten times and given up.
// Sending a literal here is what the one client proven to log in successfully sent;
// the device cannot run the SDK, so there is no real id to offer.
constexpr const char* kWapShumeiFallback = "get_deviceid_timeout";

enum class WapLoginStatus : uint8_t {
  Ok = 0,          // the jar holds a live sid
  NeedDeviceCode,  // the page asks for the emailed verification code
  Refused,         // the login form came back: rejected, for a reason the page does not state
  Network,         // the request never completed
  Protocol,        // an unrecognised page shape and no sid; nothing safe to conclude
};

const char* wapLoginStatusText(WapLoginStatus s);

// What the returned page said. Filled by wapClassifyLoginPage.
struct WapLoginPage {
  // `<form action="/login/wapLogin"` is present. Absent on the post-login page.
  bool hasLoginForm = false;
  // `<input ... name="checkdevicecode"` is present. THIS is the needs-code signal.
  bool hasDeviceCodeInput = false;
  // value="..." of the `checktype` input ("email" in every capture), empty if absent.
  char checkType[16] = {};
  // value="..." of the `checkdevicecodetype` input ("all" in every capture).
  char checkDeviceCodeType[16] = {};
};

// Scans a login-response page. Works on raw GB18030 bytes; see the file header.
// Returns false only when `page` is null or empty -- an unrecognised page is a valid
// result (every flag false), because "we do not know what this is" has to be
// distinguishable from "we did not look".
bool wapClassifyLoginPage(const char* page, size_t len, WapLoginPage& out);

// Percent-encodes for an application/x-www-form-urlencoded body: unreserved
// characters pass through, a space becomes '+', everything else becomes %XX.
//
// Not optional. A password containing '&', '=', '+' or '%' splits the form in two when
// concatenated raw, and the server then sees half a password and answers "wrong
// password" -- a failure whose real cause appears nowhere.
// Returns the length written, or 0 when it did not fit (never a partial encoding).
size_t wapUrlEncode(const char* s, size_t len, char* out, size_t outCap);

// Builds the login POST body. Fourteen fields when `deviceCode` is empty, fifteen when
// it is not -- the extra one being `checkdevicecode`, which only the needs-code page
// carries and only a retry should send.
//
// The `sub` field is the three-codepoint form (U+767B U+3000 U+5165) that the client
// proven to log in successfully sent. The captured form holds four codepoints, with an
// extra ASCII space; the proven bytes win over the observed ones, because "what the
// page contains" and "what the server accepts" are different claims.
//
// Returns the body length, or 0 when it did not fit `outCap`.
size_t wapBuildLoginForm(const char* loginName, const char* password, const char* checkType, const char* deviceCode,
                         char* out, size_t outCap);

// Same form with the main-device approval key in the `authKey` field. The approval
// flow submits the ordinary form with an empty checktype/code: the page itself only
// fills `authKey` and submits.
size_t wapBuildLoginFormWithAuth(const char* loginName, const char* password, const char* authKey, char* out,
                                 size_t outCap);

struct WapLoginResult {
  WapLoginStatus status = WapLoginStatus::Network;
  // From the needs-code page; "email" in every capture. Empty unless NeedDeviceCode.
  char checkType[16] = {};
  // Recovered from the sid on success, else 0.
  uint32_t readerId = 0;
  // Diagnostics for the log. WAP has no business code, so these are the only thing to
  // report when the outcome is unexpected.
  int httpStatus = 0;
  uint8_t redirects = 0;
  // The response did not fit the caller's page buffer. Success is still reported if
  // the jar gained a sid, because that signal does not come from the body.
  bool pageTruncated = false;
  bool cookieHeaderTruncated = false;
};

// Caller-owned scratch, following the same convention as WapBuffers in JjwxcWap.h.
// Both are live at once -- the form body has to stay valid while the response is being
// collected into the page buffer -- so they cannot share one region.
struct WapLoginBuffers {
  // Receives the final response body. The largest login page measured is 33,701 bytes;
  // 64 KB leaves room for the site to grow and for a caller that transcodes in place.
  // Nothing in it survives the call.
  char* page = nullptr;
  size_t pageCap = 0;
  // Receives the urlencoded POST body. 1024 covers a long account name and password
  // tripled by percent-encoding plus the twelve fixed fields.
  char* form = nullptr;
  size_t formCap = 0;
};

// Runs the whole first attempt: clear the jar, warm-up GET (which collects JJSESS and
// must not be skipped), POST the credentials, follow the 302, then classify.
//
// The jar is CLEARED first. Without that, a sid persisted from an earlier session
// would make a failed login look like a successful one.
WapLoginStatus wapLogin(WapClient& client, const char* loginName, const char* password, const WapLoginBuffers& bufs,
                        WapLoginResult& out);

// Retries with the emailed code. Does NOT clear the jar and does NOT repeat the
// warm-up: the cookies the first attempt collected are what make this one valid.
WapLoginStatus wapLoginWithCode(WapClient& client, const char* loginName, const char* password, const char* checkType,
                                const char* deviceCode, const WapLoginBuffers& bufs, WapLoginResult& out);

// Submits the form with a main-device approval key. Like the code retry it does
// not clear the jar or repeat the warm-up, so the cookies from the attempt that
// asked for verification are kept.
WapLoginStatus wapLoginWithAuthKey(WapClient& client, const char* loginName, const char* password, const char* authKey,
                                   const WapLoginBuffers& bufs, WapLoginResult& out);

// What a redirect away from a chapter URL means.
//
// Measured on the device, not inferred: a VIP chapter this account has not bought
// answers `302 -> https://m.jjwxc.net/buy/<novelId>/<chapterId>`. Two consequences
// worth stating, because both were gotten wrong the first time.
//
//   - It is NOT a dead session. Reading every redirect as "sign in again" aborted a
//     whole-book download at the first unpurchased chapter and told the user to log in
//     -- seconds after they had logged in successfully.
//   - WAP does not auto-purchase. It sends you to the purchase page and stops. The App
//     API behaved differently: merely requesting an unbought chapter triggered the
//     account's own server-side auto-subscription (measured code 1013, "余额不足"). So
//     the same request is free on this path and could spend money on the other.
enum class WapRedirectMeaning : uint8_t {
  Purchase,     // the chapter is not bought; per-chapter, so stub it and carry on
  SessionDead,  // the service wants a login; every remaining chapter would fail alike
  Unknown,      // a shape nobody has observed -- report it, do not guess
};

// Classifies a resolved Location from a chapter request. `location` is the absolute URL
// WapResponse carries, so a rooted "/buy/..." has already been joined onto its host.
WapRedirectMeaning wapClassifyChapterRedirect(const char* location);

// Asks the server to send the verification code. Same backend the App API uses but a
// different shape: GET with `checktype` and `username` in the query string rather than
// a POST body.//
// Returns the HTTP status (200 on success), or a negative TransportError.
//
// The response body is streamed to `sink` unparsed. This endpoint answers with the App
// API's JSON envelope -- success is code 200, not 0, and the human-readable text is in
// data.message -- and parseDeviceCode() in JjwxcProtocol.h already reads it. Parsing it
// here instead would make this file depend on the JSON SAX library for one call, and
// would put a second copy of a status-mapping rule that has already misled three
// debugging sessions. An empty checkType falls back to "email", which is what every
// capture carried.
int wapRequestDeviceCode(WapClient& client, const char* loginName, const char* checkType, BodySink sink,
                         void* sinkCtx);

}  // namespace jjwxc
