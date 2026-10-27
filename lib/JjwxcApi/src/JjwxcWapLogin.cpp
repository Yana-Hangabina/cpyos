#include "JjwxcWapLogin.h"

#include <stdio.h>
#include <string.h>

namespace jjwxc {
namespace {

// GB18030 lead bytes are 0x81..0xFE and trail bytes 0x40..0xFE, so 0x22 (`"`) is
// neither. A pattern that starts and ends with a quote therefore cannot be assembled
// out of CJK trail bytes, which is what makes searching raw GBK safe here.
bool containsSpan(const char* page, const size_t len, const char* needle) {
  const size_t nl = strlen(needle);
  if (nl == 0 || nl > len) return false;
  for (size_t i = 0; i + nl <= len; ++i) {
    if (memcmp(page + i, needle, nl) == 0) return true;
  }
  return false;
}

size_t findSpan(const char* page, const size_t len, const char* needle, const size_t from) {
  const size_t nl = strlen(needle);
  if (nl == 0 || nl > len) return len;
  for (size_t i = from; i + nl <= len; ++i) {
    if (memcmp(page + i, needle, nl) == 0) return i;
  }
  return len;
}

// Reads value="..." out of the tag that contains `name="<name>"`.
//
// Bounded by the enclosing '<' and '>' rather than by a fixed window, because the
// captures put each attribute on its own line with dozens of spaces of indentation:
//   <input type="hidden"
//          name="checktype"
//          id="checktype"
//          value="email">
// A regex-style "look ahead a bit" would miss it, and the attribute order is not
// guaranteed either -- value= can precede name=.
bool extractInputValue(const char* page, const size_t len, const char* name, char* out, const size_t outCap) {
  out[0] = '\0';
  if (outCap == 0) return false;

  char marker[40];
  const int mn = snprintf(marker, sizeof(marker), "name=\"%s\"", name);
  if (mn <= 0 || static_cast<size_t>(mn) >= sizeof(marker)) return false;

  const size_t at = findSpan(page, len, marker, 0);
  if (at >= len) return false;

  size_t tagStart = at;
  while (tagStart > 0 && page[tagStart] != '<') --tagStart;
  if (page[tagStart] != '<') return false;
  size_t tagEnd = at;
  while (tagEnd < len && page[tagEnd] != '>') ++tagEnd;

  const size_t vat = findSpan(page, tagEnd, "value=\"", tagStart);
  if (vat >= tagEnd) return false;
  const size_t vstart = vat + 7;
  size_t vend = vstart;
  while (vend < tagEnd && page[vend] != '"') ++vend;

  const size_t vl = vend - vstart;
  if (vl + 1 > outCap) return false;  // refuse rather than truncate: a half value is a wrong value
  memcpy(out, page + vstart, vl);
  out[vl] = '\0';
  return true;
}

// Grows the form one field at a time. Returns false when the field would not fit,
// leaving `used` untouched, so the caller reports the failure instead of sending a
// body whose last field is cut off -- which the server reads as a missing field.
//
// `encode` is false only for `sub`, whose value is already percent-encoded UTF-8.
bool appendField(char* out, const size_t outCap, size_t& used, const char* name, const char* value,
                 const bool encode) {
  char encoded[512];
  const char* v = value;
  if (encode) {
    encoded[0] = '\0';
    if (value[0] != '\0' && wapUrlEncode(value, strlen(value), encoded, sizeof(encoded)) == 0) return false;
    v = encoded;
  }
  const size_t nl = strlen(name);
  const size_t vl = strlen(v);
  const size_t need = (used > 0 ? 1 : 0) + nl + 1 + vl;
  if (used + need + 1 > outCap) return false;
  if (used > 0) out[used++] = '&';
  memcpy(out + used, name, nl);
  used += nl;
  out[used++] = '=';
  memcpy(out + used, v, vl);
  used += vl;
  out[used] = '\0';
  return true;
}


struct PageCollector {
  char* buf = nullptr;
  size_t cap = 0;
  size_t len = 0;
  bool truncated = false;
};

bool collectPage(void* ctx, const uint8_t* data, const size_t len) {
  auto* c = static_cast<PageCollector*>(ctx);
  if (c->buf == nullptr || c->cap == 0) return true;  // no buffer: discard, but keep going
  const size_t room = c->cap - 1 - c->len;
  const size_t n = len < room ? len : room;
  if (n > 0) {
    memcpy(c->buf + c->len, data, n);
    c->len += n;
    c->buf[c->len] = '\0';
  }
  if (n < len) c->truncated = true;
  // Never abort on overflow: the sid arrives through Set-Cookie, and killing the
  // response would drop a login that actually succeeded.
  return true;
}

// Shared by both attempts. `warmUp` runs the anonymous GET that collects JJSESS.
WapLoginStatus runAttempt(WapClient& client, const char* loginName, const char* password, const char* checkType,
                          const char* deviceCode, const char* authKey, const WapLoginBuffers& bufs, const bool warmUp,
                          WapLoginResult& out) {
  out = WapLoginResult{};
  if (loginName == nullptr || loginName[0] == '\0' || password == nullptr) {
    out.status = WapLoginStatus::Refused;
    return out.status;
  }
  if (bufs.page == nullptr || bufs.pageCap == 0 || bufs.form == nullptr || bufs.formCap == 0) {
    out.status = WapLoginStatus::Protocol;
    return out.status;
  }

  CookieJar& jar = client.jar();
  const uint64_t now = client.nowUnix();

  WapOptions opt;
  if (warmUp) {
    // Not skippable: this is the only place JJSESS comes from, and the client proven
    // to log in successfully had it in the jar when it POSTed.
    WapResponse warm;
    opt.referer = kWapLoginPageReferer;
    const int wr = client.get(kWapLoginPageUrl, opt, nullptr, nullptr, warm);
    if (wr < 0) {
      out.status = WapLoginStatus::Network;
      out.httpStatus = wr;
      return out.status;
    }
  }

  const size_t formLen = (authKey != nullptr && authKey[0] != '\0')
                             ? wapBuildLoginFormWithAuth(loginName, password, authKey, bufs.form, bufs.formCap)
                             : wapBuildLoginForm(loginName, password, warmUp ? "" : checkType, deviceCode, bufs.form,
                                                 bufs.formCap);
  if (formLen == 0) {
    out.status = WapLoginStatus::Protocol;
    return out.status;
  }

  PageCollector col;
  col.buf = bufs.page;
  col.cap = bufs.pageCap;
  col.buf[0] = '\0';

  WapOptions post;
  post.maxRedirects = 3;  // a successful login answers 302 -> /my; the chain is short
  post.referer = kWapLoginPageUrl;
  post.contentType = kWapFormContentType;

  WapResponse res;
  const int r = client.post(kWapLoginFormUrl, bufs.form, formLen, post, &collectPage, &col, res);
  out.httpStatus = res.status;
  out.redirects = res.redirectsFollowed;
  out.pageTruncated = col.truncated;
  out.cookieHeaderTruncated = res.cookieHeaderTruncated;
  if (r < 0) {
    out.status = WapLoginStatus::Network;
    return out.status;
  }

  // Success is decided by the cookie, not by the page. The server delivers sid through
  // Set-Cookie; the body is only consulted to tell the two failure modes apart, so a
  // truncated or unexpected page cannot lose a login that worked.
  if (jar.has("sid", now)) {
    out.readerId = jar.readerIdFromSid(now);
    out.status = WapLoginStatus::Ok;
    return out.status;
  }

  WapLoginPage page;
  wapClassifyLoginPage(col.buf, col.len, page);
  if (page.hasDeviceCodeInput) {
    // Prefer the page's own checktype; fall back to "email", which is what every
    // capture carried and what the endpoint accepts.
    if (page.checkType[0] != '\0') {
      snprintf(out.checkType, sizeof(out.checkType), "%s", page.checkType);
    } else {
      snprintf(out.checkType, sizeof(out.checkType), "%s", "email");
    }
    out.status = WapLoginStatus::NeedDeviceCode;
    return out.status;
  }
  if (page.hasLoginForm) {
    // The form came straight back. There is no server-rendered reason to read: the
    // message div is filled by JavaScript, which this device does not run.
    out.status = WapLoginStatus::Refused;
    return out.status;
  }
  out.status = WapLoginStatus::Protocol;
  return out.status;
}

}  // namespace

const char* wapLoginStatusText(const WapLoginStatus s) {
  switch (s) {
    case WapLoginStatus::Ok:
      return "ok";
    case WapLoginStatus::NeedDeviceCode:
      return "need-device-code";
    case WapLoginStatus::Refused:
      return "refused";
    case WapLoginStatus::Network:
      return "network";
    case WapLoginStatus::Protocol:
      default:
      return "protocol";
  }
}

bool wapClassifyLoginPage(const char* page, const size_t len, WapLoginPage& out) {
  out = WapLoginPage{};
  if (page == nullptr || len == 0) return false;
  out.hasLoginForm = containsSpan(page, len, "action=\"/login/wapLogin\"");
  out.hasDeviceCodeInput = containsSpan(page, len, "name=\"checkdevicecode\"");
  extractInputValue(page, len, "checktype", out.checkType, sizeof(out.checkType));
  extractInputValue(page, len, "checkdevicecodetype", out.checkDeviceCodeType, sizeof(out.checkDeviceCodeType));
  return true;
}

size_t wapUrlEncode(const char* s, const size_t len, char* out, const size_t outCap) {
  if (out == nullptr || outCap == 0) return 0;
  out[0] = '\0';
  if (s == nullptr) return 0;
  size_t used = 0;
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '.' || c == '_' || c == '~';
    const size_t need = unreserved ? 1 : (c == ' ' ? 1 : 3);
    if (used + need + 1 > outCap) return 0;
    if (unreserved) {
      out[used++] = static_cast<char>(c);
    } else if (c == ' ') {
      out[used++] = '+';
    } else {
      static const char kHex[] = "0123456789ABCDEF";
      out[used++] = '%';
      out[used++] = kHex[c >> 4];
      out[used++] = kHex[c & 0xF];
    }
  }
  out[used] = '\0';
  return used;
}

