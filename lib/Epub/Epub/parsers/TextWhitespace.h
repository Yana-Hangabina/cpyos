#pragma once

#include <string>

// Whitespace helpers shared by the chapter parser and its host tests.
//
// Nothing here is Arduino- or SD-dependent, so `test/css_cascade` exercises the real code.
namespace textws {

constexpr bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// Normalizes one text run to the form the word splitter expects.
//
// `preserve` follows `white-space: pre` / an HTML <pre>: interior runs of spaces, tabs and
// newlines are kept verbatim (only carriage returns are dropped, since XML parsers already
// normalize line endings). Otherwise the run is trimmed and every interior whitespace run
// collapses to a single space, which is HTML's default `white-space: normal` behaviour.
inline std::string normalizeWhitespace(const std::string& str, const bool preserve) {
  std::string result;
  if (str.empty()) return result;

  if (preserve) {
    result.reserve(str.size());
    for (const char c : str) {
      if (c != '\r') result.push_back(c);
    }
    return result;
  }

  size_t start = 0;
  while (start < str.size() && isWhitespace(str[start])) {
    ++start;
  }
  if (start == str.size()) return result;

  size_t end = str.size() - 1;
  while (end > start && isWhitespace(str[end])) {
    --end;
  }

  result.reserve(end - start + 1);
  bool inSpace = false;
  for (size_t i = start; i <= end; ++i) {
    if (isWhitespace(str[i])) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(str[i]);
      inSpace = false;
    }
  }
  return result;
}

}  // namespace textws
