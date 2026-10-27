#include "JjwxcCookies.h"

#include <stdio.h>
#include <string.h>

namespace jjwxc {
namespace {

char lower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iEqual(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool iEqualsCstr(const char* a, const char* b) {
  const size_t n = strlen(b);
  return strlen(a) == n && iEqual(a, b, n);
}

void copyBounded(char* dst, size_t dstCap, const char* src, size_t srcLen) {
  if (dstCap == 0) return;
  const size_t n = srcLen < dstCap - 1 ? srcLen : dstCap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// Leading and trailing HTTP whitespace, and any wrapping quotes some servers add.
size_t trim(const char*& s, size_t& len) {
  while (len > 0 && (*s == ' ' || *s == '\t')) {
    ++s;
    --len;
  }
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) --len;
  if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
    ++s;
    len -= 2;
  }
  return len;
}

size_t findChar(const char* s, size_t len, size_t from, const char c) {
  for (size_t i = from; i < len; ++i) {
    if (s[i] == c) return i;
  }
  return len;
}

uint64_t parseUint64(const char* s, size_t len) {
  uint64_t v = 0;
  for (size_t i = 0; i < len; ++i) {
    if (s[i] < '0' || s[i] > '9') break;
    // Saturate rather than wrap. A wrapped max-age lands near zero and would make
    // the cookie expire immediately, which reads as a mystery logout.
    if (v > (UINT64_MAX - 9) / 10) return UINT64_MAX;
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
  }
  return v;
}

// Splits "https://host:port/path?query" into the pieces cookie matching needs.
// A hand-rolled split rather than a URL class: this library has no allocator and
// only ever needs the host and the path.
bool splitUrl(const char* url, char* scheme, size_t schemeCap, char* host, size_t hostCap, char* path,
              size_t pathCap) {
  scheme[0] = host[0] = path[0] = '\0';
  const char* sep = strstr(url, "://");
  const char* hostStart = url;
  if (sep != nullptr) {
    copyBounded(scheme, schemeCap, url, static_cast<size_t>(sep - url));
    hostStart = sep + 3;
  }
  const char* slash = strchr(hostStart, '/');
  const size_t hostLen = slash != nullptr ? static_cast<size_t>(slash - hostStart) : strlen(hostStart);
  copyBounded(host, hostCap, hostStart, hostLen);
  // Strip the port; cookies are not scoped by it.
  char* colon = strchr(host, ':');
  if (colon != nullptr) *colon = '\0';
  if (host[0] == '\0') return false;
  if (slash != nullptr) {
    const char* q = strchr(slash, '?');
    copyBounded(path, pathCap, slash, q != nullptr ? static_cast<size_t>(q - slash) : strlen(slash));
  } else {
    copyBounded(path, pathCap, "/", 1);
  }
  if (path[0] == '\0') copyBounded(path, pathCap, "/", 1);
  return true;
}

}  // namespace

size_t Cookie::valueLen() const { return strlen(value); }

int CookieJar::findSlot(const char* name, const char* domain, const char* path) const {
  for (size_t i = 0; i < count_; ++i) {
    if (cookies_[i].name[0] == '\0') continue;
    if (strcmp(cookies_[i].name, name) == 0 && strcmp(cookies_[i].domain, domain) == 0 &&
        strcmp(cookies_[i].path, path) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool CookieJar::liveAt(const size_t index, const uint64_t nowUnix) const {
  const Cookie& c = cookies_[index];
  if (c.name[0] == '\0') return false;
  // nowUnix == 0 means "the clock is not set yet", which happens on a device that
  // has not synced. Treating every cookie as expired then would log the user out on
  // every cold boot, so an unknown clock keeps session and dated cookies alike.
  if (c.expiresAt != 0 && nowUnix != 0 && nowUnix >= c.expiresAt) return false;
  return true;
}

bool CookieJar::domainMatches(const size_t index, const char* host) const {
  const Cookie& c = cookies_[index];
  if (c.hostOnly) return iEqualsCstr(host, c.domain);
  if (iEqualsCstr(host, c.domain)) return true;
  const size_t dl = strlen(c.domain);
  const size_t hl = strlen(host);
  return hl > dl + 1 && host[hl - dl - 1] == '.' && iEqual(host + (hl - dl), c.domain, dl);
}

bool CookieJar::pathMatches(const size_t index, const char* path) const {
  const Cookie& c = cookies_[index];
  const size_t cl = strlen(c.path);
  if (cl == 0) return true;
  if (strncmp(path, c.path, cl) != 0) return false;
  if (c.path[cl - 1] == '/') return true;
  return path[cl] == '/' || path[cl] == '\0';
}

CookieJar::SetResult CookieJar::setFromHeader(const char* value, const size_t len, const char* requestHost,
                                              const uint64_t nowUnix) {
  if (value == nullptr || len == 0 || requestHost == nullptr) return SetResult::Rejected;

  const size_t eq = findChar(value, len, 0, '=');
  if (eq == 0 || eq == len) return SetResult::Rejected;

  const char* namePtr = value;
  size_t nameLen = eq;
  trim(namePtr, nameLen);
  const char* valPtr = value + eq + 1;
  size_t valLen = findChar(value, len, eq + 1, ';') - (eq + 1);
  // valLen is computed from the raw span; trim it in place.
  const char* valEnd = value + eq + 1 + valLen;
  trim(valPtr, valLen);
  (void)valEnd;
  if (nameLen == 0 || nameLen >= Cookie::kNameCap) return SetResult::Rejected;
  // A value too long to store is dropped whole. Truncating would send a corrupt
  // cookie that the server rejects with no indication of why.
  if (valLen >= Cookie::kValueCap) return SetResult::Rejected;

  Cookie incoming{};
  copyBounded(incoming.name, sizeof(incoming.name), namePtr, nameLen);
  copyBounded(incoming.value, sizeof(incoming.value), valPtr, valLen);

  char reqHost[Cookie::kDomainCap] = {};
  copyBounded(reqHost, sizeof(reqHost), requestHost, strlen(requestHost));
  for (char* p = reqHost; *p != '\0'; ++p) *p = lower(*p);
  copyBounded(incoming.domain, sizeof(incoming.domain), reqHost, strlen(reqHost));
  incoming.hostOnly = true;
  copyBounded(incoming.path, sizeof(incoming.path), "/", 1);

  bool deleteRequest = (valLen == 0);
  size_t at = findChar(value, len, 0, ';');
  while (at < len) {
    const size_t start = at + 1;
    const size_t end = findChar(value, len, start, ';');
    const char* attrPtr = value + start;
    size_t attrLen = end - start;
    trim(attrPtr, attrLen);
    const size_t aeq = findChar(attrPtr, attrLen, 0, '=');
    const char* an = attrPtr;
    size_t anLen = (aeq == attrLen) ? attrLen : aeq;
    const char* av = (aeq == attrLen) ? "" : attrPtr + aeq + 1;
    size_t avLen = (aeq == attrLen) ? 0 : attrLen - aeq - 1;
    trim(an, anLen);
    trim(av, avLen);

    if (anLen == 6 && iEqual(an, "domain", 6)) {
      const char* d = av;
      size_t dl = avLen;
      if (dl > 0 && d[0] == '.') {
        ++d;
        --dl;
      }
      if (dl > 0 && dl < Cookie::kDomainCap) {
        char buf[Cookie::kDomainCap];
        copyBounded(buf, sizeof(buf), d, dl);
        for (char* p = buf; *p != '\0'; ++p) *p = lower(*p);
        // Accept the domain only if the request host is inside it. Without this a
        // malicious or misconfigured response could set a cookie for a domain it
        // does not own and have it sent elsewhere.
        const size_t bl = strlen(buf);
        const size_t hl = strlen(reqHost);
        const bool inside = (hl == bl && strcmp(buf, reqHost) == 0) ||
                            (hl > bl + 1 && reqHost[hl - bl - 1] == '.' && strcmp(reqHost + (hl - bl), buf) == 0);
        if (inside) {
          copyBounded(incoming.domain, sizeof(incoming.domain), buf, bl);
          incoming.hostOnly = false;
        }
      }
    } else if (anLen == 4 && iEqual(an, "path", 4)) {
      if (avLen > 0 && avLen < Cookie::kPathCap && av[0] == '/') {
        copyBounded(incoming.path, sizeof(incoming.path), av, avLen);
      }
    } else if (anLen == 6 && iEqual(an, "secure", 6)) {
      incoming.secure = true;
    } else if (anLen == 7 && iEqual(an, "max-age", 7)) {
      // A negative or zero max-age is a deletion, which is how a logout is signalled.
      const bool negative = avLen > 0 && av[0] == '-';
      const uint64_t secs = parseUint64(negative ? av + 1 : av, negative ? avLen - 1 : avLen);
      if (negative || secs == 0) {
        deleteRequest = true;
      } else if (nowUnix != 0) {
        incoming.expiresAt = (secs > UINT64_MAX - nowUnix) ? UINT64_MAX : nowUnix + secs;
      }
    } else if (anLen == 7 && iEqual(an, "expires", 7)) {
      // Only the delta form is acted on. Parsing an HTTP-date needs a month table
      // and a civil-from-days conversion, and JJWXC pairs `expires` with `max-age`
      // anyway; ignoring the former costs nothing and avoids a date parser that
      // would go untested against the one thing it exists for.
    }
    at = end;
  }

  const int existing = findSlot(incoming.name, incoming.domain, incoming.path);
  if (deleteRequest) {
    if (existing < 0) return SetResult::Rejected;
    cookies_[static_cast<size_t>(existing)] = Cookie{};
    return SetResult::Removed;
  }
  if (existing >= 0) {
    const size_t idx = static_cast<size_t>(existing);
    if (strcmp(cookies_[idx].value, incoming.value) == 0 && cookies_[idx].expiresAt == incoming.expiresAt) {
      return SetResult::Unchanged;
    }
    cookies_[idx] = incoming;
    return SetResult::Stored;
  }
  // Prefer a reusable empty slot over growing, so a long session that repeatedly
  // replaces cookies does not run out.
  for (size_t i = 0; i < count_; ++i) {
    if (cookies_[i].name[0] == '\0') {
      cookies_[i] = incoming;
      return SetResult::Stored;
    }
  }
  if (count_ >= kMaxCookies) return SetResult::Rejected;
  cookies_[count_++] = incoming;
  return SetResult::Stored;
}

size_t CookieJar::buildHeader(const char* host, const char* path, const bool https, char* out, const size_t outCap,
                              const uint64_t nowUnix, bool* truncated) const {
  if (truncated != nullptr) *truncated = false;
  if (out == nullptr || outCap == 0) return 0;
  out[0] = '\0';
  if (host == nullptr || host[0] == '\0') return 0;
  const char* usePath = (path == nullptr || path[0] == '\0') ? "/" : path;

  char hostBuf[Cookie::kDomainCap];
  copyBounded(hostBuf, sizeof(hostBuf), host, strlen(host));
  for (char* p = hostBuf; *p != '\0'; ++p) *p = lower(*p);

  size_t used = 0;
  for (size_t i = 0; i < count_; ++i) {
    if (!liveAt(i, nowUnix)) continue;
    if (!domainMatches(i, hostBuf)) continue;
    if (!pathMatches(i, usePath)) continue;
    if (cookies_[i].secure && !https) continue;
    const size_t need = strlen(cookies_[i].name) + 1 + cookies_[i].valueLen() + (used > 0 ? 2 : 0);
    if (used + need + 1 > outCap) {
      if (truncated != nullptr) *truncated = true;
      break;
    }
    if (used > 0) {
      out[used++] = ';';
      out[used++] = ' ';
    }
    const size_t nl = strlen(cookies_[i].name);
    memcpy(out + used, cookies_[i].name, nl);
    used += nl;
    out[used++] = '=';
    const size_t vl = cookies_[i].valueLen();
    memcpy(out + used, cookies_[i].value, vl);
    used += vl;
    out[used] = '\0';
  }
  return used;
}

bool CookieJar::has(const char* name, const uint64_t nowUnix) const {
  for (size_t i = 0; i < count_; ++i) {
    if (liveAt(i, nowUnix) && strcmp(cookies_[i].name, name) == 0) return true;
  }
  return false;
}

size_t CookieJar::get(const char* name, char* out, const size_t outCap, const uint64_t nowUnix) const {
  for (size_t i = 0; i < count_; ++i) {
    if (!liveAt(i, nowUnix) || strcmp(cookies_[i].name, name) != 0) continue;
    const size_t vl = cookies_[i].valueLen();
    if (vl + 1 > outCap) return 0;
    memcpy(out, cookies_[i].value, vl);
    out[vl] = '\0';
    return vl;
  }
  return 0;
}

uint32_t CookieJar::readerIdFromSid(const uint64_t nowUnix) const {
  char sid[Cookie::kValueCap];
  if (get("sid", sid, sizeof(sid), nowUnix) == 0) return 0;
  uint32_t v = 0;
  size_t digits = 0;
  for (const char* p = sid; *p != '\0' && *p != '_'; ++p) {
    if (*p < '0' || *p > '9') return 0;
    v = v * 10 + static_cast<uint32_t>(*p - '0');
    if (++digits > 9) return 0;
  }
  return digits > 0 ? v : 0;
}

void CookieJar::clear() {
  for (size_t i = 0; i < kMaxCookies; ++i) cookies_[i] = Cookie{};
  count_ = 0;
}

size_t CookieJar::count(const uint64_t nowUnix) const {
  size_t n = 0;
  for (size_t i = 0; i < count_; ++i) {
    if (liveAt(i, nowUnix)) ++n;
  }
  return n;
}

size_t CookieJar::serialize(char* out, const size_t outCap) const {
  if (out == nullptr || outCap == 0) return 0;
  size_t used = 0;
  out[0] = '\0';
  for (size_t i = 0; i < count_; ++i) {
    if (cookies_[i].name[0] == '\0') continue;
    const int n = snprintf(out + used, outCap - used, "%s\t%s\t%s\t%s\t%d\t%llu\t%d\n", cookies_[i].name,
                           cookies_[i].value, cookies_[i].domain, cookies_[i].path, cookies_[i].secure ? 1 : 0,
                           static_cast<unsigned long long>(cookies_[i].expiresAt), cookies_[i].hostOnly ? 1 : 0);
    if (n <= 0 || used + static_cast<size_t>(n) >= outCap) break;
    used += static_cast<size_t>(n);
  }
  return used;
}

bool CookieJar::deserialize(const char* text, const size_t len) {
  clear();
  if (text == nullptr || len == 0) return false;
  size_t at = 0;
  while (at < len) {
    const size_t nl = findChar(text, len, at, '\n');
    const size_t end = (nl == len) ? len : nl;
    if (end > at) {
      // name \t value \t domain \t path \t secure \t expires \t hostOnly
      const char* fields[7] = {};
      size_t fieldLen[7] = {};
      size_t fi = 0;
      size_t start = at;
      for (size_t i = at; i <= end && fi < 7; ++i) {
        if (i == end || text[i] == '\t') {
          fields[fi] = text + start;
          fieldLen[fi] = i - start;
          ++fi;
          start = i + 1;
        }
      }
      if (fi == 7 && fieldLen[0] > 0 && fieldLen[0] < Cookie::kNameCap && fieldLen[1] < Cookie::kValueCap &&
          fieldLen[2] > 0 && fieldLen[2] < Cookie::kDomainCap && fieldLen[3] < Cookie::kPathCap &&
          count_ < kMaxCookies) {
        Cookie& c = cookies_[count_];
        c = Cookie{};
        copyBounded(c.name, sizeof(c.name), fields[0], fieldLen[0]);
        copyBounded(c.value, sizeof(c.value), fields[1], fieldLen[1]);
        copyBounded(c.domain, sizeof(c.domain), fields[2], fieldLen[2]);
        copyBounded(c.path, sizeof(c.path), fields[3], fieldLen[3]);
        c.secure = (fieldLen[4] == 1 && fields[4][0] == '1');
        char expBuf[24];
        copyBounded(expBuf, sizeof(expBuf), fields[5], fieldLen[5]);
        c.expiresAt = parseUint64(expBuf, strlen(expBuf));
        // hostOnly cannot be inferred from the domain: a stored cookie always has
        // one, whether or not the Set-Cookie named it, and guessing wrong would send
        // it to every subdomain after a reboot.
        c.hostOnly = (fieldLen[6] == 1 && fields[6][0] == '1');
        if (c.name[0] != '\0') ++count_;
      }
    }
    if (nl == len) break;
    at = nl + 1;
  }
  return count_ > 0;
}

bool resolveRedirect(const char* baseUrl, const char* location, char* out, const size_t outCap) {
  if (out == nullptr || outCap == 0 || location == nullptr || location[0] == '\0') return false;
  // "://" only makes it absolute when nothing precedes it. A Location like
  // "/redirect?next=http://m.jjwxc.net/my" contains the separator but is rooted.
  const char* schemeSep = strstr(location, "://");
  if (schemeSep != nullptr && memchr(location, '/', static_cast<size_t>(schemeSep - location)) == nullptr) {
    // Measured: a cookieless GET /my/fav answers
    //   HTTP/2 302
    //   location: http://m.jjwxc.net/my/login?ctime=849877720
    // -- plaintext, from an origin we reached over TLS. The firmware's client is
    // wolfSSL SecureClient and cannot speak HTTP at all; handing it that URL fails in
    // zero milliseconds as a bare network error with nothing to say a redirect was
    // involved. So an http Location reached from an https base is upgraded. Every host
    // this app talks to serves both, and the alternative is a guaranteed failure.
    const bool baseWasHttps = baseUrl != nullptr && strncmp(baseUrl, "https://", 8) == 0;
    const bool locationIsHttp = strncmp(location, "http://", 7) == 0;
    if (baseWasHttps && locationIsHttp) {
      return snprintf(out, outCap, "https://%s", location + 7) < static_cast<int>(outCap);
    }
    if (strlen(location) + 1 > outCap) return false;
    snprintf(out, outCap, "%s", location);
    return true;
  }

  char scheme[16], host[Cookie::kDomainCap], path[256];
  if (baseUrl == nullptr || !splitUrl(baseUrl, scheme, sizeof(scheme), host, sizeof(host), path, sizeof(path))) {
    return false;
  }
  if (location[0] == '/' && location[1] == '/') {
    // Protocol-relative: keep the base scheme.
    return snprintf(out, outCap, "%s:%s", scheme, location) < static_cast<int>(outCap);
  }
  if (location[0] == '/') {
    return snprintf(out, outCap, "%s://%s%s", scheme, host, location) < static_cast<int>(outCap);
  }
  // Relative to the base path's directory. WAP does not currently answer this way,
  // but treating it as absolute would silently request the wrong URL. `path` always
  // starts with '/', so `dir` does too and must not gain another separator.
  char dir[256];
  copyBounded(dir, sizeof(dir), path, strlen(path));
  char* lastSlash = strrchr(dir, '/');
  if (lastSlash != nullptr) lastSlash[1] = '\0';
  else copyBounded(dir, sizeof(dir), "/", 1);
  return snprintf(out, outCap, "%s://%s%s%s", scheme, host, dir, location) < static_cast<int>(outCap);
}

bool splitRequestUrl(const char* url, char* host, const size_t hostCap, char* path, const size_t pathCap) {
  if (url == nullptr || host == nullptr || hostCap == 0 || path == nullptr || pathCap == 0) return false;
  char scheme[16];
  return splitUrl(url, scheme, sizeof(scheme), host, hostCap, path, pathCap);
}

}  // namespace jjwxc
