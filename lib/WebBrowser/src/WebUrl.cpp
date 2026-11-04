#include "WebUrl.h"

#include <cctype>
#include <cstdint>
#include <vector>

namespace webbrowser {
namespace {

std::string_view trimView(std::string_view s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

std::string toLower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

bool hasControlChar(std::string_view s) {
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) return true;
  }
  return false;
}

bool isAsciiHost(std::string_view authority) {
  for (const char c : authority) {
    if (static_cast<unsigned char>(c) >= 0x80) return false;
    if (c == ' ') return false;
  }
  return !authority.empty();
}

// RFC 3986 5.2.4, for absolute paths only.
std::string normalizePath(std::string_view path) {
  std::vector<std::string_view> segments;
  bool trailingSlash = path.empty() || path.back() == '/';
  size_t i = 0;
  while (i <= path.size()) {
    const size_t slash = path.find('/', i);
    const size_t end = slash == std::string_view::npos ? path.size() : slash;
    const std::string_view segment = path.substr(i, end - i);
    if (segment == "..") {
      if (!segments.empty()) segments.pop_back();
    } else if (segment != "." && !segment.empty()) {
      segments.push_back(segment);
    } else if (segment == ".") {
      trailingSlash = true;
    }
    if (slash == std::string_view::npos) break;
    i = slash + 1;
  }
  std::string out;
  for (const auto& segment : segments) {
    out.push_back('/');
    out.append(segment);
  }
  if (out.empty() || (trailingSlash && out.back() != '/')) out.push_back('/');
  return out;
}

void splitPathAndQuery(std::string_view s, std::string& path, std::string& query, bool& hasQuery) {
  const size_t hash = s.find('#');
  if (hash != std::string_view::npos) s = s.substr(0, hash);
  const size_t q = s.find('?');
  hasQuery = q != std::string_view::npos;
  if (hasQuery) {
    path = std::string(s.substr(0, q));
    query = std::string(s.substr(q + 1));
  } else {
    path = std::string(s);
    query.clear();
  }
}

}  // namespace

bool splitUrl(const std::string_view url, UrlParts& out) {
  out = UrlParts{};
  std::string_view s = trimView(url);
  const size_t schemeEnd = s.find("://");
  if (schemeEnd == std::string_view::npos) return false;
  out.scheme = toLower(s.substr(0, schemeEnd));
  std::string_view rest = s.substr(schemeEnd + 3);
  const size_t hash = rest.find('#');
  if (hash != std::string_view::npos) rest = rest.substr(0, hash);
  const size_t q = rest.find('?');
  std::string_view beforeQuery = rest;
  if (q != std::string_view::npos) {
    out.query = std::string(rest.substr(q + 1));
    beforeQuery = rest.substr(0, q);
  }
  const size_t slash = beforeQuery.find('/');
  if (slash == std::string_view::npos) {
    out.authority = std::string(beforeQuery);
    out.path = "/";
  } else {
    out.authority = std::string(beforeQuery.substr(0, slash));
    out.path = std::string(beforeQuery.substr(slash));
  }
  out.valid = (out.scheme == "http" || out.scheme == "https") && !out.authority.empty();
  return out.valid;
}

std::string joinUrl(const UrlParts& parts) {
  std::string out = parts.scheme;
  out += "://";
  out += parts.authority;
  out += parts.path.empty() ? "/" : parts.path;
  if (!parts.query.empty()) {
    out.push_back('?');
    out += parts.query;
  }
  return out;
}

bool percentEncodeNonAscii(std::string& url) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(url.size() + 8);
  bool changed = false;
  for (const char c : url) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x80 || u == ' ' || u == '"' || u == '<' || u == '>' || u == '`' || u < 0x20) {
      out.push_back('%');
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 0x0F]);
      changed = true;
    } else {
      out.push_back(c);
    }
  }
  if (changed) url = std::move(out);
  return changed;
}

