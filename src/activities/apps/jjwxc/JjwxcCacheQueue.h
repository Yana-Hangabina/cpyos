#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Persistent queue for JJWXC comment-body caching.
//
// Path (built by JjwxcStore):
//   /.crosspoint/jjwxc/cache_queue.bin
//
// One item covers a book/range, not one chapter: a 187-chapter book would
// otherwise fill the queue with hundreds of rows. The worker walks the item's
// chapters and paragraphs, and the files on the card (toc.bin / JJC1 / JJD1)
// are the source of truth for what is already done. The counters here are for
// display and are updated by the app when it persists progress.
//
// The format is deliberately fixed-size and platform-neutral (little-endian
// integers, no padding in the wire image) so a host test can pin it. A corrupt
// queue must never crash the app: decode rejects the whole image.
namespace JjwxcCacheQueue {

constexpr uint32_t kMagic = 0x3151584AU;  // "JXQ1" little-endian
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 12;
constexpr size_t kMaxItems = 6;
constexpr size_t kNovelIdCap = 16;
constexpr size_t kTitleCap = 96;
constexpr size_t kErrorCap = 48;
// 112 (id+title) + 12 (from/to/anchor) + 4 (state/flags/reserved)
// + 20 (five counters) + 8 (retry/updated) + 48 (error).
constexpr size_t kRecordSize = 204;
constexpr size_t kTrailerSize = 4;
constexpr size_t kMaxImageSize = kHeaderSize + kMaxItems * kRecordSize + kTrailerSize;

enum class State : uint8_t {
  Queued = 0,
  Running = 1,
  Paused = 2,
  NeedsWifi = 3,
  NeedsLogin = 4,
  Done = 5,
  Error = 6,
};

constexpr uint8_t kFlagAnchorValid = 0x01;

struct Item {
  char novelId[kNovelIdCap] = {};
  char title[kTitleCap] = {};
  uint32_t fromOrdinal = 0;
  uint32_t toOrdinal = 0;
  uint32_t anchorOrdinal = 0;
  uint8_t state = static_cast<uint8_t>(State::Queued);
  uint8_t flags = 0;
  uint16_t reserved = 0;
  uint32_t doneChapters = 0;
  uint32_t totalChapters = 0;
  uint32_t doneParagraphs = 0;
  uint32_t totalParagraphs = 0;
  uint32_t requests = 0;
  uint32_t retryAtEpoch = 0;
  uint32_t updatedEpoch = 0;
  char lastError[kErrorCap] = {};

