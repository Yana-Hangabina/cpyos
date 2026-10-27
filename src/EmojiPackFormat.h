#pragma once

// On-flash container for the bundled emoji face (`EMJ1`), read by EmojiFont.
//
// It lives in the data/spiffs partition, after the language pack, and is written
// by exp/flash_device.sh from build/emoji.dat (scripts/generate_emoji_pack.py).
// Keeping it out of app0 means a new emoji set is a data-partition flash, not a
// firmware rebuild, and the loader still rejects a truncated or stale pack
// instead of handing FreeType arbitrary bytes.

#include <cstddef>
#include <cstdint>

namespace emoji_pack {

inline constexpr char kMagic[4] = {'E', 'M', 'J', '1'};
inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kHeaderSize = 32;
// Partition-relative offset. The language pack is the growing artifact and stays
// before this blob's own growth room; emoji is fixed-size and sits at the end.
inline constexpr uint32_t kPartitionOffset = 0x160000;
inline constexpr uint32_t kMaxBytes = 0x20000;  // 128 KiB, the tail of a 1.5 MiB spiffs

struct Header {
  char magic[4];
  uint8_t version;
  uint8_t reserved[3];
  uint32_t ttfSize;
  uint32_t crc32;
  uint8_t reserved2[16];
};

static_assert(sizeof(Header) == kHeaderSize, "emoji pack header must stay 32 bytes");

}  // namespace emoji_pack
