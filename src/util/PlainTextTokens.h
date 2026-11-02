#pragma once

#include <Utf8.h>

#include <cstddef>
#include <vector>

// One selectable token of a plain-text dictionary line: a byte span relative
// to the line's first byte. Shared by the definition viewer, which packs a
// page of these into TextBlock words so the existing word-selection hit
// testing can run over plain definitions too.
struct PlainTextToken {
  size_t start;
  size_t len;
};

// Split a plain-text line into selectable tokens: whitespace separates words,
// and Han/Kana break per codepoint so a Chinese definition offers single
// characters instead of the whole unspaced run. Hangul stays word-whole and a
// combining mark never starts a token (matching the reader's line layout).
// `out` is cleared; tokens never overlap and always advance.
inline void tokenizePlainTextLine(const char* text, const size_t length, std::vector<PlainTextToken>& out) {
  // One token per two bytes is the most a line can hold; bound the loop even
  // if a malicious dictionary yields degenerate boundaries.
  constexpr size_t kMaxTokens = 96;
  out.clear();
  size_t i = 0;
  while (i < length) {
    const char c = text[i];
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }
    const size_t start = i;
    const auto* ptr = reinterpret_cast<const unsigned char*>(text + i);
    uint32_t previous = utf8NextCodepoint(&ptr);
    i = static_cast<size_t>(ptr - reinterpret_cast<const unsigned char*>(text));
    while (i < length) {
      if (text[i] == ' ' || text[i] == '\t' || text[i] == '\r') break;
      ptr = reinterpret_cast<const unsigned char*>(text + i);
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (!utf8IsHangul(previous) && !utf8IsHangul(cp) && (utf8IsCjkBreakable(previous) || utf8IsCjkBreakable(cp)) &&
          !utf8IsCombiningMark(cp)) {
        break;
      }
      previous = cp;
      i = static_cast<size_t>(ptr - reinterpret_cast<const unsigned char*>(text));
    }
    if (i == start) i++;  // malformed input must still advance
    out.push_back({start, i - start});
    if (out.size() >= kMaxTokens) break;
  }
}
