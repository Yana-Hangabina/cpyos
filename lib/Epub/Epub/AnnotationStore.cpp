#include "AnnotationStore.h"

#include <algorithm>
#include <cstring>

namespace annotations {
namespace {

void writeU16(std::vector<uint8_t>& out, const uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void writeU32(std::vector<uint8_t>& out, const uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}

void writeString(std::vector<uint8_t>& out, const std::string& text, const size_t cap) {
  const std::string clamped = clampUtf8(text, cap);
  writeU16(out, static_cast<uint16_t>(clamped.size()));
  out.insert(out.end(), clamped.begin(), clamped.end());
}

bool readU16(const uint8_t* data, const size_t length, size_t& pos, uint16_t& value) {
  if (pos + 2 > length) return false;
  value = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
  pos += 2;
  return true;
}

bool readU32(const uint8_t* data, const size_t length, size_t& pos, uint32_t& value) {
  if (pos + 4 > length) return false;
  value = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
          (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
  pos += 4;
  return true;
}

bool readString(const uint8_t* data, const size_t length, size_t& pos, const size_t cap, std::string& out) {
  uint16_t size = 0;
  if (!readU16(data, length, pos, size)) return false;
  if (size > cap) return false;  // never trust a length that could not have been written
  if (pos + size > length) return false;
  out.assign(reinterpret_cast<const char*>(data + pos), size);
  pos += size;
  return true;
}

}  // namespace

std::string clampUtf8(const std::string& text, const size_t maxBytes) {
  if (text.size() <= maxBytes) return text;
  size_t cut = maxBytes;
  // Never end inside a multi-byte sequence: back off over continuation bytes.
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  return text.substr(0, cut);
}

bool normalize(std::vector<Annotation>& list) {
  if (list.size() > kMaxCount) return false;
  for (auto& a : list) {
    if (a.endOffset > a.startOffset) {
      a.quote = clampUtf8(a.quote, kMaxQuoteBytes);
      a.note = clampUtf8(a.note, kMaxNoteBytes);
      a.chapterTitle = clampUtf8(a.chapterTitle, kMaxTitleBytes);
    }
  }
  std::stable_sort(list.begin(), list.end(), [](const Annotation& a, const Annotation& b) {
    if (a.spineIndex != b.spineIndex) return a.spineIndex < b.spineIndex;
    return a.startOffset < b.startOffset;
  });
  // Same chapter + same range = one annotation; the most recently written entry
  // wins (so editing a note replaces it rather than leaving a stale duplicate).
  std::vector<Annotation> deduped;
  deduped.reserve(list.size());
  for (size_t i = 0; i < list.size(); ++i) {
    const bool lastOfGroup =
        (i + 1 == list.size()) || !(list[i + 1].spineIndex == list[i].spineIndex &&
                                   list[i + 1].startOffset == list[i].startOffset &&
                                   list[i + 1].endOffset == list[i].endOffset);
    if (lastOfGroup) deduped.push_back(std::move(list[i]));
  }
  list = std::move(deduped);
  return true;
}

std::vector<uint8_t> serialize(const std::vector<Annotation>& list) {
  std::vector<uint8_t> out;
  std::vector<Annotation> copy = list;
  if (!normalize(copy)) copy.resize(kMaxCount);
  out.reserve(12 + copy.size() * 32);
  writeU32(out, kFormatMagic);
  writeU16(out, kFormatVersion);
  writeU16(out, static_cast<uint16_t>(copy.size()));
  for (const auto& a : copy) {
    writeU16(out, a.spineIndex);
    writeU32(out, a.startOffset);
    writeU32(out, a.endOffset);
    out.push_back(a.kind);
    writeString(out, a.quote, kMaxQuoteBytes);
    writeString(out, a.note, kMaxNoteBytes);
    writeString(out, a.chapterTitle, kMaxTitleBytes);
  }
  return out;
}

bool deserialize(const uint8_t* data, const size_t length, std::vector<Annotation>& out) {
  out.clear();
  if (!data || length < 8) return false;
  size_t pos = 0;
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t count = 0;
  if (!readU32(data, length, pos, magic) || magic != kFormatMagic) return false;
  if (!readU16(data, length, pos, version) || version != kFormatVersion) return false;
  if (!readU16(data, length, pos, count)) return false;
  if (count > kMaxCount) return false;
  std::vector<Annotation> parsed;
  parsed.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Annotation a;
    if (!readU16(data, length, pos, a.spineIndex) || !readU32(data, length, pos, a.startOffset) ||
        !readU32(data, length, pos, a.endOffset)) {
      return false;
    }
    if (pos >= length) return false;
    a.kind = data[pos++];
    if (a.kind > static_cast<uint8_t>(Kind::Note)) return false;
    if (!readString(data, length, pos, kMaxQuoteBytes, a.quote) ||
        !readString(data, length, pos, kMaxNoteBytes, a.note) ||
        !readString(data, length, pos, kMaxTitleBytes, a.chapterTitle)) {
      return false;
    }
    if (a.endOffset <= a.startOffset) return false;
    parsed.push_back(std::move(a));
  }
  // Trailing bytes mean the file is not what its header claims.
  if (pos != length) return false;
  out = std::move(parsed);
  return true;
}

std::string formatExport(const std::string& bookTitle, const std::string& generatedOn,
                         const std::vector<Annotation>& list) {
  std::string out;
  out += "《" + bookTitle + "》";
  out += " 批注导出 " + generatedOn + "\n";
  std::vector<Annotation> sorted = list;
  normalize(sorted);
  std::string currentTitle;
  for (const auto& a : sorted) {
    const std::string title = a.chapterTitle.empty() ? (std::string("位置 ") + std::to_string(a.spineIndex + 1))
                                                     : a.chapterTitle;
    if (title != currentTitle) {
      currentTitle = title;
      out += "\n── " + title + "\n";
    }
    out += a.isNote() ? "  · 划线：" : "  · 高亮：";
    out += a.quote.empty() ? std::string("（未记录原文）") : a.quote;
    out += "\n";
    if (a.isNote() && !a.note.empty()) {
      out += "    批注：";
      out += a.note;
      out += "\n";
    }
  }
  if (sorted.empty()) out += "\n（暂无批注）\n";
  return out;
}

}  // namespace annotations
