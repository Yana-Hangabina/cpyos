#include "WebCharset.h"

#include <cctype>

namespace webbrowser {
namespace {

constexpr size_t kNpos = std::string_view::npos;

bool equalsNoCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

size_t findNoCase(const std::string_view hay, const std::string_view needle, const size_t from) {
  if (needle.empty() || from >= hay.size()) return kNpos;
  for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
    if (equalsNoCase(hay.substr(i, needle.size()), needle)) return i;
  }
  return kNpos;
}

std::string lowercase(const std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

// Parses the token after a "charset" keyword that is followed by '='.
std::string parseCharsetValue(const std::string_view s) {
  size_t i = 7;  // past "charset"
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  if (i >= s.size() || s[i] != '=') return {};
  ++i;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  if (i < s.size() && (s[i] == '"' || s[i] == '\'')) ++i;
  const size_t start = i;
  while (i < s.size()) {
    const char c = s[i];
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
      ++i;
    } else {
      break;
    }
  }
  return lowercase(s.substr(start, i - start));
}

}  // namespace

std::string sniffCharset(const std::string_view head, const std::string_view contentType) {
  if (!contentType.empty()) {
    const size_t at = findNoCase(contentType, "charset", 0);
    if (at != kNpos) {
      const std::string value = parseCharsetValue(contentType.substr(at));
      if (!value.empty()) return value;
    }
  }
  // Meta declarations: <meta charset="gbk"> or
  // <meta http-equiv="Content-Type" content="text/html; charset=gbk">.
  size_t at = 0;
  for (int guard = 0; guard < 8; ++guard) {
    at = findNoCase(head, "charset", at);
    if (at == kNpos) break;
    const std::string value = parseCharsetValue(head.substr(at));
    if (!value.empty()) return value;
    at += 7;
  }
  return {};
}

bool isGbkCharset(const std::string_view charset) {
  return charset == "gbk" || charset == "gb2312" || charset == "gb18030" || charset == "x-gbk" ||
         charset == "cp936" || charset == "euc-cn";
}

}  // namespace webbrowser
