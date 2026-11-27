#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <cstring>

// Allocation-free text fitting for surfaces that must survive a tight heap.
//
// GfxRenderer::truncatedText()/wrappedText() return std::string / std::vector and
// are the wrong tool for a card whose whole job is to report "not enough
// memory": under -fno-exceptions a failed `new` inside them aborts (PNC-04), so
// the out-of-memory card could kill the device instead of explaining it. These
// helpers measure through the same renderer (so the result matches drawText) and
// write into caller-owned buffers.
namespace textfit {

// Byte length of the UTF-8 sequence starting at `p` (1..4).
inline size_t sequenceLength(const char lead) {
  const auto byte = static_cast<unsigned char>(lead);
  if ((byte & 0xE0) == 0xC0) return 2;
  if ((byte & 0xF0) == 0xE0) return 3;
  if ((byte & 0xF8) == 0xF0) return 4;
  return 1;
}

// Copy at most cap-1 bytes of a single line, never splitting a sequence.
inline size_t copyLine(char* out, const size_t cap, const char* text, size_t length) {
  if (out == nullptr || cap == 0) return 0;
  if (length > cap - 1) {
    length = cap - 1;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
  }
  memcpy(out, text, length);
  out[length] = '\0';
  return length;
}

// One line, cut at `maxWidth` with a trailing "..." when it was cut.
inline size_t truncate(const GfxRenderer& renderer, const int fontId, const char* text, const int maxWidth,
                       const EpdFontFamily::Style style, char* out, const size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  out[0] = '\0';
  if (text == nullptr || maxWidth <= 0) return 0;
  char probe[256];
  size_t used = 0;
  const char* cursor = text;
  bool cut = false;
  while (*cursor != '\0' && *cursor != '\n') {
    const size_t n = sequenceLength(*cursor);
    if (used + n >= sizeof(probe)) {
      cut = true;
      break;
    }
    memcpy(probe + used, cursor, n);
    probe[used + n] = '\0';
    if (renderer.getTextWidth(fontId, probe, style) > maxWidth && used > 0) {
      cut = true;
      break;
    }
    used += n;
    cursor += n;
  }
  if (!cut) return copyLine(out, cap, text, used);

  // Walk back whole codepoints until "<prefix>..." fits; never split a sequence.
  while (used > 0) {
    char candidate[260];
    memcpy(candidate, text, used);
    candidate[used] = '\0';
    strncat(candidate, "...", sizeof(candidate) - used - 1);
    if (renderer.getTextWidth(fontId, candidate, style) <= maxWidth) break;
    size_t k = used;
    do {
      --k;
    } while (k > 0 && (static_cast<unsigned char>(text[k]) & 0xC0) == 0x80);
    used = k;
  }
  memcpy(out, text, used);
  const size_t room = cap > used + 1 ? cap - used - 1 : 0;
  if (room >= 3) {
    memcpy(out + used, "...", 3);
    out[used + 3] = '\0';
  } else {
    out[used] = '\0';
  }
  return strlen(out);
}

// Greedy wrap into fixed-stride lines. Returns the number of lines written.
inline uint8_t wrap(const GfxRenderer& renderer, const int fontId, const char* text, const int maxWidth,
                    const EpdFontFamily::Style style, char* out, const size_t stride, const uint8_t maxLines) {
  if (out == nullptr || stride == 0 || maxLines == 0) return 0;
  if (text == nullptr || maxWidth <= 0) return 0;
  uint8_t lines = 0;
  const char* cursor = text;
  while (*cursor != '\0' && lines < maxLines) {
    char* line = out + static_cast<size_t>(lines) * stride;
    char probe[256];
    size_t used = 0;
    if (*cursor == '\n') {
      ++cursor;
      line[0] = '\0';
      ++lines;
      continue;
    }
    while (*cursor != '\0' && *cursor != '\n') {
      const size_t n = sequenceLength(*cursor);
      if (used + n >= sizeof(probe)) break;
      memcpy(probe + used, cursor, n);
      probe[used + n] = '\0';
      if (renderer.getTextWidth(fontId, probe, style) > maxWidth && used > 0) break;
      used += n;
      cursor += n;
    }
    copyLine(line, stride, cursor - used, used);
    ++lines;
    // Skip the space that would otherwise start the next line.
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\n') ++cursor;
  }
  return lines;
}

}  // namespace textfit
