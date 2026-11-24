#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Appends a Unicode codepoint to a std::string in UTF-8 encoding.
void utf8AppendCodepoint(uint32_t cp, std::string& out);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);

// Canonical composition (NFC) for the Latin / Vietnamese range and Hangul:
// precomposes a base letter followed by combining diacritical mark(s), and
// conjoining Hangul jamo sequences (L+V[+T]), into single codepoints. Needed
// because the device fonts have no combining-mark positioning and carry only
// precomposed Hangul syllables, so NFD text (some EPUB chapter titles; every
// filename written by macOS) otherwise renders broken or blank.
std::string utf8ComposeNfc(const std::string& in);

// Compose a null-terminated display buffer without allocating or growing it.
// Uncomposed bytes (including malformed UTF-8) are preserved unchanged.
void utf8ComposeNfcInPlace(char* buffer);

// The base letter a precomposed codepoint decomposes to, or 0 when there is
// none ("é" -> "e", but "ø" -> 0: it is a letter in its own right, not
// o-with-stroke). Lives here rather than in a caller because the compose table
// is a ~5 KB static array in the header: a second includer is a second copy in
// flash.
//
// A linear scan, unlike utf8ComposePair's binary search above it: the table is
// sorted by (base, mark), which this lookup searches against the grain. Adding
// a second table sorted by composed would cost more flash than sharing this one
// saves.
uint32_t utf8DecomposedBase(uint32_t cp);

// Simple (1:1) uppercase mapping for CSS small-caps: Latin, Greek and Cyrillic
// only. Codepoints without a mapping are returned unchanged, which is what a
// small-caps run over CJK text needs (no case, so no change).
uint32_t utf8UppercaseForSmallCaps(uint32_t cp);

// Truncate a raw char buffer to the last complete UTF-8 codepoint boundary.
// Returns the new length (<= len). If the buffer ends mid-sequence, the
// incomplete trailing bytes are excluded.
int utf8SafeTruncateBuffer(const char* buf, int len);

// Returns true for CJK characters that break without hyphenation. Covers CJK Unified Ideographs,
// Hiragana, Katakana, Hangul, CJK punctuation, and fullwidth forms. Han and Kana have implicit
// word boundaries per character; Hangul is space-delimited, so layout keeps Hangul words whole
// (see utf8IsHangul) and splits one without a hyphen only through the hyphenation path.
inline bool utf8IsCjkBreakable(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x3000 && cp <= 0x303F)     // CJK Symbols and Punctuation
         || (cp >= 0x3040 && cp <= 0x309F)     // Hiragana
         || (cp >= 0x30A0 && cp <= 0x30FF)     // Katakana
         || (cp >= 0x3130 && cp <= 0x318F)     // Hangul Compatibility Jamo
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xAC00 && cp <= 0xD7AF)     // Hangul Syllables
         || (cp >= 0xD7B0 && cp <= 0xD7FF)     // Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2A6DF)   // CJK Extension B
         || (cp >= 0x2A700 && cp <= 0x2B73F);  // CJK Extension C
}

// Returns true for codepoints that carry no ink. Beyond ASCII whitespace this covers the
// spaces OCR'd Chinese text uses as filler -- ideographic space, NBSP, the U+2000-U+200A
// run, narrow/medium-mathematical spaces -- and the zero-width characters (ZWSP, ZWNJ,
// ZWJ, BOM). Needed because "is this paragraph blank?" cannot be answered with a byte
// comparison: a block of U+3000 is invisible on paper yet non-empty as bytes, and a
// renderer that only tests for ASCII space draws it as a blank line. (The watermark
// ZWNJ case is why ZW is included: JJWXC's VIP pages pad text with U+200C.)
inline bool utf8IsBlankCodepoint(const uint32_t cp) {
  return cp <= 0x20                                  // NUL..space: controls and space
         || cp == 0xA0 || cp == 0xAD                 // NBSP, soft hyphen
         || (cp >= 0x2000 && cp <= 0x200F)           // spaces, ZWSP, ZWNJ, ZWJ, marks
         || cp == 0x202F || cp == 0x205F             // narrow no-break, medium mathematical
         || cp == 0x2060 || cp == 0xFEFF             // word joiner, BOM
         || cp == 0x3000 || cp == 0x3164;            // ideographic space, Hangul filler
}

// True when `s` is empty or consists of nothing but blank codepoints.
inline bool utf8IsBlankText(const std::string& s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  const unsigned char* end = p + s.size();
  while (p < end) {
    const unsigned char* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (p == before) return false;  // malformed bytes are content, not blankness
    if (!utf8IsBlankCodepoint(cp)) return false;
  }
  return true;
}

// Returns true for Hangul letters. Korean separates words with spaces, so a boundary touching
// Hangul is never an implicit line-break opportunity (CSS word-break: keep-all).
inline bool utf8IsHangul(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)      // Hangul Jamo
         || (cp >= 0x3130 && cp <= 0x318F)   // Hangul Compatibility Jamo
         || (cp >= 0xA960 && cp <= 0xA97F)   // Hangul Jamo Extended-A
         || (cp >= 0xAC00 && cp <= 0xD7FF)   // Hangul Syllables, Hangul Jamo Extended-B
         || (cp >= 0xFFA0 && cp <= 0xFFDC);  // Halfwidth Hangul
}

// Hebrew combining marks (niqqud/cantillation) and basic Arabic harakat carry no
// letter of their own; strip them before a dictionary lookup.
inline bool isHebrewMark(const uint32_t cp) {
  return (cp >= 0x0591 && cp <= 0x05BD) || cp == 0x05BF || (cp >= 0x05C1 && cp <= 0x05C2) ||
         (cp >= 0x05C4 && cp <= 0x05C5) || (cp >= 0x05C7 && cp <= 0x05C9) || cp == 0xFB1E;
}
inline bool isArabicVowelMark(const uint32_t cp) {
  return (cp >= 0x064B && cp <= 0x0650) || cp == 0x0652 || cp == 0x0670;
}

// Returns true for Unicode combining diacritical marks that should not advance the cursor.
inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F)      // Combining Diacritical Marks
         || (cp >= 0x1DC0 && cp <= 0x1DFF)   // Combining Diacritical Marks Supplement
         || (cp >= 0x20D0 && cp <= 0x20FF)   // Combining Diacritical Marks for Symbols
         || (cp >= 0xFE20 && cp <= 0xFE2F);  // Combining Half Marks
}
