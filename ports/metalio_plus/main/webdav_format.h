// Pure formatting/parsing helpers for the WebDAV subset. ESP-IDF-free.
#pragma once

#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>

namespace webdav {

inline std::string xmlEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c;
    }
  }
  return out;
}

// Percent-encodes a logical path for a DAV <href>, keeping '/' separators.
inline std::string hrefEncode(const std::string& path) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(path.size() + 8);
  for (unsigned char c : path) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Accepts an absolute URL (http://host/path) or a path. Returns the logical
// path plus a trailing '/' for collections, or false for a malformed value.
inline bool pathFromDestination(const std::string& value, std::string& out) {
  std::string path = value;
  const size_t scheme = path.find("://");
  if (scheme != std::string::npos) {
    const size_t slash = path.find('/', scheme + 3);
    if (slash == std::string::npos) return false;
    path = path.substr(slash);
  }
  const size_t query = path.find('?');
  if (query != std::string::npos) path.resize(query);
  if (path.empty() || path.front() != '/') return false;
  // Percent-decode.
  std::string decoded;
  decoded.reserve(path.size());
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(static_cast<unsigned char>(path[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
      const auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
      };
      decoded += static_cast<char>((hex(path[i + 1]) << 4) | hex(path[i + 2]));
      i += 2;
    } else {
      decoded += path[i];
    }
  }
  while (decoded.size() > 1 && decoded.back() == '/') decoded.pop_back();
  if (decoded.find("..") != std::string::npos) return false;
  out = decoded;
  return true;
}

// Depth header: "0" -> 0, "1" -> 1, "infinity" -> 1 (bounded), default 1.
inline int parseDepth(const char* value) {
  if (value == nullptr) return 1;
  if (value[0] == '0') return 0;
  return 1;
}

inline bool overwriteAllowed(const char* value) { return !(value != nullptr && (value[0] == 'F' || value[0] == 'f')); }

inline const char* contentTypeFor(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext == "epub") return "application/epub+zip";
  if (ext == "txt" || ext == "md" || ext == "fb2") return "text/plain; charset=utf-8";
  if (ext == "html" || ext == "xhtml") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css";
  if (ext == "json") return "application/json";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "png") return "image/png";
  if (ext == "gif") return "image/gif";
  if (ext == "pdf") return "application/pdf";
  return "application/octet-stream";
}

inline std::string httpDate(std::time_t when) {
  char buffer[40] = {};
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &when);
#else
  gmtime_r(&when, &utc);
#endif
  static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  std::snprintf(buffer, sizeof(buffer), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                days[utc.tm_wday % 7], utc.tm_mday, months[utc.tm_mon % 12], utc.tm_year + 1900,
                utc.tm_hour, utc.tm_min, utc.tm_sec);
  return buffer;
}

}  // namespace webdav