size_t wapBuildLoginFormWithAuth(const char* loginName, const char* password, const char* authKey, char* out,
                                 const size_t outCap) {
  if (loginName == nullptr || loginName[0] == '\0' || password == nullptr || out == nullptr || outCap == 0) return 0;
  out[0] = '\0';
  size_t used = 0;
  bool ok = true;
  ok = ok && appendField(out, outCap, used, "referer", "/my", true);
  ok = ok && appendField(out, outCap, used, "login_mode", "jjwxc", true);
  ok = ok && appendField(out, outCap, used, "loginname", loginName, true);
  ok = ok && appendField(out, outCap, used, "loginpass", password, true);
  ok = ok && appendField(out, outCap, used, "select_device_name", "", true);
  ok = ok && appendField(out, outCap, used, "checktype", "", true);
  ok = ok && appendField(out, outCap, used, "checkdevicecodetype", "", true);
  ok = ok && appendField(out, outCap, used, "shumei_transcation", "", true);
  ok = ok && appendField(out, outCap, used, "shumei_captcha_rid", "", true);
  ok = ok && appendField(out, outCap, used, "authKey", authKey != nullptr ? authKey : "", true);
  ok = ok && appendField(out, outCap, used, "registerRule", "on", true);
  ok = ok && appendField(out, outCap, used, "cookietime", "1", true);
  ok = ok && appendField(out, outCap, used, "shumeideviceId", kWapShumeiFallback, true);
  ok = ok && appendField(out, outCap, used, "sub", "%E7%99%BB%E3%80%80%E5%85%A5", false);
  if (!ok) return 0;
  return used;
}