  bool anchorValid() const { return (flags & kFlagAnchorValid) != 0; }
  void setAnchorValid(const bool valid) {
    flags = valid ? static_cast<uint8_t>(flags | kFlagAnchorValid)
                  : static_cast<uint8_t>(flags & ~kFlagAnchorValid);
  }
  State stateValue() const { return static_cast<State>(state); }
  bool terminal() const { return stateValue() == State::Done; }
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
inline uint32_t crc32(const uint8_t* data, const size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
inline void copyText(char* dst, const size_t cap, const char* src) {
  if (dst == nullptr || cap == 0) return;
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}
}  // namespace detail

inline size_t encodedSize(const size_t count) {
  if (count > kMaxItems) return 0;
  return kHeaderSize + count * kRecordSize + kTrailerSize;
}

inline size_t encode(const Item* items, const size_t count, uint8_t* out, const size_t cap) {
  const size_t need = encodedSize(count);
  if (items == nullptr && count != 0) return 0;
  if (out == nullptr || cap < need || need == 0) return 0;
  detail::putU32(out, kMagic);
  detail::putU16(out + 4, kVersion);
  detail::putU16(out + 6, static_cast<uint16_t>(kRecordSize));
  detail::putU32(out + 8, static_cast<uint32_t>(count));
  uint8_t* p = out + kHeaderSize;
  for (size_t i = 0; i < count; ++i) {
    const Item& it = items[i];
    detail::copyText(reinterpret_cast<char*>(p), kNovelIdCap, it.novelId);
    detail::copyText(reinterpret_cast<char*>(p + kNovelIdCap), kTitleCap, it.title);
    size_t off = kNovelIdCap + kTitleCap;
    detail::putU32(p + off, it.fromOrdinal);
    off += 4;
    detail::putU32(p + off, it.toOrdinal);
    off += 4;
    detail::putU32(p + off, it.anchorOrdinal);
    off += 4;
    p[off++] = it.state;
    p[off++] = it.flags;
    detail::putU16(p + off, it.reserved);
    off += 2;
    detail::putU32(p + off, it.doneChapters);
    off += 4;
    detail::putU32(p + off, it.totalChapters);
    off += 4;
    detail::putU32(p + off, it.doneParagraphs);
    off += 4;
    detail::putU32(p + off, it.totalParagraphs);
    off += 4;
    detail::putU32(p + off, it.requests);
    off += 4;
    detail::putU32(p + off, it.retryAtEpoch);
    off += 4;
    detail::putU32(p + off, it.updatedEpoch);
    off += 4;
    detail::copyText(reinterpret_cast<char*>(p + off), kErrorCap, it.lastError);
    p += kRecordSize;
  }
  const uint32_t crc = detail::crc32(out, kHeaderSize + count * kRecordSize);
  detail::putU32(p, crc);
  return need;
}

// Copies at most maxOut items but reports the file's full count, so a caller
// with a small buffer can distinguish "damaged" from "more rows than asked".
inline bool decode(const uint8_t* data, const size_t len, Item* out, const size_t maxOut, size_t& count) {
  count = 0;
  if (data == nullptr || len < kHeaderSize + kTrailerSize) return false;
  if (detail::getU32(data) != kMagic || detail::getU16(data + 4) != kVersion ||
      detail::getU16(data + 6) != kRecordSize) {
    return false;
  }
  const uint32_t n = detail::getU32(data + 8);
  if (n > kMaxItems) return false;
  const size_t need = kHeaderSize + static_cast<size_t>(n) * kRecordSize + kTrailerSize;
  if (len != need) return false;
  const uint32_t wantCrc = detail::getU32(data + need - kTrailerSize);
  if (detail::crc32(data, need - kTrailerSize) != wantCrc) return false;
  const uint8_t* p = data + kHeaderSize;
  for (uint32_t i = 0; i < n; ++i) {
    if (out != nullptr && i < maxOut) {
      Item& it = out[i];
      memset(&it, 0, sizeof(it));
      detail::copyText(it.novelId, kNovelIdCap, reinterpret_cast<const char*>(p));
      detail::copyText(it.title, kTitleCap, reinterpret_cast<const char*>(p + kNovelIdCap));
      size_t off = kNovelIdCap + kTitleCap;
      it.fromOrdinal = detail::getU32(p + off);
      off += 4;
      it.toOrdinal = detail::getU32(p + off);
      off += 4;
      it.anchorOrdinal = detail::getU32(p + off);
      off += 4;
      it.state = p[off++];
      it.flags = p[off++];
      it.reserved = detail::getU16(p + off);
      off += 2;
      it.doneChapters = detail::getU32(p + off);
      off += 4;
      it.totalChapters = detail::getU32(p + off);
      off += 4;
      it.doneParagraphs = detail::getU32(p + off);
      off += 4;
      it.totalParagraphs = detail::getU32(p + off);
      off += 4;
      it.requests = detail::getU32(p + off);
      off += 4;
      it.retryAtEpoch = detail::getU32(p + off);
      off += 4;
      it.updatedEpoch = detail::getU32(p + off);
      off += 4;
      detail::copyText(it.lastError, kErrorCap, reinterpret_cast<const char*>(p + off));
    }
    p += kRecordSize;
  }
  count = n;
  return true;
}

// The worker visits chapters in a wrap-around order: the chapter after the
// reader's current one first, through the end, then from the range start up to
// and including the current one. `step` is 0..len-1.
inline uint32_t ordinalForStep(const uint32_t fromIn, const uint32_t toIn, const uint32_t anchor,
                               const bool anchorValid, const uint32_t step) {
  uint32_t from = fromIn;
  uint32_t to = toIn;
  if (from > to) {
    const uint32_t t = from;
    from = to;
    to = t;
  }
  const uint32_t len = to - from + 1;
  if (len == 0) return from;
  if (!anchorValid || anchor < from || anchor > to) return from + (step % len);
  return from + ((anchor - from + 1 + (step % len)) % len);
}

}  // namespace JjwxcCacheQueue
