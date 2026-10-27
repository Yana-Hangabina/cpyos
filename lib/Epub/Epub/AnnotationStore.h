#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Reader annotations (highlights / underlined notes) for the EPUB reader.
//
// Positions are stored as (spineIndex, visibleTextOffset range) — the same
// coordinate the bookmark system already uses, so an annotation survives a font,
// size, spacing or writing-mode change. The selected text (`quote`) and the
// chapter title are captured at creation time so the export file and the merged
// list still read correctly if the book's markup is later repaginated.
//
// The codec is deliberately strict: a damaged file is rejected whole, never
// guessed at. A wrong annotation range would underline the wrong sentence, which
// is exactly the kind of silent corruption this project refuses to ship.
namespace annotations {

constexpr uint32_t kFormatMagic = 0x4F4E4E41;  // 'ANNO' little-endian
constexpr uint16_t kFormatVersion = 1;
constexpr size_t kMaxCount = 512;
constexpr size_t kMaxQuoteBytes = 120;
constexpr size_t kMaxNoteBytes = 400;
constexpr size_t kMaxTitleBytes = 120;

enum class Kind : uint8_t {
  Highlight = 0,  // underline only
  Note = 1,       // underline + a bubble that opens the note
};

struct Annotation {
  uint16_t spineIndex = 0;
  uint32_t startOffset = 0;
  uint32_t endOffset = 0;  // exclusive
  uint8_t kind = static_cast<uint8_t>(Kind::Highlight);
  std::string quote;         // selected text (may be truncated)
  std::string note;          // empty for Kind::Highlight
  std::string chapterTitle;  // captured at creation for list/export

  bool isNote() const { return kind == static_cast<uint8_t>(Kind::Note); }
};

// Truncates on a UTF-8 boundary so a capped field never ends mid-codepoint.
std::string clampUtf8(const std::string& text, size_t maxBytes);

// Sorted by (spineIndex, startOffset); duplicates (same spine+range) collapse to
// the last one written. Returns false only if the input exceeds kMaxCount.
bool normalize(std::vector<Annotation>& list);

std::vector<uint8_t> serialize(const std::vector<Annotation>& list);
bool deserialize(const uint8_t* data, size_t length, std::vector<Annotation>& out);

// True when `offset` falls inside the annotation's range.
inline bool contains(const Annotation& a, const uint32_t offset) {
  return offset >= a.startOffset && offset < a.endOffset;
}

// Overlap test against a word range [wordStart, wordEnd).
inline bool overlaps(const Annotation& a, const uint32_t wordStart, const uint32_t wordEnd) {
  return a.startOffset < wordEnd && wordStart < a.endOffset;
}

// "《book》 批注导出 YYYY-MM-DD" + one section per chapter title, newest ranges
// last. `generatedOn` is passed in so the caller owns the clock.
std::string formatExport(const std::string& bookTitle, const std::string& generatedOn,
                         const std::vector<Annotation>& list);

}  // namespace annotations