size_t wapBuildLoginForm(const char* loginName, const char* password, const char* checkType, const char* deviceCode,
                         char* out, const size_t outCap) {
  // An empty account is refused here as well as in runAttempt: building the body is the
  // last point at which it can be caught, and POSTing it would spend a login attempt on
  // a request that cannot succeed.
  if (out == nullptr || outCap == 0 || loginName == nullptr || loginName[0] == '\0' || password == nullptr) return 0;
  out[0] = '\0';
  size_t used = 0;

  const char* ct = (checkType != nullptr) ? checkType : "";
  const char* code = (deviceCode != nullptr) ? deviceCode : "";
  // The retry form carries the code and names its channel; the first attempt leaves
  // both empty, exactly as the captured initial form does.
  const bool withCode = code[0] != '\0';

  bool ok = true;
  ok = ok && appendField(out, outCap, used, "referer", "/my", true);
  ok = ok && appendField(out, outCap, used, "login_mode", "jjwxc", true);
  ok = ok && appendField(out, outCap, used, "loginname", loginName, true);
  ok = ok && appendField(out, outCap, used, "loginpass", password, true);
  ok = ok && appendField(out, outCap, used, "select_device_name", "", true);
  ok = ok && appendField(out, outCap, used, "checktype", withCode ? ct : "", true);
  if (withCode) {
    ok = ok && appendField(out, outCap, used, "checkdevicecode", code, true);
  }
  ok = ok && appendField(out, outCap, used, "checkdevicecodetype", withCode ? "all" : "", true);
  ok = ok && appendField(out, outCap, used, "shumei_transcation", "", true);
  ok = ok && appendField(out, outCap, used, "shumei_captcha_rid", "", true);
  ok = ok && appendField(out, outCap, used, "authKey", "", true);
  ok = ok && appendField(out, outCap, used, "registerRule", "on", true);
  ok = ok && appendField(out, outCap, used, "cookietime", "1", true);
  ok = ok && appendField(out, outCap, used, "shumeideviceId", kWapShumeiFallback, true);
  // U+767B U+3000 U+5165, pre-encoded: written as escapes so this file stays pure
  // ASCII and nobody has to trust that their editor preserved a three-byte
  // ideographic space. This is the three-codepoint form the proven client sent; the
  // captured form holds four (an extra ASCII space) and the proven bytes win.
  ok = ok && appendField(out, outCap, used, "sub", "%E7%99%BB%E3%80%80%E5%85%A5", false);
  if (!ok) return 0;
  return used;
}

