#include "UiScriptFont.h"

#include <VectorFontSupport.h>

#if defined(CROSSPOINT_SYSTEM_FONT_PACK) && defined(CROSSPOINT_FONT_PACK_EMBEDDED) && CROSSPOINT_VECTOR_FONTS

#include <GfxRenderer.h>
#include <Logging.h>
#include <MinizConfig.h>
#include <SdCardFontSystem.h>
#include <TtfEpdFont.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <vector>

#include "fontIds.h"

namespace {

constexpr char kMagic[4] = {'C', 'P', 'F', 'Z'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr size_t kMaxRawBytes = 512 * 1024;
// 426 non-CJK UI codepoints. 384 glyphs is enough for several screens without
// spending a CJK-sized arena on a face that only draws short labels.
constexpr size_t kCacheBytes = 16 * 1024;
constexpr uint16_t kMaxGlyphs = 384;

struct EmbeddedHeader {
  char magic[4];
  uint16_t version;
  uint16_t headerSize;
  uint32_t rawSize;
  uint32_t rawCrc;
  uint32_t compressedSize;
  uint8_t reserved[12];
};
static_assert(sizeof(EmbeddedHeader) == kHeaderSize, "UI script carrier header layout changed");

extern const uint8_t kCarrierStart[] asm("_binary_assets_fonts_cpyos_ui_ext_def_start");
extern const uint8_t kCarrierEnd[] asm("_binary_assets_fonts_cpyos_ui_ext_def_end");

uint8_t* g_buffer = nullptr;
size_t g_size = 0;
bool g_ready = false;
bool g_tried = false;

struct Registered {
  uint8_t pointSize = 0;
  std::unique_ptr<TtfEpdFont> font;
  int id = 0;
};
std::vector<Registered> g_fonts;

constexpr int scriptFontId(const uint8_t pointSize) { return 0x55534500 | pointSize; }

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  crc = ~crc;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

int cjkIdForSize(const uint8_t pt) {
  switch (pt) {
    case 8:
      return CJK_UI_8_FONT_ID;
    case 10:
      return CJK_UI_10_FONT_ID;
    case 12:
      return CJK_UI_12_FONT_ID;
    case 14:
      return CJK_UI_14_FONT_ID;
    case 16:
      return CJK_UI_16_FONT_ID;
    case 18:
      return CJK_UI_18_FONT_ID;
    default:
      return 0;
  }
}

unsigned long readSource(void* ctx, const unsigned long offset, unsigned char* buffer, const unsigned long count) {
  const auto* source = static_cast<const uint8_t*>(ctx);
  if (source == nullptr || offset >= g_size) return 0;
  const size_t available = g_size - offset;
  const size_t length = count < available ? count : available;
  std::memcpy(buffer, source + offset, length);
  return length;
}

void releaseSlot(GfxRenderer& renderer, Registered& slot) {
  if (slot.font) {
    renderer.unregisterTtfFont(slot.id);
    renderer.removeFont(slot.id);
  }
  slot.font.reset();
  slot.id = 0;
  slot.pointSize = 0;
}

void registerAt(GfxRenderer& renderer, const uint8_t pt) {
  auto font = std::unique_ptr<TtfEpdFont>(new (std::nothrow) TtfEpdFont());
  if (!font) {
    LOG_ERR("USF", "OOM: TtfEpdFont @%upt", static_cast<unsigned>(pt));
    return;
  }
  font->addStreamSource(TtfEpdFont::Regular, readSource, g_buffer, g_size);
  if (!font->load(pt, /*twoBit=*/true, kCacheBytes, kMaxGlyphs, /*fourBit=*/true)) {
    LOG_ERR("USF", "vector face load failed @%upt", static_cast<unsigned>(pt));
    return;
  }
  Registered slot;
  slot.pointSize = pt;
  slot.id = scriptFontId(pt);
  // main.cpp / the port registers built-in ids before this runs; drop any
  // placeholder so the vector family is not silently ignored.
  renderer.removeFont(slot.id);
  renderer.insertFont(slot.id, font->family());
  renderer.registerTtfFont(slot.id, font.get());
  const int cjkId = cjkIdForSize(pt);
  if (cjkId != 0) renderer.addSupplementFont(cjkId, slot.id);
  if (pt >= 10) renderer.addSupplementFont(CJK_READER_SIZE_FONT_ID(pt), slot.id);
  slot.font = std::move(font);
  g_fonts.push_back(std::move(slot));
}

}  // namespace

namespace ui_script_font {

bool begin() {
  if (g_ready) return true;
  if (g_tried) return false;
  g_tried = true;
  const size_t blobSize = static_cast<size_t>(kCarrierEnd - kCarrierStart);
  EmbeddedHeader header{};
  if (blobSize < sizeof(header)) {
    LOG_ERR("USF", "carrier too small");
    return false;
  }
  std::memcpy(&header, kCarrierStart, sizeof(header));
  if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion ||
      header.headerSize != kHeaderSize || header.rawSize == 0 || header.rawSize > kMaxRawBytes ||
      header.compressedSize == 0 || header.compressedSize > blobSize - kHeaderSize) {
    LOG_ERR("USF", "carrier header invalid");
    return false;
  }
  auto* raw = static_cast<uint8_t*>(heap_caps_malloc(header.rawSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (raw == nullptr) {
    LOG_ERR("USF", "carrier PSRAM allocation failed (%u)", static_cast<unsigned>(header.rawSize));
    return false;
  }
  if (tinfl_decompress_mem_to_mem(raw, header.rawSize, kCarrierStart + kHeaderSize, header.compressedSize, 0) !=
      static_cast<int>(header.rawSize)) {
    LOG_ERR("USF", "carrier inflate failed");
    heap_caps_free(raw);
    return false;
  }
  if (crc32Update(0, raw, header.rawSize) != header.rawCrc) {
    LOG_ERR("USF", "carrier CRC mismatch");
    heap_caps_free(raw);
    return false;
  }
  g_buffer = raw;
  g_size = header.rawSize;
  g_ready = true;
  LOG_INF("USF", "UI script supplement ready: %u bytes", static_cast<unsigned>(g_size));
  return true;
}

bool ready() { return g_ready; }

void configure(GfxRenderer& renderer, const uint8_t* pointSizes, const size_t count) {
  if (!g_ready || pointSizes == nullptr) return;
  bool wanted[35] = {};
  for (size_t i = 0; i < count; ++i) {
    const uint8_t pt = pointSizes[i];
    if (pt >= 8 && pt <= 34) wanted[pt] = true;
  }
  std::erase_if(g_fonts, [&](Registered& slot) {
    if (wanted[slot.pointSize]) return false;
    releaseSlot(renderer, slot);
    return true;
  });
  for (size_t i = 0; i < count; ++i) {
    const uint8_t pt = pointSizes[i];
    if (pt < 8 || pt > 34) continue;
    const bool loaded = std::any_of(g_fonts.begin(), g_fonts.end(),
                                    [pt](const Registered& slot) { return slot.pointSize == pt; });
    if (loaded) {
      // removeFont() clears supplements that pointed at this id.
      const int cjkId = cjkIdForSize(pt);
      if (cjkId != 0) renderer.addSupplementFont(cjkId, scriptFontId(pt));
      if (pt >= 10) renderer.addSupplementFont(CJK_READER_SIZE_FONT_ID(pt), scriptFontId(pt));
      continue;
    }
    registerAt(renderer, pt);
  }
}

void release(GfxRenderer& renderer) {
  for (auto& slot : g_fonts) releaseSlot(renderer, slot);
  g_fonts.clear();
  if (g_buffer != nullptr) {
    heap_caps_free(g_buffer);
    g_buffer = nullptr;
  }
  g_size = 0;
  g_ready = false;
  g_tried = false;
}

}  // namespace ui_script_font

#else

namespace ui_script_font {
bool begin() { return false; }
bool ready() { return false; }
void configure(GfxRenderer&, const uint8_t*, size_t) {}
void release(GfxRenderer&) {}
}  // namespace ui_script_font

#endif
