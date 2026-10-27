#include "EmojiFont.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <TtfEpdFont.h>
#include <esp_partition.h>

#include <cstring>
#include <memory>

#include "EmojiPackFormat.h"

#if CROSSPOINT_VECTOR_FONTS && defined(BOARD_HAS_PSRAM) && defined(CROSSPOINT_I18N_PACK)

namespace emoji_font {
namespace {

constexpr const char* kTag = "EMJ";
constexpr int kEmojiFontId = 0x454D4F4A;  // 'EMOJ'
constexpr size_t kGlyphCacheBytes = 96 * 1024;

memory::ByteBuffer ttfBytes_;
size_t ttfSize_ = 0;
std::unique_ptr<TtfEpdFont> face_;
uint8_t pointSize_ = 0;
int fontId_ = 0;

uint32_t crc32(const uint8_t* data, const size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void unregister(GfxRenderer& renderer) {
  if (fontId_ != 0) {
    renderer.unregisterTtfFont(fontId_);
    renderer.removeFont(fontId_);
    fontId_ = 0;
  }
  renderer.setEmojiFont(0);
  face_.reset();
  pointSize_ = 0;
}

bool loadPack() {
  if (ttfBytes_) return true;
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "spiffs");
  if (part == nullptr) {
    LOG_ERR(kTag, "no spiffs partition; emoji fallback disabled");
    return false;
  }
  emoji_pack::Header header{};
  if (esp_partition_read(part, emoji_pack::kPartitionOffset, &header, sizeof(header)) != ESP_OK) {
    LOG_ERR(kTag, "emoji header read failed");
    return false;
  }
  if (memcmp(header.magic, emoji_pack::kMagic, sizeof(header.magic)) != 0 || header.version != emoji_pack::kVersion ||
      header.ttfSize < 512 || header.ttfSize > emoji_pack::kMaxBytes) {
    LOG_ERR(kTag, "emoji pack header invalid (size=%u)", static_cast<unsigned>(header.ttfSize));
    return false;
  }
  const size_t bytes = header.ttfSize;
  // Reserve room for the face's caches and the rest of a reader page.
  if (!memory::psramHasHeadroom(bytes + kGlyphCacheBytes + 128 * 1024, bytes, 64 * 1024)) {
    LOG_ERR(kTag, "not enough PSRAM for a %u-byte emoji face", static_cast<unsigned>(bytes));
    return false;
  }
  auto buffer = memory::makePsramByteBufferUninitializedNoThrow(bytes);
  if (!buffer) {
    LOG_ERR(kTag, "emoji TTF allocation failed (%u bytes)", static_cast<unsigned>(bytes));
    return false;
  }
  if (esp_partition_read(part, emoji_pack::kPartitionOffset + emoji_pack::kHeaderSize, buffer.get(), bytes) !=
      ESP_OK) {
    LOG_ERR(kTag, "emoji TTF read failed");
    return false;
  }
  if (crc32(buffer.get(), bytes) != header.crc32) {
    LOG_ERR(kTag, "emoji TTF CRC mismatch");
    return false;
  }
  ttfBytes_ = std::move(buffer);
  ttfSize_ = bytes;
  return true;
}

bool install(GfxRenderer& renderer, const uint8_t pointSize) {
  if (!ttfBytes_ && !loadPack()) return false;
  if (pointSize == 0 || pointSize == pointSize_) return fontId_ != 0;
  unregister(renderer);
  if (!memory::psramHasHeadroom(kGlyphCacheBytes + 128 * 1024, kGlyphCacheBytes, 32 * 1024)) {
    LOG_ERR(kTag, "no PSRAM headroom for the emoji glyph cache");
    return false;
  }
  auto face = makeUniqueNoThrow<TtfEpdFont>();
  if (!face) {
    LOG_ERR(kTag, "emoji face allocation failed");
    return false;
  }
  face->addResidentSource(TtfEpdFont::Regular, ttfBytes_.get(), static_cast<uint32_t>(ttfSize_));
  // Match the reader's grayscale depth: the 16-level path reads 4-bit coverage,
  // the B/W path ignores the extra bits. One face serves both, as the embedded
  // EPUB font does.
  const bool fourBit = renderer.getGrayscaleLevels() == 16;
  if (!face->load(pointSize, /*twoBit=*/true, kGlyphCacheBytes, /*maxGlyphs=*/512, fourBit)) {
    LOG_ERR(kTag, "emoji face failed to load at %upt", static_cast<unsigned>(pointSize));
    return false;
  }
  renderer.insertFont(kEmojiFontId, face->family());
  renderer.registerTtfFont(kEmojiFontId, face.get());
  renderer.setEmojiFont(kEmojiFontId);
  face_ = std::move(face);
  fontId_ = kEmojiFontId;
  pointSize_ = pointSize;
  LOG_INF(kTag, "emoji fallback ready: %u bytes at %upt id=%d", static_cast<unsigned>(ttfSize_),
          static_cast<unsigned>(pointSize), kEmojiFontId);
  return true;
}

}  // namespace

bool begin(GfxRenderer& renderer, const uint8_t pointSize) {
  return install(renderer, pointSize);
}
void setPointSize(GfxRenderer& renderer, const uint8_t pointSize) { (void)install(renderer, pointSize); }
uint8_t pointSize() { return pointSize_; }
int fontId() { return fontId_; }

}  // namespace emoji_font

#else

namespace emoji_font {
bool begin(GfxRenderer&, uint8_t) { return false; }
void setPointSize(GfxRenderer&, uint8_t) {}
uint8_t pointSize() { return 0; }
int fontId() { return 0; }
}  // namespace emoji_font

#endif