WapLoginStatus wapLogin(WapClient& client, const char* loginName, const char* password, const WapLoginBuffers& bufs,
                        WapLoginResult& out) {
  // A sid left over from a previous session would otherwise make a failed login look
  // successful, because success is decided by the jar.
  client.jar().clear();
  return runAttempt(client, loginName, password, nullptr, nullptr, nullptr, bufs, /*warmUp=*/true, out);
}

WapLoginStatus wapLoginWithCode(WapClient& client, const char* loginName, const char* password, const char* checkType,
                                const char* deviceCode, const WapLoginBuffers& bufs, WapLoginResult& out) {
  // Deliberately does not clear the jar: JJSESS and friends from the first attempt are
  // what make this one a continuation rather than a fresh anonymous login.
  return runAttempt(client, loginName, password, checkType, deviceCode, nullptr, bufs, /*warmUp=*/false, out);
}

WapLoginStatus wapLoginWithAuthKey(WapClient& client, const char* loginName, const char* password, const char* authKey,
                                   const WapLoginBuffers& bufs, WapLoginResult& out) {
  if (authKey == nullptr || authKey[0] == '\0') {
    out = WapLoginResult{};
    out.status = WapLoginStatus::Protocol;
    return out.status;
  }
  return runAttempt(client, loginName, password, nullptr, nullptr, authKey, bufs, /*warmUp=*/false, out);
}

WapRedirectMeaning wapClassifyChapterRedirect(const char* location) {
  if (location == nullptr || location[0] == '\0') return WapRedirectMeaning::Unknown;

  // Match on the PATH, and only at its start. Searching the whole URL would let a
  // query string decide the outcome -- `/redirect?next=/buy/1/2` is not a purchase
  // redirect, and misreading it would stub a chapter the account can actually read.
  const char* path = location;
  const char* scheme = strstr(location, "://");
  if (scheme != nullptr) {
    path = strchr(scheme + 3, '/');
    if (path == nullptr) return WapRedirectMeaning::Unknown;  // host only, no path
  }
  size_t len = 0;
  while (path[len] != '\0' && path[len] != '?' && path[len] != '#') ++len;

  // A prefix match has to stop at a path-segment boundary, or `/my/loginhelp` reads as
  // a login redirect. Same rule CookieJar::pathMatches uses.
  auto startsSegment = [&](const char* want) {
    const size_t wl = strlen(want);
    if (len < wl || memcmp(path, want, wl) != 0) return false;
    if (want[wl - 1] == '/') return true;  // the pattern already ends on a boundary
    return len == wl || path[wl] == '/' || path[wl] == '?' || path[wl] == '#';
  };
  if (startsSegment("/buy/")) return WapRedirectMeaning::Purchase;
  if (startsSegment("/my/login")) return WapRedirectMeaning::SessionDead;
  return WapRedirectMeaning::Unknown;
}

int wapRequestDeviceCode(WapClient& client, const char* loginName, const char* checkType, const BodySink sink,
                         void* sinkCtx) {
  if (loginName == nullptr || loginName[0] == '\0') return kTransportNetwork;
  char user[256];
  char type[32];
  if (wapUrlEncode(loginName, strlen(loginName), user, sizeof(user)) == 0) return kTransportTooLarge;
  const char* ct = (checkType != nullptr && checkType[0] != '\0') ? checkType : "email";
  if (wapUrlEncode(ct, strlen(ct), type, sizeof(type)) == 0) return kTransportTooLarge;

  char url[384];
  const int n = snprintf(url, sizeof(url), "%s?checktype=%s&username=%s", kWapDeviceCodeUrl, type, user);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return kTransportTooLarge;

  WapOptions opt;
  opt.referer = kWapLoginPageUrl;
  WapResponse res;
  return client.get(url, opt, sink, sinkCtx, res);
}

}  // namespace jjwxc
