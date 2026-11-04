#include "TextPager.h"

#include <algorithm>
#include <cstdint>

namespace webbrowser {
namespace {

constexpr uint32_t kNoBreak = 0xFFFFFFFFu;

bool isContinuation(const unsigned char c) { return (c & 0xC0) == 0x80; }

int decodeUtf8(const char* s, const size_t len, uint32_t& cp) {
  const auto c = static_cast<unsigned char>(s[0]);
  if (c < 0x80) {
    cp = c;
    return 1;
  }
  if ((c & 0xE0) == 0xC0 && len >= 2 && isContinuation(static_cast<unsigned char>(s[1]))) {
    cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[1]) & 0x3Fu);
    return 2;
  }
  if ((c & 0xF0) == 0xE0 && len >= 3 && isContinuation(static_cast<unsigned char>(s[1])) &&
      isContinuation(static_cast<unsigned char>(s[2]))) {
    cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[1]) & 0x3Fu) << 6) |
         (static_cast<unsigned char>(s[2]) & 0x3Fu);
    return 3;
  }
  if ((c & 0xF8) == 0xF0 && len >= 4 && isContinuation(static_cast<unsigned char>(s[1])) &&
      isContinuation(static_cast<unsigned char>(s[2])) && isContinuation(static_cast<unsigned char>(s[3]))) {
    cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[1]) & 0x3Fu) << 12) |
         ((static_cast<unsigned char>(s[2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[3]) & 0x3Fu);
    return 4;
  }
  cp = 0xFFFD;
  return 1;
}

bool isCjk(const uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x3040 && cp <= 0x30FF) ||
         (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

bool noBreakBefore(const uint32_t cp) {
  switch (cp) {
    case ',':
    case '.':
    case '!':
    case '?':
    case ';':
    case ':':
    case ')':
    case ']':
    case '}':
    case 0xFF0C:
    case 0x3002:
    case 0xFF01:
    case 0xFF1F:
    case 0x3001:
    case 0xFF1B:
    case 0xFF1A:
    case 0xFF09:
    case 0x300D:
    case 0x300F:
    case 0x3011:
    case 0x300B:
    case 0x3009:
    case 0x2026:
    case 0x2014:
    case 0x201D:
    case 0x2019:
    case 0x00B7:
    case 0x30FB:
      return true;
    default:
      return false;
  }
}

bool noBreakAfter(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0xFF08:
    case 0x300C:
    case 0x300E:
    case 0x3010:
    case 0x300A:
    case 0x3008:
    case 0x201C:
    case 0x2018:
      return true;
    default:
      return false;
  }
}

bool canBreakBetween(const uint32_t previous, const uint32_t cp) {
  if (cp == ' ') return true;
  if (noBreakBefore(cp) || noBreakAfter(previous)) return false;
  if (isCjk(previous) || isCjk(cp)) return true;
  return previous == '-' || previous == '/';
}

struct LineRange {
  uint32_t start = 0;
  uint32_t length = 0;
  uint32_t blockIndex = 0;
  uint8_t flags = 0;
};

class Layout {
 public:
  Layout(const Document& document, const PagerSpec& spec, Pagination& out)
      : document_(document), spec_(spec), out_(out) {}

  void run() {
    if (spec_.measure == nullptr || spec_.lineHeight <= 0 || spec_.pageHeight <= 0 ||
        spec_.contentWidth <= 0) {
      return;
    }
    for (bool& valid : asciiValid_) valid = false;
    for (bool& valid : asciiBoldValid_) valid = false;

    collectRanges();
    if (spec_.verifyWholeLines) fixupRanges();
    layoutLines();
    buildPages();
  }

 private:
  int widthOf(const uint32_t cp, const char* bytes, const int length, const bool bold) {
    if (bold) return spec_.measure(spec_.measureContext, bytes, static_cast<size_t>(length), true);
    if (cp >= 0x20 && cp <= 0x7E) {
      const int index = static_cast<int>(cp) - 0x20;
      if (!asciiValid_[index]) {
        ascii_[index] = spec_.measure(spec_.measureContext, bytes, static_cast<size_t>(length), false);
        asciiValid_[index] = true;
      }
      return ascii_[index];
    }
    if (lastValid_ && cp == lastCodePoint_) return lastWidth_;
    lastWidth_ = spec_.measure(spec_.measureContext, bytes, static_cast<size_t>(length), false);
    lastCodePoint_ = cp;
    lastValid_ = true;
    return lastWidth_;
  }

  int wholeWidth(const uint32_t start, const uint32_t end, const bool bold) const {
    if (end <= start) return 0;
    return spec_.measure(spec_.measureContext, document_.text.data() + start, end - start, bold);
  }

