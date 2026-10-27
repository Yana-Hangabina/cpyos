#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Per-chapter paragraph-comment totals, cached beside the chapter on the SD card
// so the reader can draw comment bubbles while offline.
//
// Path (built by JjwxcStore, which owns all JJWXC on-card paths):
//   /.crosspoint/jjwxc/<novelId>/comments/<chapterId>.bin
//
// Layout, little-endian, 12-byte header + fixed 6-byte entries. The header is
// deliberately the same shape as toc.bin/spine.bin (uint32 magic / uint16
// version / uint16 entry size / uint32 count) so a reader of one index already
// knows how to sanity-check this one.
//
//   offset  size  field
//   0       4     magic "JJC1" (0x31434A4A on the wire)
//   4       2     version (1)
//   6       2     entry size (6)
//   8       4     entry count
//   12      4*n   { uint32 paragraphId, uint16 total }
//
// Validation is strict on purpose. The reader draws a bubble for every entry
// whose paragraph it sees on the page; a wrong paragraphId or a wrong total is
// not a crash, it is silently misleading UI. A rejected file degrades to "no
// bubbles", which is the only safe answer.
//
// This header has no project dependencies beyond <cstdint>/<cstring>, so the
// host test compiles it directly (cpyos/src/JjwxcCommentSidecarTest.cpp). The
// device-side wrappers in JjwxcStore do the SD I/O and use these functions so
// the test exercises the exact bytes that reach the card.
namespace JjwxcCommentSidecar {

constexpr uint32_t kMagic = 0x31434A4AU;  // "JJC1" as little-endian bytes
constexpr uint16_t kVersion = 1;
constexpr uint16_t kEntrySize = 6;
constexpr size_t kHeaderSize = 12;
// A long chapter has a few hundred paragraphs; only commented ones are stored.
// 512 caps the file at 12 + 3,072 bytes and bounds every buffer in the reader.
constexpr size_t kMaxEntries = 512;

struct Count {
  uint32_t paragraphId = 0;
  // The server reports a 32-bit total, but a paragraph with >65535 comments does
  // not exist on this site; narrowing here keeps the entry at six bytes. Callers
  // converting from a wider source must clamp rather than rely on truncation.
  uint16_t total = 0;
};

namespace detail {

inline void putU16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void putU32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// memcpy rather than a wider load through a cast: the source may be byte-aligned
// (the C3's RISC-V core faults on unaligned multi-byte access).
inline uint16_t getU16(const uint8_t* p) {
  uint16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint32_t getU32(const uint8_t* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

}  // namespace detail

// Fills the 12-byte header. `count` must already be <= kMaxEntries; callers
// validate because this function cannot report failure.
inline void encodeHeader(const size_t count, uint8_t* out) {
  detail::putU32(out, kMagic);
  detail::putU16(out + 4, kVersion);
  detail::putU16(out + 6, kEntrySize);
  detail::putU32(out + 8, count <= kMaxEntries ? static_cast<uint32_t>(count) : static_cast<uint32_t>(kMaxEntries));
}

inline void encodeEntry(const Count& entry, uint8_t* out) {
  detail::putU32(out, entry.paragraphId);
  detail::putU16(out + 4, entry.total);
}

// Whole-image encoder, used by JjwxcStore and by the host test. Returns the
// bytes written, or 0 when the arguments cannot produce a valid image:
//   - counts == nullptr with count > 0
//   - count > kMaxEntries
//   - out == nullptr
//   - outCap < kHeaderSize + count * kEntrySize
inline size_t encode(const Count* counts, const size_t count, uint8_t* out, const size_t outCap) {
  if ((counts == nullptr && count != 0) || count > kMaxEntries || out == nullptr) return 0;
  const size_t need = kHeaderSize + count * kEntrySize;
  if (outCap < need) return 0;
  encodeHeader(count, out);
  for (size_t i = 0; i < count; ++i) encodeEntry(counts[i], out + kHeaderSize + i * kEntrySize);
  return need;
}

// Validates and decodes. Copies at most `maxOut` entries into `out` but always
// reports the file's full entry count in `entryCount`, so a caller with a small
// output buffer can still tell "the file is damaged" (false) from "the file has
// more rows than I asked for" (true).
//
// Rejects: short header, bad magic, unknown version, wrong entry size, more
// entries than kMaxEntries, and a length that does not hold every entry. A
// longer-than-needed buffer is accepted for forward compatibility; the trailing
// bytes are ignored.
inline bool decode(const uint8_t* data, const size_t len, Count* out, const size_t maxOut, size_t& entryCount) {
  entryCount = 0;
  if (data == nullptr || len < kHeaderSize) return false;
  if (detail::getU32(data) != kMagic) return false;
  if (detail::getU16(data + 4) != kVersion) return false;
  if (detail::getU16(data + 6) != kEntrySize) return false;
  const uint32_t count = detail::getU32(data + 8);
  if (count > kMaxEntries) return false;
  if (len < kHeaderSize + static_cast<size_t>(count) * kEntrySize) return false;
  if (out == nullptr && maxOut != 0) return false;
  const size_t copy = count < maxOut ? count : maxOut;
  for (size_t i = 0; i < copy; ++i) {
    const uint8_t* p = data + kHeaderSize + i * kEntrySize;
    out[i].paragraphId = detail::getU32(p);
    out[i].total = detail::getU16(p + 4);
  }
  entryCount = count;
  return true;
}

// Comment bodies for one paragraph, cached so a second open of the same panel
// works with the radio off. Stored beside the count sidecar:
//   /.crosspoint/jjwxc/<novelId>/comments/<chapterId>-<paragraphId>.bin
//
// Layout, little-endian, 16-byte header + variable records:
//   offset  size  field
//   0       4     magic "JJD1" (0x31444A4A on the wire)
//   4       2     version (2)
//   6       2     header size (16)
//   8       4     server-reported total for this paragraph
//   12      2     record count
//   14      2     reserved (0)
//   v2 record: { uint16 authorLen, dateLen, ipLen, bodyLen, commentIdLen,
//                uint32 agreeNum, disagreeNum, replyTotal,
//                uint8 readerAgree, uint8 isAuthor,
//                char author[], date[], ip[], body[], commentId[] }
//
// Version 1 (16-byte header, 8-byte record header, no id/counts) is still
// decoded: those rows remain readable offline, just without the tap actions
// until a refresh rewrites them as v2. Encoding always writes v2.
//
// Strings are stored without their NUL and capped to the field arrays below.
// Every length is validated before it is used to advance, so a corrupt or
// hand-edited file can never walk past the buffer; a rejected cache degrades to
// "no offline comments" and the caller may fetch again.
namespace Bodies {

constexpr uint32_t kMagic = 0x31444A4AU;  // "JJD1" as little-endian bytes
constexpr uint16_t kVersion = 2;
constexpr uint16_t kLegacyVersion = 1;
constexpr uint16_t kHeaderSize = 16;
constexpr size_t kRecordHeaderSize = 24;
constexpr size_t kLegacyRecordHeaderSize = 8;
// One server page is 20; 200 matches the reader's cap and bounds the file at
// 16 + 200 * 672 = ~131 KB.
constexpr size_t kMaxEntries = 200;
constexpr size_t kAuthorCap = 48;
constexpr size_t kDateCap = 32;
constexpr size_t kIpCap = 32;
constexpr size_t kBodyCap = 512;
constexpr size_t kCommentIdCap = 16;

struct Item {
  char author[kAuthorCap] = {};
  char date[kDateCap] = {};
  char ip[kIpCap] = {};
  char body[kBodyCap] = {};
  char commentId[kCommentIdCap] = {};
  uint32_t agreeNum = 0;
  uint32_t disagreeNum = 0;
  uint32_t replyTotal = 0;
  uint8_t readerAgree = 0;  // 1 liked, 2 disliked, 0 none/unknown
  bool isAuthor = false;
};

constexpr size_t kMaxRecordSize =
    kRecordHeaderSize + kAuthorCap + kDateCap + kIpCap + kBodyCap + kCommentIdCap;
constexpr size_t kMaxEncodedBytes = kHeaderSize + kMaxEntries * kMaxRecordSize;

namespace detail {

inline size_t textLen(const char* text, const size_t cap) {
  if (text == nullptr) return 0;
  size_t n = 0;
  while (n < cap && text[n] != '\0') ++n;
  return n;
}

inline size_t recordSize(const Item& item) {
  return kRecordHeaderSize + textLen(item.author, kAuthorCap) + textLen(item.date, kDateCap) +
         textLen(item.ip, kIpCap) + textLen(item.body, kBodyCap) + textLen(item.commentId, kCommentIdCap);
}

inline uint16_t getU16(const uint8_t* p) {
  uint16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline void putU16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void putU32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline uint32_t getU32(const uint8_t* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline void copyOut(char* dst, const uint8_t* src, const size_t len) {
  std::memcpy(dst, src, len);
  dst[len] = '\0';
}

}  // namespace detail

// Exact serialized size for a valid list, or 0 when the arguments cannot be
// encoded (null with count > 0, too many entries, a text longer than its field).
inline size_t encodedSize(const Item* items, const size_t count) {
  if ((items == nullptr && count != 0) || count > kMaxEntries) return 0;
  size_t bytes = kHeaderSize;
  for (size_t i = 0; i < count; ++i) {
    // A field that fills its whole array has no terminating NUL, and decode
    // refuses such a length; refuse it here too so encode() never emits a file
    // its own reader would reject.
    if (detail::textLen(items[i].author, kAuthorCap) >= kAuthorCap ||
        detail::textLen(items[i].date, kDateCap) >= kDateCap ||
        detail::textLen(items[i].ip, kIpCap) >= kIpCap ||
        detail::textLen(items[i].body, kBodyCap) >= kBodyCap ||
        detail::textLen(items[i].commentId, kCommentIdCap) >= kCommentIdCap) {
      return 0;
    }
    bytes += detail::recordSize(items[i]);
  }
  return bytes;
}

inline size_t encode(const Item* items, const size_t count, const uint32_t total, uint8_t* out, const size_t outCap) {
  const size_t need = encodedSize(items, count);
  if (need == 0 || out == nullptr || outCap < need) return 0;
  detail::putU32(out, kMagic);
  detail::putU16(out + 4, kVersion);
  detail::putU16(out + 6, kHeaderSize);
  detail::putU32(out + 8, total);
  detail::putU16(out + 12, static_cast<uint16_t>(count));
  detail::putU16(out + 14, 0);

  size_t pos = kHeaderSize;
  for (size_t i = 0; i < count; ++i) {
    const Item& item = items[i];
    const size_t authorLen = detail::textLen(item.author, kAuthorCap);
    const size_t dateLen = detail::textLen(item.date, kDateCap);
    const size_t ipLen = detail::textLen(item.ip, kIpCap);
    const size_t bodyLen = detail::textLen(item.body, kBodyCap);
    const size_t commentIdLen = detail::textLen(item.commentId, kCommentIdCap);
    detail::putU16(out + pos, static_cast<uint16_t>(authorLen));
    detail::putU16(out + pos + 2, static_cast<uint16_t>(dateLen));
    detail::putU16(out + pos + 4, static_cast<uint16_t>(ipLen));
    detail::putU16(out + pos + 6, static_cast<uint16_t>(bodyLen));
    detail::putU16(out + pos + 8, static_cast<uint16_t>(commentIdLen));
    detail::putU32(out + pos + 10, item.agreeNum);
    detail::putU32(out + pos + 14, item.disagreeNum);
    detail::putU32(out + pos + 18, item.replyTotal);
    out[pos + 22] = item.readerAgree;
    out[pos + 23] = item.isAuthor ? 1 : 0;
    pos += kRecordHeaderSize;
    std::memcpy(out + pos, item.author, authorLen);
    pos += authorLen;
    std::memcpy(out + pos, item.date, dateLen);
    pos += dateLen;
    std::memcpy(out + pos, item.ip, ipLen);
    pos += ipLen;
    std::memcpy(out + pos, item.body, bodyLen);
    pos += bodyLen;
    std::memcpy(out + pos, item.commentId, commentIdLen);
    pos += commentIdLen;
  }
  return pos;
}

// Validates and decodes at most `maxOut` records. `recordCount` always reports
// the file's count, so "damaged" (false) stays distinguishable from "larger than
// my buffer" (true). Empty strings are allowed and come back as "".
inline bool decode(const uint8_t* data, const size_t len, Item* out, const size_t maxOut, size_t& recordCount,
                   uint32_t& total) {
  recordCount = 0;
  total = 0;
  if (data == nullptr || len < kHeaderSize) return false;
  if (detail::getU32(data) != kMagic) return false;
  const uint16_t version = detail::getU16(data + 4);
  if (version != kVersion && version != kLegacyVersion) return false;
  if (detail::getU16(data + 6) != kHeaderSize) return false;
  if (detail::getU16(data + 14) != 0) return false;
  const uint32_t count = detail::getU16(data + 12);
  if (count > kMaxEntries) return false;
  if (out == nullptr && maxOut != 0) return false;
  total = detail::getU32(data + 8);

  size_t pos = kHeaderSize;
  const size_t copy = count < maxOut ? count : maxOut;
  for (size_t i = 0; i < count; ++i) {
    const size_t header = version == kLegacyVersion ? kLegacyRecordHeaderSize : kRecordHeaderSize;
    if (pos + header > len) return false;
    const size_t authorLen = detail::getU16(data + pos);
    const size_t dateLen = detail::getU16(data + pos + 2);
    const size_t ipLen = detail::getU16(data + pos + 4);
    const size_t bodyLen = detail::getU16(data + pos + 6);
    const size_t commentIdLen = version == kLegacyVersion ? 0 : detail::getU16(data + pos + 8);
    const uint32_t agreeNum = version == kLegacyVersion ? 0 : detail::getU32(data + pos + 10);
    const uint32_t disagreeNum = version == kLegacyVersion ? 0 : detail::getU32(data + pos + 14);
    const uint32_t replyTotal = version == kLegacyVersion ? 0 : detail::getU32(data + pos + 18);
    const uint8_t readerAgree = version == kLegacyVersion ? 0 : data[pos + 22];
    const bool isAuthor = version != kLegacyVersion && data[pos + 23] != 0;
    pos += header;
    if (authorLen >= kAuthorCap || dateLen >= kDateCap || ipLen >= kIpCap || bodyLen >= kBodyCap ||
        commentIdLen >= kCommentIdCap) {
      return false;
    }
    if (pos + authorLen + dateLen + ipLen + bodyLen + commentIdLen > len) return false;
    if (i < copy) {
      Item& item = out[i];
      detail::copyOut(item.author, data + pos, authorLen);
      detail::copyOut(item.date, data + pos + authorLen, dateLen);
      detail::copyOut(item.ip, data + pos + authorLen + dateLen, ipLen);
      detail::copyOut(item.body, data + pos + authorLen + dateLen + ipLen, bodyLen);
      if (commentIdLen > 0) {
        detail::copyOut(item.commentId, data + pos + authorLen + dateLen + ipLen + bodyLen, commentIdLen);
      } else {
        item.commentId[0] = '\0';
      }
      item.agreeNum = agreeNum;
      item.disagreeNum = disagreeNum;
      item.replyTotal = replyTotal;
      item.readerAgree = readerAgree;
      item.isAuthor = isAuthor;
    }
    pos += authorLen + dateLen + ipLen + bodyLen + commentIdLen;
  }
  // Trailing bytes are accepted for forward compatibility, but every record the
  // header promises must be present (checked above).
  recordCount = count;
  return true;
}

}  // namespace Bodies

}  // namespace JjwxcCommentSidecar
