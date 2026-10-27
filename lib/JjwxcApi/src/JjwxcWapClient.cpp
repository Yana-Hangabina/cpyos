#include "JjwxcWapClient.h"

#include <stdio.h>
#include <string.h>

namespace jjwxc {
namespace {

char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool nameIs(const char* name, const size_t nameLen, const char* want) {
  const size_t wl = strlen(want);
  if (nameLen != wl) return false;
  for (size_t i = 0; i < wl; ++i) {
    if (asciiLower(name[i]) != want[i]) return false;
  }
  return true;
}

// Substring search over a bounded, possibly non-terminated span, case-insensitively.
// Used for `charset=gbk` inside Content-Type, which servers spell inconsistently
// (GBK, gb2312, gb18030) and which decides whether the caller must transcode.
bool spanContains(const char* hay, size_t hayLen, const char* needle) {
  const size_t nl = strlen(needle);
  if (nl == 0 || nl > hayLen) return false;
  for (size_t i = 0; i + nl <= hayLen; ++i) {
    size_t j = 0;
    while (j < nl && asciiLower(hay[i + j]) == needle[j]) ++j;
    if (j == nl) return true;
  }
  return false;
}

// Everything the response headers have to contribute, gathered in one place because
// HeaderSink is a plain function pointer and cannot close over anything.
struct HeaderCapture {
  CookieJar* jar = nullptr;
  const char* requestHost = nullptr;
  const char* requestUrl = nullptr;
  uint64_t nowUnix = 0;
  char* location = nullptr;
  size_t locationCap = 0;
  uint8_t stored = 0;
  bool isGbk = false;
  // Set as soon as a Location header arrives and the caller has redirect budget left.
  // The HTTP client dispatches every header before it streams a byte of the body, so
  // this is decided in time to keep the intermediate page away from the caller's sink
  // -- a 302's HTML is not what anyone asked for, and forwarding it would splice a
  // login page into the middle of a chapter.
  //
  // The cost of keying on Location rather than on the status is that a 200 carrying a
  // Location would also have its body dropped. WAP does not send one, and the status
  // is not reachable from a header callback, so this is the honest trade rather than a
  // hidden one.
  bool armDiscard = false;
  bool discardBody = false;
  HeaderSink forward = nullptr;
  void* forwardCtx = nullptr;
};

void captureHeader(void* ctx, const char* name, const size_t nameLen, const char* value, const size_t valueLen) {
  auto* cap = static_cast<HeaderCapture*>(ctx);
  if (name == nullptr || value == nullptr) return;

  if (nameIs(name, nameLen, "set-cookie")) {
    if (cap->jar != nullptr && cap->requestHost != nullptr) {
      const auto r = cap->jar->setFromHeader(value, valueLen, cap->requestHost, cap->nowUnix);
      if (r == CookieJar::SetResult::Stored) ++cap->stored;
    }
  } else if (nameIs(name, nameLen, "location")) {
    if (cap->location != nullptr && cap->locationCap > 0 && cap->requestUrl != nullptr) {
      // Only discard the body once the Location is known to be usable. If it is not,
      // the loop cannot follow and the 3xx body is the best answer available.
      if (resolveRedirect(cap->requestUrl, value, cap->location, cap->locationCap) && cap->armDiscard) {
        cap->discardBody = true;
      }
    }
  } else if (nameIs(name, nameLen, "content-type")) {
    cap->isGbk = spanContains(value, valueLen, "gbk") || spanContains(value, valueLen, "gb2312") ||
                 spanContains(value, valueLen, "gb18030");
  }

  if (cap->forward != nullptr) cap->forward(cap->forwardCtx, name, nameLen, value, valueLen);
}

// Bridges BodySink to the discard decision above.
struct BodyBridge {
  BodySink sink = nullptr;
  void* ctx = nullptr;
  const HeaderCapture* cap = nullptr;
};

bool bridgeBody(void* ctx, const uint8_t* data, const size_t len) {
  auto* b = static_cast<BodyBridge*>(ctx);
  if (b->cap != nullptr && b->cap->discardBody) return true;
  if (b->sink == nullptr) return true;
  return b->sink(b->ctx, data, len);
}

bool isRedirectStatus(const int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

}  // namespace

int WapClient::get(const char* url, const WapOptions& opt, const BodySink sink, void* sinkCtx, WapResponse& out) {
  return perform("GET", url, nullptr, 0, opt, sink, sinkCtx, out);
}

int WapClient::post(const char* url, const char* body, const size_t bodyLen, const WapOptions& opt, const BodySink sink,
                    void* sinkCtx, WapResponse& out) {
  return perform("POST", url, body, bodyLen, opt, sink, sinkCtx, out);
}

int WapClient::perform(const char* method, const char* url, const char* body, const size_t bodyLen,
                       const WapOptions& opt, const BodySink sink, void* sinkCtx, WapResponse& out) {
  out = WapResponse{};
  if (url == nullptr || url[0] == '\0' || method == nullptr) {
    out.status = kTransportNetwork;
    return out.status;
  }
  if (strlen(url) + 1 > sizeof(io_.url)) {
    out.status = kTransportTooLarge;
    return out.status;
  }
  snprintf(io_.url, sizeof(io_.url), "%s", url);

  const char* curMethod = method;
  const char* curBody = body;
  size_t curBodyLen = bodyLen;

  for (uint8_t hop = 0;; ++hop) {
    // `location` describes the response that produced `status`, so it is cleared per
    // hop. Leaving the previous hop's value in place would report a redirect that the
    // final response did not have, and a caller that keys "session expired" off it
    // would send the user to log in for no reason.
    out.location[0] = '\0';

    char host[Cookie::kDomainCap] = {};
    // Paths here are short (/vip/<novelId>/21, /my/login?login_mode=jjwxc); a longer
    // one is truncated, which only weakens path matching and never sends a cookie to
    // the wrong host -- every cookie this app holds is Path=/.
    char path[192] = {};
    if (!splitRequestUrl(io_.url, host, sizeof(host), path, sizeof(path))) {
      out.status = kTransportNetwork;
      return out.status;
    }

    bool truncated = false;
    io_.cookie[0] = '\0';
    const size_t cookieLen =
        jar_.buildHeader(host, path, /*https=*/true, io_.cookie, sizeof(io_.cookie), nowUnix_, &truncated);
    if (truncated) out.cookieHeaderTruncated = true;

    RequestHeader headers[7];
    size_t hn = 0;
    headers[hn++] = {"User-Agent", kWapUserAgent};
    headers[hn++] = {"Accept", kWapAccept};
    headers[hn++] = {"Accept-Language", kWapAcceptLanguage};
    if (cookieLen > 0) headers[hn++] = {"Cookie", io_.cookie};
    if (opt.referer != nullptr && opt.referer[0] != '\0') headers[hn++] = {"Referer", opt.referer};
    if (curBody != nullptr && opt.contentType != nullptr) headers[hn++] = {"Content-Type", opt.contentType};

    HeaderCapture cap;
    cap.jar = &jar_;
    cap.requestHost = host;
    cap.requestUrl = io_.url;
    cap.nowUnix = nowUnix_;
    cap.location = out.location;
    cap.locationCap = sizeof(out.location);
    cap.forward = opt.onHeader;
    cap.forwardCtx = opt.headerCtx;
    // Only arm the discard when there is budget to actually follow: with none left,
    // the 3xx body IS the answer the caller asked for.
    cap.armDiscard = hop < opt.maxRedirects;

    BodyBridge bridge;
    bridge.sink = sink;
    bridge.ctx = sinkCtx;
    bridge.cap = &cap;

    const int r = transport_.request(curMethod, io_.url, headers, hn, curBody, curBodyLen, &bridgeBody, &bridge,
                                     &captureHeader, &cap);
    if (r < 0) {
      out.status = r;
      snprintf(out.finalUrl, sizeof(out.finalUrl), "%s", io_.url);
      return r;
    }

    out.status = r;
    snprintf(out.finalUrl, sizeof(out.finalUrl), "%s", io_.url);
    out.cookiesStored = static_cast<uint8_t>(out.cookiesStored + cap.stored);
    if (cap.isGbk) out.isGbk = true;

    if (!isRedirectStatus(r)) return r;
    out.sawRedirect = true;
    if (out.location[0] == '\0') return r;  // a 3xx with nowhere to go is the answer
    const bool canFollow = hop < opt.maxRedirects;
    if (!canFollow) return r;

    // 301/302/303 downgrade to a bodiless GET, which is what every browser does and
    // what WAP login depends on: the POST is answered with a 302 to /my, and
    // re-POSTing the credentials there would log in twice. 307/308 repeat verbatim.
    if (r != 307 && r != 308) {
      curMethod = "GET";
      curBody = nullptr;
      curBodyLen = 0;
    }

    char next[sizeof(io_.url)];
    if (!resolveRedirect(io_.url, out.location, next, sizeof(next))) return r;
    // A Location pointing back at the same URL would spin for maxRedirects hops and
    // then report a 3xx that looks like a server bug rather than a loop.
    if (strcmp(next, io_.url) == 0) return r;
    snprintf(io_.url, sizeof(io_.url), "%s", next);
    ++out.redirectsFollowed;
  }
}

}  // namespace jjwxc