bool normalizeHttpUrl(const std::string_view input, std::string& out, bool& upgraded) {
  upgraded = false;
  std::string_view s = trimView(input);
  if (s.empty() || hasControlChar(s)) return false;

  std::string candidate;
  if (s.find("://") == std::string_view::npos) {
    // Reject a foreign scheme ("javascript:", "mailto:") before treating the
    // text as a host. A colon followed only by digits is a port, not a scheme.
    const size_t colon = s.find(':');
    const size_t boundary = s.find_first_of("/?#");
    if (colon != std::string_view::npos && (boundary == std::string_view::npos || colon < boundary)) {
      bool portLike = colon + 1 < s.size();
      for (size_t i = colon + 1; i < s.size() && (boundary == std::string_view::npos || i < boundary); ++i) {
        if (s[i] < '0' || s[i] > '9') {
          portLike = false;
          break;
        }
      }
      if (!portLike) return false;
    }
    candidate = s.rfind("//", 0) == 0 ? "https:" + std::string(s) : "https://" + std::string(s);
  } else {
    candidate = std::string(s);
  }

  UrlParts parts;
  if (!splitUrl(candidate, parts)) return false;
  // Explicit http:// stays plain HTTP: the transport now speaks both schemes.
  // A bare host still defaults to https, so the insecure path is opt-in.
  upgraded = false;
  if (!isAsciiHost(parts.authority)) return false;
  if (parts.path.empty()) parts.path = "/";
  percentEncodeNonAscii(parts.path);
  percentEncodeNonAscii(parts.query);
  out = joinUrl(parts);
  return true;
}

bool resolveHref(const std::string_view base, const std::string_view href, std::string& out,
                 bool& sameDocument) {
  sameDocument = false;
  out.clear();
  const std::string_view h = trimView(href);
  if (h.empty()) return false;
  if (h.front() == '#') {
    sameDocument = true;
    return true;
  }

  // A scheme is "name:" before the first '/', '?' or '#'.
  const size_t colon = h.find(':');
  const size_t slash = h.find_first_of("/?#");
  if (colon != std::string_view::npos && (slash == std::string_view::npos || colon < slash)) {
    const std::string scheme = toLower(h.substr(0, colon));
    if (scheme == "http" || scheme == "https") {
      bool upgraded = false;
      return normalizeHttpUrl(std::string(h), out, upgraded);
    }
    return false;  // javascript:, mailto:, data:, tel:, about:, ...
  }

  UrlParts baseParts;
  if (!splitUrl(base, baseParts)) return false;

  if (h.rfind("//", 0) == 0) {
    bool upgraded = false;
    return normalizeHttpUrl(baseParts.scheme + ":" + std::string(h), out, upgraded);
  }

  std::string mergedPath;
  std::string query;
  bool hasQuery = false;
  if (h.front() == '?') {
    mergedPath = baseParts.path.empty() ? "/" : baseParts.path;
    query = std::string(h.substr(1));
    const size_t hash = query.find('#');
    if (hash != std::string::npos) query.erase(hash);
    hasQuery = true;
  } else {
    std::string rawPath;
    splitPathAndQuery(h, rawPath, query, hasQuery);
    if (!rawPath.empty() && rawPath.front() == '/') {
      mergedPath = normalizePath(rawPath);
    } else {
      const size_t lastSlash = baseParts.path.rfind('/');
      const std::string_view dir = lastSlash == std::string::npos
                                       ? std::string_view("/")
                                       : std::string_view(baseParts.path).substr(0, lastSlash + 1);
      mergedPath = normalizePath(std::string(dir) + rawPath);
    }
  }

  UrlParts parts;
  parts.scheme = baseParts.scheme;
  parts.authority = baseParts.authority;
  parts.path = mergedPath;
  parts.query = query;
  parts.valid = true;
  percentEncodeNonAscii(parts.path);
  percentEncodeNonAscii(parts.query);
  out = joinUrl(parts);
  return true;
}

std::string hostOf(const std::string_view url) {
  UrlParts parts;
  if (!splitUrl(url, parts)) return {};
  std::string host = parts.authority;
  const size_t at = host.find('@');
  if (at != std::string::npos) host.erase(0, at + 1);
  const size_t port = host.rfind(':');
  if (port != std::string::npos && host.find(']') == std::string::npos) host.erase(port);
  return host;
}

std::string originOf(const std::string_view url) {
  UrlParts parts;
  if (!splitUrl(url, parts)) return {};
  return parts.scheme + "://" + parts.authority;
}

}  // namespace webbrowser
