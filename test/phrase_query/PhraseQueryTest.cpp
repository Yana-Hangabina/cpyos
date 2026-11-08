// Host test for src/util/SelectionText.h — the reader's selection-to-phrase
// assembly (paragraphs, gaps, CJK adjacency, edge cleaning) and the dictionary
// candidate ladder.
//
// Nothing here needs the e-ink HAL, and a wrong separator produces a phrase that
// silently never matches, so every rule is pinned. Run with
// run_phrase_query_test.sh (it also mutates the CJK gap rule and expects red).
#include <cstdio>
#include <string>
#include <vector>

#include "util/SelectionText.h"

namespace {

int gChecks = 0;
int gFailures = 0;

void expectEq(const std::string& actual, const std::string& expected, const std::string& what) {
  ++gChecks;
  if (actual != expected) {
    ++gFailures;
    std::printf("FAIL %s: got '%s', want '%s'\n", what.c_str(), actual.c_str(), expected.c_str());
  }
}

void expectEq(const size_t actual, const size_t expected, const std::string& what) {
  ++gChecks;
  if (actual != expected) {
    ++gFailures;
    std::printf("FAIL %s: got %zu, want %zu\n", what.c_str(), actual, expected);
  }
}

void expectEq(const std::vector<std::string>& actual, const std::vector<std::string>& expected,
              const std::string& what) {
  ++gChecks;
  if (actual == expected) return;
  ++gFailures;
  std::string got;
  for (const auto& item : actual) got += "[" + item + "]";
  std::string want;
  for (const auto& item : expected) want += "[" + item + "]";
  std::printf("FAIL %s: got %s, want %s\n", what.c_str(), got.c_str(), want.c_str());
}

void expectTrue(const bool value, const std::string& what) {
  ++gChecks;
  if (!value) {
    ++gFailures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

using selection_text::Token;

Token tok(const char* text, const uint16_t paragraph = 1, const bool gapBefore = true) {
  return Token{text, paragraph, gapBefore};
}

std::string join(const std::vector<Token>& tokens) {
  return selection_text::joinSelection(tokens.data(), tokens.size());
}

void testLatinJoin() {
  // A gap becomes one space; a glued token keeps none (the source had no
  // whitespace, e.g. punctuation attached to the previous word).
  expectEq(join({tok("the"), tok("quick"), tok("brown")}), "the quick brown", "latin gaps");
  expectEq(join({tok("quick"), tok("brown", 1, false)}), "quickbrown", "latin glued");
  // A new paragraph always starts a new line, whatever the gap flag says.
  expectEq(join({tok("one"), tok("two", 2)}), "one\ntwo", "paragraph break");
  // The first token never carries a leading separator.
  expectEq(join({tok("first", 7, true)}), "first", "leading gap dropped");
  expectEq(join({}), "", "empty selection");
}

void testCjkJoin() {
  // CJK is laid out per character, so the pixel gap between two characters is a
  // justification artifact, not a word break: no space must be inserted.
  expectEq(join({tok("\xE4\xBD\xA0"), tok("\xE5\xA5\xBD")}), "\xE4\xBD\xA0\xE5\xA5\xBD", "cjk gap suppressed");
  // A paragraph break still wins over the CJK rule.
  expectEq(join({tok("\xE4\xBD\xA0"), tok("\xE5\xA5\xBD", 2)}), "\xE4\xBD\xA0\n\xE5\xA5\xBD", "cjk paragraph");
  // Latin next to CJK keeps the space the source had.
  expectEq(join({tok("\xE4\xBD\xA0"), tok("world")}), "\xE4\xBD\xA0 world", "cjk to latin keeps gap");
  expectEq(join({tok("world"), tok("\xE5\xA5\xBD")}), "world \xE5\xA5\xBD", "latin to cjk keeps gap");
  // Hangul is space-delimited text, so two Hangul words do take a space; only a
  // glued pair stays glued. (utf8IsCjkBreakable covers Hangul too, so this pins
  // that the rule is about adjacency, not the script.)
  expectEq(join({tok("\xEC\x95\x88\xEB\x85\x95"), tok("\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94")}),
           "\xEC\x95\x88\xEB\x85\x95 \xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94", "hangul words");
}

void testNormalize() {
  expectEq(selection_text::normalizePhrase("  the   quick\n brown "), "the quick brown", "collapse whitespace");
  // Em space (U+2003) and no-break space (U+00A0) are indentation artifacts.
  expectEq(selection_text::normalizePhrase("\xE2\x80\x83word"), "word", "em-space prefix");
  expectEq(selection_text::normalizePhrase("\xC2\xA0word"), "word", "nbsp prefix");
  expectEq(selection_text::normalizePhrase("\xE2\x80\x83\xE2\x80\x83word two"), "word two", "repeated em-space");
  // A discretionary hyphen from a line-end break is not part of the word.
  expectEq(selection_text::normalizePhrase("discretion-"), "discretion", "trailing hyphen");
  // An interior line-end hyphen cannot be told apart from a real one, so only
  // the phrase-final hyphen is removed (Dictionary::lookup has no hyphenation).
  // Edge punctuation, including the General Punctuation block (curly quotes and
  // dashes) that Dictionary::cleanWord also strips.
  expectEq(selection_text::normalizePhrase("\xE2\x80\x9C" "hello\xE2\x80\x9D"), "hello", "curly quotes");
  expectEq(selection_text::normalizePhrase("(word),"), "word", "ascii edges");
  expectEq(selection_text::normalizePhrase("\xE2\x80\x94"), "", "lone dash rejected");
  expectEq(selection_text::normalizePhrase("\xE2\x80\xA6"), "", "ellipsis rejected");
  expectEq(selection_text::normalizePhrase("   "), "", "whitespace only rejected");
  // Case is left alone: the dictionary lowercases Latin on its own, and
  // lowercasing a CJK phrase would be wrong rather than useless.
  expectEq(selection_text::normalizePhrase("Hello World"), "Hello World", "case preserved");
  expectEq(selection_text::normalizePhrase("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"),
           "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C", "cjk untouched");
}

void testClamp() {
  const std::string phrase = selection_text::normalizePhrase(
      "alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november");
  expectTrue(phrase.size() <= selection_text::kMaxPhraseBytes, "clamp respects the byte cap");
  // A Latin cut lands on a token boundary: the longest whole-word prefix that
  // fits, never a truncated word ("kilo" would push it past 64 bytes).
  expectEq(phrase, "alpha bravo charlie delta echo foxtrot golf hotel india juliet", "clamp keeps whole words");
  // A long CJK run has no spaces; the cut must land on a codepoint boundary.
  std::string cjk;
  for (int i = 0; i < 40; i++) cjk += "\xE4\xBD\xA0";  // 120 bytes of U+4F60
  const std::string clamped = selection_text::clampPhraseBytes(cjk);
  expectTrue(clamped.size() <= selection_text::kMaxPhraseBytes, "cjk clamp respects the byte cap");
  expectEq(clamped.size() % 3, 0ul, "cjk clamp lands on a codepoint boundary");
  expectEq(selection_text::clampPhraseBytes("short"), "short", "short phrase untouched");
}

void testLookupContent() {
  expectTrue(!selection_text::hasLookupContent(""), "empty has no content");
  expectTrue(!selection_text::hasLookupContent(" "), "space has no content");
  expectTrue(selection_text::hasLookupContent("\xE4\xBD\xA0"), "single cjk char has content");
  expectTrue(selection_text::hasLookupContent("a"), "single latin letter has content");
}

std::vector<std::string> candidates(const std::string& phrase, const size_t anchor = 0) {
  const auto list = selection_text::phraseCandidates(phrase, anchor);
  std::vector<std::string> out;
  for (size_t i = 0; i < list.count; i++) out.push_back(list.items[i]);
  return out;
}

void testLatinLadder() {
  // Two tokens: the whole phrase, then the longest token.
  expectEq(candidates("of the"), std::vector<std::string>{"of the", "the"}, "latin two-token ladder");
  // Three tokens: whole, longest, then the pair around the anchor.
  const auto three = candidates("in the beginning");
  expectEq(three.size(), 3ul, "latin three-token ladder size");
  expectEq(three[0], "in the beginning", "latin ladder: whole phrase");
  expectEq(three[1], "beginning", "latin ladder: longest token");
  expectEq(three[2], "in the", "latin ladder: pair at the anchor");
  // The anchor moves the pair: anchor on "the" (byte 3) pairs it with "in".
  const auto anchored = candidates("in the beginning", 3);
  expectEq(anchored[2], "in the", "latin ladder: anchor 3");
  // A single token is only itself.
  expectEq(candidates("groups"), std::vector<std::string>{"groups"}, "latin single token");
}

void testCjkLadder() {
  // 4 characters: the whole run is the 4-window, so the ladder is 4, 3, 2.
  const std::string four = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C";  // 你好世界
  const auto list4 = candidates(four);
  expectEq(list4.size(), 3ul, "cjk four-char ladder size");
  expectEq(list4[0], four, "cjk ladder: whole run");
  expectEq(list4[1].size(), 9ul, "cjk ladder: 3-codepoint window");
  expectEq(list4[2].size(), 6ul, "cjk ladder: 2-codepoint window");
  // 5 characters, anchor on the third: windows still contain the anchor.
  const std::string five =
      "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B\xE4\xBA\x94";  // 一二三四五
  const auto list5 = candidates(five, 6);
  expectEq(list5.size(), 4ul, "cjk five-char ladder size");
  expectEq(list5[1].size(), 12ul, "cjk ladder: 4-codepoint window");
  expectTrue(list5[1].find("\xE4\xB8\x89") != std::string::npos, "cjk 4-window contains the anchor");
  expectTrue(list5[2].find("\xE4\xB8\x89") != std::string::npos, "cjk 3-window contains the anchor");
  expectTrue(list5[3].find("\xE4\xB8\x89") != std::string::npos, "cjk 2-window contains the anchor");
  // Two characters: no window is shorter than the run, and none equals it.
  expectEq(candidates("\xE4\xBD\xA0\xE5\xA5\xBD").size(), 1ul, "cjk two-char ladder size");
  // A mixed phrase with a space takes the Latin ladder, not CJK windows.
  expectEq(candidates("\xE4\xBD\xA0\xE5\xA5\xBD world")[0], "\xE4\xBD\xA0\xE5\xA5\xBD world", "mixed phrase whole");
}

void testQueryForm() {
  selection_text::PhraseQuery query;
  query.text = "in the beginning";
  query.anchorOffset = 3;
  const auto list = selection_text::phraseCandidates(query);
  expectEq(list.count, 3ul, "phraseCandidates(PhraseQuery) size");
  expectEq(list.items[0], "in the beginning", "phraseCandidates(PhraseQuery) whole");
}

}  // namespace

int main() {
  testLatinJoin();
  testCjkJoin();
  testNormalize();
  testClamp();
  testLookupContent();
  testLatinLadder();
  testCjkLadder();
  testQueryForm();
  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
