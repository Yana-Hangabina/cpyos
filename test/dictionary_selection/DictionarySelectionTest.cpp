// Host regression for the plain-definition word tokenizer that feeds the
// recursive dictionary viewer's selection page. Pure code: Utf8 helpers only.
#include <cstdio>
#include <string>
#include <vector>

#include "util/PlainTextTokens.h"

namespace {

int g_failures = 0;

void fail(const char* what, const std::string& detail) {
  std::printf("FAIL: %s (%s)\n", what, detail.c_str());
  g_failures++;
}

void expectTokens(const std::string& line, const std::vector<std::string>& expected, const char* what) {
  std::vector<PlainTextToken> tokens;
  tokenizePlainTextLine(line.data(), line.size(), tokens);
  std::vector<std::string> got;
  got.reserve(tokens.size());
  size_t previousEnd = 0;
  for (const auto& t : tokens) {
    if (t.len == 0 || t.start < previousEnd || t.start + t.len > line.size()) {
      fail(what, "token escapes the line or overlaps");
      return;
    }
    previousEnd = t.start + t.len;
    got.push_back(line.substr(t.start, t.len));
  }
  if (got == expected) return;
  std::string detail = "got [";
  for (const auto& g : got) detail += "'" + g + "' ";
  detail += "] expected [";
  for (const auto& e : expected) detail += "'" + e + "' ";
  detail += "]";
  fail(what, detail);
}

}  // namespace

int main() {
  expectTokens("hello world", {"hello", "world"}, "ascii words");
  expectTokens("  leading   trailing  ", {"leading", "trailing"}, "whitespace runs");
  expectTokens("a\tb\rc", {"a", "b", "c"}, "tab and CR separators");
  // Han splits per character; adjacent Latin is its own word.
  expectTokens("\xe6\xB0\xB4\xe6\xB7\xB1\xe7\x81\xAB\xe7\x83\xAD",  // 水深火热
               {"\xe6\xB0\xB4", "\xe6\xB7\xB1", "\xe7\x81\xAB", "\xe7\x83\xAD"}, "han per character");
  expectTokens("\xe6\xB0\xB4word\xe7\xBA\xB9", {"\xe6\xB0\xB4", "word", "\xe7\xBA\xB9"},  // 水word纹
               "han/latin boundary");
  expectTokens("\xE3\x85\x9C\xEC\x8A\xA4\xED\x8A\xB8 \xEB\x8B\xA8\xEC\x96\xB4",  // 테스트 단어
               {"\xE3\x85\x9C\xEC\x8A\xA4\xED\x8A\xB8", "\xEB\x8B\xA8\xEC\x96\xB4"}, "hangul stays whole");
  // "cafe" + U+0301 combining acute: the mark stays inside one token.
  expectTokens("cafe\xCC\x81 test", {"cafe\xCC\x81", "test"}, "combining mark");
  // CJK punctuation is its own selectable token.
  expectTokens("\xe6\xB0\xB4\xE3\x80\x82\xE7\x81\xAB", {"\xe6\xB0\xB4", "\xE3\x80\x82", "\xE7\x81\xAB"},
               "cjk punctuation");
  // Degenerate bytes must still advance and stay inside the line; they are not
  // word boundaries, so a run of them stays one token.
  expectTokens("\xFF\xFE", {"\xFF\xFE"}, "invalid bytes");
  // Long unbroken run: caller caps the length; one token if under the cap.
  {
    const std::string longWord(200, 'a');
    expectTokens(longWord, {longWord}, "long token");
  }
  // Token bound: a 400-byte run of alternating Han/space can't loop forever.
  {
    std::string dense;
    for (int i = 0; i < 200; i++) dense += "\xe6\xB0\xB4 ";
    std::vector<PlainTextToken> tokens;
    tokenizePlainTextLine(dense.data(), dense.size(), tokens);
    if (tokens.size() != 96) fail("dense input", "expected the 96-token bound");
  }

  if (g_failures == 0) {
    std::printf("dictionary selection tokenizer: ALL PASS\n");
    return 0;
  }
  std::printf("dictionary selection tokenizer: %d failure(s)\n", g_failures);
  return 1;
}
