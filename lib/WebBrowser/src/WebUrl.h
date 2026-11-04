#pragma once

#include <string>
#include <string_view>

namespace webbrowser {

struct UrlParts {
  std::string scheme;     // lowercase, no "://"
  std::string authority;  // host[:port]
  std::string path;       // starts with '/' when absolute
  std::string query;      // without the leading '?'
  bool valid = false;
};

// Splits an absolute URL. The fragment is dropped: this browser never renders
// a fragment and keeping it would make history entries look different for the
// same page.
bool splitUrl(std::string_view url, UrlParts& out);
std::string joinUrl(const UrlParts& parts);

// Turns what the user typed into a fetchable absolute URL.
//  - "example.com/x" gains an https:// prefix;
//  - an explicit http:// stays plain HTTP (the transport supports both); the
//    caller should warn that plain HTTP is not private;
//  - non-http(s) schemes, non-ASCII host names (no IDN support) and control
//    characters are rejected instead of being sent as garbage.
bool normalizeHttpUrl(std::string_view input, std::string& out, bool& upgraded);

// Resolves an href against the document URL. In-page references ("#top") set
// sameDocument and leave `out` empty. Unfetchable schemes return false.
bool resolveHref(std::string_view base, std::string_view href, std::string& out, bool& sameDocument);

std::string hostOf(std::string_view url);
std::string originOf(std::string_view url);
bool percentEncodeNonAscii(std::string& url);

}  // namespace webbrowser
