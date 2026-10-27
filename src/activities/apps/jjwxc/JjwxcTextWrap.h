#pragma once

#include <cstddef>
#include <cstdint>

// Pure break rules for the JJWXC metadata fields the book-detail page shows.
//
// Header-only and free of any renderer, theme or Arduino dependency, so the very
// same code runs on the device and in the host test (cpyos/src/JjwxcTextWrapTest.cpp).
// Nothing here allocates and nothing here measures: the caller supplies a pixel-width
// callback and gets byte ranges back, which is what lets one walk feed both the cached
// layout and the drawing loop (AGENTS.md rule #14 -- drawing and hit-testing share one
// geometry source).
//
// The two rules come from what the fields actually contain:
//   - novelTags is a separator-joined list ("甜文 情有独钟", or comma-joined
//     "灵异神怪,种田文"), so a break is only legal AT a separator. Splitting a tag in
//     half is worse than the single clipped line this replaces.
//   - novelClass is a dash-joined path ("原创-纯爱-近代现代-爱情"), so the legal breaks
//     are right after a '-'; a class with no dash falls back to breaking between code
//     points, exactly like prose.
// Neither may split a UTF-8 sequence: half a code point reaching drawText() renders as
// a box, which is the one failure this app's text handling is not allowed to have.

namespace jjwxc {

enum class WrapField : uint8_t {
  Plain,     // prose (the blurb): break between any two code points, hard-break at '\n'
  Tags,      // novelTags: break only at separators
  Category,  // novelClass: break after '-', else like Plain
};

// One wrapped line: a byte range [offset, offset + bytes) of the source text. Kept as a
// POD so a list of them can live in a PSRAM-backed vector instead of as std::strings in
// the ~40 KB of internal RAM this app has left.
struct WrapLine {
  uint32_t offset = 0;
  uint16_t bytes = 0;
};

inline bool isWrapLineBreak(const char c) { return c == '\n' || c == '\r'; }

// Byte length of the code point at `pos`, from its lead byte. A stray continuation byte
// (a malformed stream) is one byte, which keeps every walk below moving instead of
// spinning. Never runs past `len`.
inline size_t utf8CharLen(const char* text, const size_t len, const size_t pos) {
  if (text == nullptr || pos >= len) return 0;
  const auto lead = static_cast<unsigned char>(text[pos]);
  size_t n = 1;
  if (lead >= 0xF0) n = 4;
  else if (lead >= 0xE0) n = 3;
  else if (lead >= 0xC0) n = 2;
  if (pos + n > len) n = len - pos;
  return n;
}

// True when the code point at `pos` is one of the separators a tag list may be joined
// with: ASCII space/tab/comma/semicolon/pipe/slash, plus the CJK ones a Chinese site
// actually emits -- U+00B7 '·', U+3000 ideographic space, U+3001 '、', U+FF0C '，',
// U+FF1B '；'.
inline bool isWrapSeparator(const char* text, const size_t len, const size_t pos) {
  if (text == nullptr || pos >= len) return false;
  const auto c0 = static_cast<unsigned char>(text[pos]);
  if (c0 == ' ' || c0 == '\t' || c0 == ',' || c0 == ';' || c0 == '|' || c0 == '/') return true;
  const auto at = [text, len](const size_t i) {
    return i < len ? static_cast<unsigned char>(text[i]) : 0u;
  };
  if (c0 == 0xC2) return at(pos + 1) == 0xB7;                                                // ·
  if (c0 == 0xE3) return at(pos + 1) == 0x80 && (at(pos + 2) == 0x81 || at(pos + 2) == 0x80);  // 、 / CJK space
  if (c0 == 0xEF) return at(pos + 1) == 0xBC && (at(pos + 2) == 0x8C || at(pos + 2) == 0x9B);  // ， / ；
  return false;
}

// Length of the run of separators at `pos` (0 when there is none).
inline size_t wrapSeparatorRunLen(const char* text, const size_t len, const size_t pos) {
  size_t p = pos;
  while (p < len && isWrapSeparator(text, len, p)) p += utf8CharLen(text, len, p);
  return p - pos;
}

// Byte length of the unit at `pos` that must not be broken, INCLUDING the separators
// that trail it (they belong to the line the unit ends, which is what keeps the next
// line from starting with a space). Always >= 1 for a `pos` that is not at the end and
// not at a line break, so a caller can never spin on a zero-length unit.
inline size_t wrapUnitLen(const char* text, const size_t len, const size_t pos, const WrapField field) {
  if (text == nullptr || pos >= len) return 0;
  if (isWrapLineBreak(text[pos])) return utf8CharLen(text, len, pos);
  if (field == WrapField::Plain) return utf8CharLen(text, len, pos);

  // Leading separators glue onto whatever precedes them; a unit never starts with one.
  const size_t lead = wrapSeparatorRunLen(text, len, pos);
  if (lead > 0) return lead;

  if (field == WrapField::Tags) {
    size_t p = pos;
    while (p < len && !isWrapLineBreak(text[p]) && !isWrapSeparator(text, len, p)) {
      p += utf8CharLen(text, len, p);
    }
    return (p - pos) + wrapSeparatorRunLen(text, len, p);
  }

  // Category: the segment up to and including the next '-'. Without a dash the rest of
  // the segment is one unit, which the walker then hard-splits on a code-point boundary
  // rather than letting it overflow the column.
  size_t p = pos;
  while (p < len && !isWrapLineBreak(text[p]) && text[p] != '-') p += utf8CharLen(text, len, p);
  if (p < len && text[p] == '-') return (p + 1) - pos;
  return p - pos;
}

// Byte length of the wrapped line that starts at `start`.
//
// `measure(text, bytes)` returns the pixel width of that byte range; it is a template
// parameter so the device can pass a renderer call and a host test can pass arithmetic.
// Guarantees, in the order they matter:
//   - never returns a range that ends inside a code point;
//   - never returns 0 for a `start` inside the text, so a caller's walk always advances
//     (a unit wider than the whole line is hard-split, and a single glyph wider than the
//     line is emitted whole rather than looping forever);
//   - stops at '\n'/'\r' WITHOUT consuming them, exactly as the blurb walk always has,
//     so a blank line in the source is still a line of its own;
//   - returns the whole remainder for a non-positive maxWidth, matching the old
//     `maxWidth > 0 &&` guard instead of emitting one glyph per line.
template <typename Measure>
inline size_t wrapLineLen(const char* text, const size_t len, const size_t start, const int maxWidth,
                          const WrapField field, Measure measure) {
  if (text == nullptr || start >= len) return 0;
  if (text[start] == '\n') return 1;
  if (text[start] == '\r') return (start + 1 < len && text[start + 1] == '\n') ? 2 : 1;
  if (maxWidth <= 0) return len - start;

  size_t pos = start;
  size_t lineEnd = start;
  int width = 0;
  while (pos < len) {
    if (isWrapLineBreak(text[pos])) return pos - start;
    size_t unit = wrapUnitLen(text, len, pos, field);
    if (unit == 0) unit = utf8CharLen(text, len, pos);  // unreachable; never spin anyway
    if (unit == 0) break;
    const int w = measure(text + pos, unit);
    if (lineEnd > start && width + w > maxWidth) return lineEnd - start;
    if (lineEnd == start && w > maxWidth) {
      // One unit wider than the whole line: cut it on a code-point boundary instead of
      // overflowing the column. `p > start` is what makes this terminate -- the first
      // glyph is always taken, however wide it is.
      size_t p = pos;
      int acc = 0;
      while (p < pos + unit && !isWrapLineBreak(text[p])) {
        const size_t cl = utf8CharLen(text, len, p);
        if (cl == 0) break;
        const int cw = measure(text + p, cl);
        if (p > start && acc + cw > maxWidth) return p - start;
        acc += cw;
        p += cl;
      }
    }
    width += w;
    pos += unit;
    lineEnd = pos;
  }
  return lineEnd - start;
}

// Number of wrapped lines `text` needs, or 0 for empty input. `limit` bounds the walk
// for a caller that only needs to know "more than the budget": pass budget + 1 and a
// result of exactly that means the text was cut.
template <typename Measure>
inline int countWrappedLines(const char* text, const size_t len, const int maxWidth, const WrapField field,
                             Measure measure, const int limit = 0) {
  if (text == nullptr || len == 0 || maxWidth <= 0) return 0;
  int lines = 0;
  size_t pos = 0;
  while (pos < len) {
    if (limit > 0 && lines >= limit) break;
    const size_t n = wrapLineLen(text, len, pos, maxWidth, field, measure);
    if (n == 0) break;
    pos += n;
    ++lines;
  }
  return lines;
}

}  // namespace jjwxc