  // Largest break opportunity strictly inside (start, end): the index of the
  // code point that would begin the next line.
  uint32_t lastBreakBefore(const uint32_t start, const uint32_t end) const {
    const char* text = document_.text.data();
    uint32_t previous = 0;
    uint32_t best = kNoBreak;
    uint32_t i = start;
    while (i < end) {
      uint32_t cp = 0;
      const int bytes = decodeUtf8(text + i, end - i, cp);
      if (i > start && previous != 0 && canBreakBetween(previous, cp)) best = i;
      previous = cp;
      i += static_cast<uint32_t>(bytes);
    }
    return best;
  }

  static uint32_t nextCodepointEnd(const char* text, const uint32_t start, const uint32_t end) {
    if (start >= end) return end;
    uint32_t cp = 0;
    const int bytes = decodeUtf8(text + start, end - start, cp);
    return std::min(end, start + static_cast<uint32_t>(bytes));
  }

  static uint32_t previousCodepointStart(const char* text, const uint32_t start, const uint32_t end) {
    if (end <= start) return start;
    uint32_t i = end - 1;
    while (i > start && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
    return i;
  }

  void collectRanges() {
    for (uint32_t blockIndex = 0; blockIndex < document_.blocks.size(); ++blockIndex) {
      const Block& block = document_.blocks[blockIndex];
      if (block.textLength == 0) continue;
      flags_ = 0;
      if (block.kind == BlockKind::Heading) flags_ |= LineHeading;
      if (block.kind == BlockKind::Pre) flags_ |= LinePre;
      if (block.kind == BlockKind::ListItem) flags_ |= LineList;
      if (block.kind == BlockKind::Quote) flags_ |= LineQuote;
      bold_ = (flags_ & LineHeading) != 0;
      wrapBlock(block, blockIndex);
      if (stop_) return;
    }
  }

  void emitRange(const uint32_t start, const uint32_t end, const uint32_t blockIndex) {
    emitTrimmed(start, end, blockIndex);
  }

  void emitTrimmed(const uint32_t start, uint32_t end, const uint32_t blockIndex) {
    while (end > start && document_.text.data()[end - 1] == ' ') --end;
    if (end <= start && (flags_ & LinePre) == 0) return;
    if (ranges_.size() >= static_cast<size_t>(spec_.maxLines)) {
      out_.truncated = true;
      stop_ = true;
      return;
    }
    LineRange range;
    range.start = start;
    range.length = end > start ? end - start : 0;
    range.blockIndex = blockIndex;
    range.flags = flags_;
    ranges_.push_back(range);
  }

  void wrapBlock(const Block& block, const uint32_t blockIndex) {
    const char* text = document_.text.data();
    const uint32_t end = block.textStart + block.textLength;
    const bool pre = (block.kind == BlockKind::Pre);
    uint32_t i = block.textStart;
    uint32_t lineStart = i;
    int width = 0;
    uint32_t previous = 0;
    uint32_t lastBreak = kNoBreak;

    while (i < end) {
      if (text[i] == '\n') {
        if (pre) emitTrimmed(lineStart, i, blockIndex);
        else emitRange(lineStart, i, blockIndex);
        lineStart = i + 1;
        i = lineStart;
        width = 0;
        previous = 0;
        lastBreak = kNoBreak;
        if (stop_) return;
        continue;
      }
      uint32_t cp = 0;
      const int bytes = decodeUtf8(text + i, end - i, cp);
      int cellWidth;
      if (cp == ' ' && spec_.spaceAdvance != nullptr) {
        uint32_t right = 0;
        uint32_t peek = i + static_cast<uint32_t>(bytes);
        while (peek < end && text[peek] == ' ') ++peek;
        if (peek < end) {
          uint32_t ignored = 0;
          decodeUtf8(text + peek, end - peek, ignored);
          right = ignored;
        }
        cellWidth = spec_.spaceAdvance(spec_.measureContext, previous, right, bold_);
      } else {
        cellWidth = widthOf(cp, text + i, bytes, bold_);
      }
      if (i > lineStart && previous != 0 && canBreakBetween(previous, cp)) lastBreak = i;
      if (width + cellWidth > spec_.contentWidth && i > lineStart) {
        const uint32_t breakAt = (lastBreak != kNoBreak && lastBreak > lineStart) ? lastBreak : i;
        emitRange(lineStart, breakAt, blockIndex);
        if (stop_) return;
        lineStart = breakAt;
        while (lineStart < end && text[lineStart] == ' ') ++lineStart;
        i = lineStart;
        width = 0;
        previous = 0;
        lastBreak = kNoBreak;
        continue;
      }
      width += cellWidth;
      previous = cp;
      i += static_cast<uint32_t>(bytes);
    }
    if (lineStart < end) {
      if (pre) emitTrimmed(lineStart, end, blockIndex);
      else emitRange(lineStart, end, blockIndex);
    }
  }

  // The per-glyph estimate can disagree with the width the renderer draws (a
  // fallback face, kerning, space advance). Verify every range with the same
  // whole-string call drawText uses and shrink it until it fits. This is a
  // second pass on purpose: measuring whole prefixes while wrapping is O(L^2)
  // per line, while only the few ranges that overflow need it here.
  void fixupRanges() {
    if (spec_.measure == nullptr) return;
    WebVector<LineRange> fixed;
    fixed.reserve(ranges_.size() + ranges_.size() / 4 + 8);
    const char* text = document_.text.data();

    for (size_t index = 0; index < ranges_.size(); ++index) {
      const LineRange range = ranges_[index];
      if (range.length == 0) {
        fixed.push_back(range);  // a <pre> blank line
        continue;
      }
      const bool bold = (range.flags & LineHeading) != 0;
      const uint32_t blockEnd = range.start + range.length;
      uint32_t pos = range.start;

      while (pos < blockEnd) {
        if (static_cast<int>(fixed.size()) >= spec_.maxLines) {
          out_.truncated = true;
          ranges_.swap(fixed);
          return;
        }
        uint32_t lineEnd = blockEnd;
        while (lineEnd > pos && wholeWidth(pos, lineEnd, bold) > spec_.contentWidth) {
          uint32_t breakAt = lastBreakBefore(pos, lineEnd);
          if (breakAt == kNoBreak || breakAt <= pos) {
            breakAt = previousCodepointStart(text, pos, lineEnd);
          }
          if (breakAt <= pos) break;
          lineEnd = breakAt;
        }
        if (lineEnd <= pos) {
          // No break opportunity at all: take one code point so this terminates.
          lineEnd = nextCodepointEnd(text, pos, blockEnd);
        }
        LineRange head = range;
        head.start = pos;
        head.length = lineEnd - pos;
        fixed.push_back(head);
        pos = lineEnd;
        while (pos < blockEnd && text[pos] == ' ') ++pos;
      }
    }
    ranges_.swap(fixed);
  }

  void layoutLines() {
    int32_t y = 0;
    uint32_t previousBlock = kNoBreak;
    for (size_t i = 0; i < ranges_.size(); ++i) {
      const LineRange& range = ranges_[i];
      if (range.blockIndex != previousBlock) {
        if (previousBlock != kNoBreak) {
          y += (range.flags & LineHeading) ? spec_.headingGap : spec_.blockGap;
        }
        previousBlock = range.blockIndex;
      }
      const int32_t page = y / spec_.pageHeight;
      if (y % spec_.pageHeight + spec_.lineHeight > spec_.pageHeight) {
        y = (page + 1) * spec_.pageHeight;
      }
      if (static_cast<int>(out_.lines.size()) >= spec_.maxLines) {
        out_.truncated = true;
        break;
      }
      if (y / spec_.pageHeight >= spec_.maxPages) {
        out_.truncated = true;
        break;
      }
      PagedLine line;
      line.textStart = range.start;
      line.textLength = range.length;
      line.y = y;
      line.blockIndex = range.blockIndex;
      line.flags = range.flags;
      out_.lines.push_back(line);
      y += spec_.lineHeight;
    }
  }

  void buildPages() {
    out_.pages.clear();
    for (uint32_t i = 0; i < out_.lines.size(); ++i) {
      const int32_t yOffset = (out_.lines[i].y / spec_.pageHeight) * spec_.pageHeight;
      if (yOffset / spec_.pageHeight >= spec_.maxPages) {
        out_.truncated = true;
        break;
      }
      if (out_.pages.empty() || out_.pages.back().yOffset != yOffset) {
        PagedPage page;
        page.firstLine = i;
        page.lineCount = 0;
        page.yOffset = yOffset;
        out_.pages.push_back(page);
      }
      ++out_.pages.back().lineCount;
    }
  }

  const Document& document_;
  const PagerSpec& spec_;
  Pagination& out_;
  WebVector<LineRange> ranges_;
  uint8_t flags_ = 0;
  bool bold_ = false;
  bool stop_ = false;
  int ascii_[95]{};
  bool asciiValid_[95]{};
  bool asciiBoldValid_[95]{};
  uint32_t lastCodePoint_ = 0;
  int lastWidth_ = 0;
  bool lastValid_ = false;
};

}  // namespace

bool paginate(const Document& document, const PagerSpec& spec, Pagination& out) {
  out.clear();
  Layout layout(document, spec, out);
  layout.run();
  return !out.lines.empty();
}

}  // namespace webbrowser
