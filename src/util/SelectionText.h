#pragma once

#include <Utf8.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

// Text assembly for the reader's text selection: joins the selected words into
// one string (the highlight quote) and normalizes it into a dictionary lookup
// phrase, plus the fallback candidate ladder for phrases that are not
// themselves headwords.
//
// Pure code: no HAL, no logging, no allocation beyond what the caller's
// std::string already needs. It lives outside EpubReaderActivity so the rules
// can be host-tested (test/phrase_query) — a wrong separator silently produces
// a phrase that never matches, which is exactly the kind of bug a screenshot
// cannot show.
//
// Separator rules are ports of upstream's clipping text builder (upstream
// c201af43 / d7fcc7ce): a new paragraph is '\n', a source gap is one ' ', and
// tokens that were glued in the source stay glued. A gap between two CJK
// codepoints (Han/Kana) is dropped because there it is a justification
// artifact, not a word break; Hangul is space-delimited and keeps its gaps.
namespace selection_text {

// Dictionary::lookup keeps up to five working buffers and refuses when the heap
// cannot hold 5*wordBytes; a phrase is also pointless beyond a headword (Chinese
// headwords are 2-4 characters). 64 bytes is the cap for both reasons.
inline constexpr size_t kMaxPhraseBytes = 64;
inline constexpr size_t kMaxCandidates = 4;

// One selected word, in reading order. `text` is borrowed (the page that owns
// it must outlive the call) and NUL-terminated.
struct Token {
  const char* text = nullptr;
  uint16_t paragraph = 0xFFFF;  // TextBlock::getParagraphIndex(); 0xFFFF = unknown
  bool gapBefore = false;       // the source had whitespace or a line break before it
};

// One lookup to run: the normalized phrase plus the byte offset of the word the
// user long-pressed, so the CJK window ladder can start at that character even
// when the selection was extended to the left.
struct PhraseQuery {
  std::string text;
  size_t anchorOffset = 0;
};

// The candidate ladder, deduplicated, in lookup order.
struct PhraseCandidates {
  std::string items[kMaxCandidates];
  size_t count = 0;
};

inline bool isAsciiSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline bool isAsciiAlnum(const unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Same notion of a word byte as Dictionary::cleanWord: ASCII alphanumerics plus
// every non-ASCII byte, so accented and CJK words keep their edges.
inline bool isWordByte(const unsigned char c) { return c >= 0x80 || isAsciiAlnum(c); }

// U+0020/09/0A/0D plus U+00A0 NO-BREAK SPACE and U+2002/U+2003 EN/EM SPACE.
// The last three are indentation and layout artifacts, never part of a headword.
inline bool isSpaceCodepoint(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0x00A0 || cp == 0x2002 || cp == 0x2003;
}

// A codepoint that never takes a space before its neighbour: Han, Kana and CJK
// punctuation/fullwidth forms. Hangul is deliberately excluded — Korean is
// space-delimited and the layout keeps Hangul words whole, so a gap there IS a
// word break and gluing the words would produce a phrase no dictionary has.
inline bool gluesToNeighbour(const uint32_t cp) { return utf8IsCjkBreakable(cp) && !utf8IsHangul(cp); }

inline uint32_t firstCodepoint(const std::string_view text) {
  if (text.empty()) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const auto* before = p;
  const uint32_t cp = utf8NextCodepoint(&p);
  return p == before ? 0 : cp;
}

inline uint32_t lastCodepoint(const std::string_view text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const auto* end = p + text.size();
  uint32_t last = 0;
  while (p < end) {
    const auto* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (p == before) break;  // malformed: stop rather than loop
    last = cp;
  }
  return last;
}

// Join the selected words into one string: '\n' between paragraphs, ' ' where
// the source had a gap, nothing where the tokens were glued. Two neighbouring
// CJK codepoints never take a space (see gluesToNeighbour).
inline std::string joinSelection(const Token* tokens, const size_t count) {
  std::string out;
  if (!tokens) return out;
  out.reserve(count * 6);
  uint16_t previousParagraph = 0xFFFF;
  uint32_t previousLast = 0;
  for (size_t i = 0; i < count; i++) {
    const char* text = tokens[i].text;
    if (!text || text[0] == '\0') continue;
    const std::string_view tokenText(text);
    if (!out.empty()) {
      if (tokens[i].paragraph != previousParagraph) {
        out.push_back('\n');
      } else if (tokens[i].gapBefore &&
                 !(gluesToNeighbour(previousLast) && gluesToNeighbour(firstCodepoint(tokenText)))) {
        out.push_back(' ');
      }
    }
    out.append(tokenText);
    previousParagraph = tokens[i].paragraph;
    previousLast = lastCodepoint(tokenText);
  }
  return out;
}

// Cut a phrase down to `limit` bytes. A Latin word is never split (the cut backs
// up to the last space); a CJK run has no spaces, so there the cut lands on a
// codepoint boundary instead.
inline std::string clampPhraseBytes(const std::string& text, const size_t limit = kMaxPhraseBytes) {
  if (text.size() <= limit) return text;
  size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  size_t tokenCut = cut;
  while (tokenCut > 0 && !isAsciiSpace(text[tokenCut - 1])) --tokenCut;
  if (tokenCut > 0) {
    size_t trimmed = tokenCut;
    while (trimmed > 0 && isAsciiSpace(text[trimmed - 1])) --trimmed;
    cut = trimmed;
  }
  return text.substr(0, cut);
}

// Normalize a joined selection into a lookup phrase:
//  - collapse every whitespace run (including paragraph newlines) to one ' ',
//  - strip the edge punctuation Dictionary::cleanWord would strip, including
//    3-byte General Punctuation (curly quotes, dashes, ellipsis),
//  - drop a trailing discretionary hyphen (the word was hyphenated at a line end),
//  - cap the length on a token boundary.
// Case is deliberately untouched: Dictionary::lookup lowercases Latin itself and
// lowercasing here would mangle CJK.
inline std::string normalizePhrase(const std::string_view raw) {
  if (raw.empty()) return {};
  std::string collapsed;
  collapsed.reserve(raw.size());
  bool pendingSpace = false;
  const auto* p = reinterpret_cast<const unsigned char*>(raw.data());
  const auto* end = p + raw.size();
  while (p < end) {
    const auto* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (p == before) {  // malformed byte: skip it
      p = before + 1;
      continue;
    }
    if (isSpaceCodepoint(cp)) {
      pendingSpace = !collapsed.empty();
      continue;
    }
    if (pendingSpace) {
      collapsed.push_back(' ');
      pendingSpace = false;
    }
    collapsed.append(reinterpret_cast<const char*>(before), static_cast<size_t>(p - before));
  }
  if (collapsed.empty()) return {};

  // Trailing discretionary hyphen: "discretion-" from a line-end break must be
  // looked up as "discretion". Only when a word byte precedes it, so a phrase
  // that merely ends in punctuation is left to the edge strip below.
  if (collapsed.size() >= 2 && collapsed.back() == '-') {
    const auto beforeHyphen = static_cast<unsigned char>(collapsed[collapsed.size() - 2]);
    if (isWordByte(beforeHyphen)) collapsed.pop_back();
  }

  const auto* b = reinterpret_cast<const unsigned char*>(collapsed.data());
  size_t start = 0;
  size_t stop = collapsed.size();
  while (start < stop) {
    if (!isWordByte(b[start])) {
      start++;
    } else if (stop - start >= 3 && b[start] == 0xE2 && (b[start + 1] == 0x80 || b[start + 1] == 0x81)) {
      start += 3;  // General Punctuation: “ ” ‘ ’ – — …
    } else {
      break;
    }
  }
  while (stop > start) {
    if (!isWordByte(b[stop - 1])) {
      stop--;
    } else if (stop - start >= 3 && b[stop - 3] == 0xE2 && (b[stop - 2] == 0x80 || b[stop - 2] == 0x81)) {
      stop -= 3;
    } else {
      break;
    }
  }
  if (start >= stop) return {};
  return clampPhraseBytes(collapsed.substr(start, stop - start));
}

// A phrase is worth looking up when at least one non-space character survived.
// Punctuation-only and whitespace-only selections normalize away to nothing.
inline bool hasLookupContent(const std::string_view phrase) {
  if (phrase.empty()) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(phrase.data());
  const auto* end = p + phrase.size();
  while (p < end) {
    const auto* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (p == before) {
      p = before + 1;
      continue;
    }
    if (!isSpaceCodepoint(cp)) return true;
  }
  return false;
}

inline void addCandidate(PhraseCandidates& out, std::string value) {
  if (value.empty()) return;
  for (size_t i = 0; i < out.count; i++) {
    if (out.items[i] == value) return;
  }
  if (out.count >= kMaxCandidates) return;
  out.items[out.count++] = std::move(value);
}

// Candidate ladder, hard-capped at 3 lookups for Latin and 4 for CJK (each
// lookup reopens the .idx and binary-searches, so the cap is SD latency, not
// memory). Chinese headwords are 2-4 characters, so the CJK windows are the only
// fallback that can hit; for Latin the longest token (or the pair around the
// anchor) rescues "on the other hand" style selections.
inline PhraseCandidates phraseCandidates(const std::string_view phrase, const size_t anchorOffset) {
  PhraseCandidates out;
  if (phrase.empty()) return out;
  addCandidate(out, std::string(phrase));

  bool hasSpace = false;
  for (const char c : phrase) {
    if (c == ' ') {
      hasSpace = true;
      break;
    }
  }

  if (!hasSpace) {
    // One unspaced run: only a CJK run has a per-character ladder. A single
    // Latin word is already a complete candidate.
    size_t offsets[kMaxPhraseBytes + 1];
    size_t count = 0;
    bool anyCjk = false;
    const auto* p = reinterpret_cast<const unsigned char*>(phrase.data());
    const auto* end = p + phrase.size();
    while (p < end && count < kMaxPhraseBytes) {
      offsets[count++] = static_cast<size_t>(p - reinterpret_cast<const unsigned char*>(phrase.data()));
      const auto* before = p;
      const uint32_t cp = utf8NextCodepoint(&p);
      if (p == before) p = before + 1;
      anyCjk = anyCjk || gluesToNeighbour(cp);
    }
    if (!anyCjk) return out;
    offsets[count] = phrase.size();
    size_t anchor = 0;
    for (size_t i = 0; i < count; i++) {
      if (offsets[i] <= anchorOffset) anchor = i;
    }
    for (const size_t width : {size_t{4}, size_t{3}, size_t{2}}) {
      if (count <= width) continue;  // the whole phrase already covers this window
      size_t start = anchor + width > count ? count - width : anchor;
      addCandidate(out, std::string(phrase.substr(offsets[start], offsets[start + width] - offsets[start])));
    }
    return out;
  }

  // Whitespace-separated tokens.
  constexpr size_t kMaxTokens = 16;
  size_t tokenStart[kMaxTokens];
  size_t tokenLen[kMaxTokens];
  size_t tokenCount = 0;
  for (size_t i = 0; i < phrase.size() && tokenCount < kMaxTokens;) {
    if (phrase[i] == ' ') {
      i++;
      continue;
    }
    const size_t start = i;
    while (i < phrase.size() && phrase[i] != ' ') i++;
    tokenStart[tokenCount] = start;
    tokenLen[tokenCount] = i - start;
    tokenCount++;
  }
  if (tokenCount < 2) return out;

  size_t anchorToken = 0;
  for (size_t i = 0; i < tokenCount; i++) {
    if (tokenStart[i] <= anchorOffset) anchorToken = i;
  }
  // Longest token; ties go to the anchor's token.
  size_t longest = 0;
  for (size_t i = 1; i < tokenCount; i++) {
    if (tokenLen[i] > tokenLen[longest] || (tokenLen[i] == tokenLen[longest] && i == anchorToken)) longest = i;
  }
  addCandidate(out, std::string(phrase.substr(tokenStart[longest], tokenLen[longest])));
  if (tokenCount >= 3) {
    const size_t first = anchorToken > 0 ? anchorToken - 1 : 0;
    const size_t last = first + 1;
    addCandidate(out, std::string(phrase.substr(tokenStart[first], tokenStart[last] + tokenLen[last] - tokenStart[first])));
  }
  return out;
}

inline PhraseCandidates phraseCandidates(const PhraseQuery& query) {
  return phraseCandidates(std::string_view(query.text), query.anchorOffset);
}

}  // namespace selection_text
