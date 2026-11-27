#include "SystemFontPack.h"

#include <VectorFontSupport.h>

#if defined(CROSSPOINT_SYSTEM_FONT_PACK) && CROSSPOINT_VECTOR_FONTS

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <StackSafety.h>
#include <TtfEpdFont.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
// Vendored miniz with CrossPoint's configuration: it renames the tinfl
// entry points away from the ESP32 mask-ROM ones and pulls the vendored
// header by relative path. Including <miniz.h> directly makes ESP-IDF resolve
// esp_rom's TINFL_LESS_MEMORY build instead, which has a different struct
// layout.
#include <MinizConfig.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <vector>

#include "ReaderFontSizes.h"
#include "SdCardFontSystem.h"
#include "fontIds.h"

namespace {

constexpr char kMagic[4] = {'C', 'P', 'F', 'P'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
// The reader can ask for any size in this set; UI roles only use 10..18. 18pt
// stays for the old UI display role and 20..40 are reader-only. The exact
// intermediate points cost nothing until a reader face is registered; the
// picker itself uses the shorter ladder in ReaderFontSizes.h.
constexpr uint8_t kPointSizes[] = {8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 40};
constexpr char kBuildMarker[] = "CROSSPOINT-SYSTEM-FONT-PACK-V1";
#if defined(CROSSPOINT_FONT_PACK_EMBEDDED)
constexpr char kBackendMarker[] = "CROSSPOINT-SYSTEM-FONT-PACK-EMBEDDED-V1";
#else
constexpr char kBackendMarker[] = "CROSSPOINT-SYSTEM-FONT-PACK-SPIFFS-V1";
#endif

struct PackHeader {
  char magic[4];
  uint16_t version;
  uint16_t headerSize;
  uint32_t payloadSize;
  uint32_t payloadCrc;
  uint8_t reserved[16];
};
static_assert(sizeof(PackHeader) == kHeaderSize, "system font pack header layout changed");

#if defined(CROSSPOINT_FONT_PACK_EMBEDDED)
// Raw-deflate carrier linked into app0. It is decompressed once into PSRAM at
// boot, so an OTA update ships the font with the app and no SD card or data
// partition step is required.
constexpr char kEmbeddedMagic[4] = {'C', 'P', 'F', 'Z'};
struct EmbeddedHeader {
  char magic[4];
  uint16_t version;
  uint16_t headerSize;
  uint32_t rawSize;
  uint32_t rawCrc;
  uint32_t compressedSize;
  uint8_t reserved[12];
};
static_assert(sizeof(EmbeddedHeader) == kHeaderSize, "embedded font pack header layout changed");
constexpr size_t kMaxRawBytes = 4u * 1024u * 1024u;
extern const uint8_t kEmbeddedFontPackStart[] asm("_binary_assets_fonts_cpyos_han_sans_def_start");
extern const uint8_t kEmbeddedFontPackEnd[] asm("_binary_assets_fonts_cpyos_han_sans_def_end");
uint8_t* g_packBuffer = nullptr;
#endif

enum class Backend : uint8_t { Partition, Memory, File };

struct Source {
  Backend backend = Backend::Partition;
  const esp_partition_t* partition = nullptr;
  const uint8_t* memory = nullptr;
  HalFile* file = nullptr;
  size_t dataOffset = 0;
  size_t payloadSize = 0;
};

struct RegisteredFont {
  std::vector<int> fontIds;
  std::unique_ptr<TtfEpdFont> font;
  uint8_t pointSize = 0;
  // True once the CJK_UI_*/NOTOSANS_* role ids for this size are registered.
  // A block-only face (layout M2) starts false and is upgraded if configure()
  // later wants the size for a UI role.
  bool roleAliases = false;
};

Source g_source;
std::vector<RegisteredFont> g_fonts;
bool g_ready = false;
bool g_fourBit = false;
// Point sizes the reader's block-font provider asked to keep beyond the current
// configure() set (layout milestone M2). Bounded by the caller; configure() ORs them
// into its wanted mask so a UI scale change cannot release a face a cached section
// still names.
bool g_pinnedPointSizes[35] = {};

// Preview faces are debug-only and deliberately live outside g_fonts, so a
// later configure()/release() can never confuse them with a reader role.
constexpr int kPreview2BitFontId = 0x46504332;  // "FPC2"
constexpr int kPreview4BitFontId = 0x46504334;  // "FPC4"
std::unique_ptr<TtfEpdFont> g_previewFont[2];
int g_previewId[2] = {0, 0};

bool usesFourBitFaces() {
#if defined(CROSSPOINT_FONT_PACK_EMBEDDED)
  return true;  // Read Pico: PSRAM-backed stream, 16-level panel
#else
  return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
#endif
}

size_t cacheBytesFor(const uint8_t pointSize, const bool fourBit) {
  if (!fourBit) return 8 * 1024;
  if (pointSize <= 12) return 128 * 1024;
  if (pointSize <= 16) return 256 * 1024;
  if (pointSize <= 18) return 384 * 1024;
  return 512 * 1024;
}

uint16_t maxGlyphsFor(const uint8_t pointSize, const bool fourBit) {
  if (!fourBit) return 96;
  if (pointSize <= 12) return 1024;
  if (pointSize <= 16) return 1536;
  return 2048;
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

unsigned long readSource(void* ctx, const unsigned long offset, unsigned char* buffer, const unsigned long count) {
  auto* source = static_cast<Source*>(ctx);
  if (source == nullptr) return 0;
  if (offset > source->payloadSize || count > source->payloadSize - offset) return 0;
  if (count == 0) return 0;  // FreeType uses a zero-length read as a seek probe
  if (source->backend == Backend::Memory) {
    if (source->memory == nullptr) return 0;
    std::memcpy(buffer, source->memory + offset, count);
    return count;
  }
  if (source->backend == Backend::File) {
    if (source->file == nullptr) return 0;
    // seek+read must be one transaction: the render task and page loader share
    // the SD card, and another caller moving the cursor between the two would
    // return bytes from the wrong offset.
    HalStorage::StorageLock lock;
    if (!source->file->seek(source->dataOffset + offset)) return 0;
    return source->file->read(buffer, count) == static_cast<int>(count) ? count : 0;
  }
  if (source->partition == nullptr) return 0;
  // FreeType calls this while rasterizing a glyph, i.e. potentially on the
  // render task, whose stack lives in PSRAM. A partition read there would take
  // the cache -- and the caller's own stack -- away, so the device would panic
  // instead of rendering. A failed read is the safe answer: FreeType reports a
  // broken face, and the UI keeps the bitmap fallback it already has.
  if (!memory::internalFlashAllowed(fault::Code::FontPackStreamRefused)) {
    // Once per boot: this is a hot path and the answer will not change.
    static bool warned = false;
    if (!warned) {
      warned = true;
      LOG_ERR("SFP", "font pack read refused: this task stack is in PSRAM");
    }
    return 0;
  }
  return esp_partition_read(source->partition, source->dataOffset + offset, buffer, count) == ESP_OK ? count : 0;
}

void addFaceId(GfxRenderer& renderer, RegisteredFont& entry, const int id) {
  // main.cpp registers every built-in id before this runs. insertFont() refuses
  // duplicates, so the real vector face would be silently ignored; drop the
  // placeholder first (removeFont also clears fallbacks that point at it —
  // typography::apply() rebinds them immediately afterwards).
  renderer.removeFont(id);
  renderer.insertFont(id, entry.font->family());
  renderer.registerTtfFont(id, entry.font.get());
  entry.fontIds.push_back(id);
}

// UI role ids (CJK_UI_*, NOTOSANS_*) this point size serves. Only configure()
// needs them. A block-only face must not touch these ids: they may already be
// bound as preferred fonts by typography::apply(), and removeFont() would drop
// the binding.
void addRoleAliases(GfxRenderer& renderer, RegisteredFont& entry) {
  const uint8_t pointSize = entry.pointSize;
  if (pointSize <= 16) {
    addFaceId(renderer, entry, pointSize == 8    ? CJK_UI_8_FONT_ID
                                   : pointSize == 10 ? CJK_UI_10_FONT_ID
                                   : pointSize == 12 ? CJK_UI_12_FONT_ID
                                   : pointSize == 14 ? CJK_UI_14_FONT_ID
                                                     : CJK_UI_16_FONT_ID);
  } else if (pointSize == 18) {
    addFaceId(renderer, entry, CJK_UI_18_FONT_ID);
  }
  // The reader path asks for NOTOSANS_12/14/16/18; a selected reader size must
  // keep its size. NOTOSERIF ids are aliased to the same single family because
  // a no-PSRAM target cannot afford a second multi-MB face; without this they
  // would fall back to the built-in 12pt bitmap and change size mid-book.
  switch (pointSize) {
    case 12:
      addFaceId(renderer, entry, NOTOSANS_12_FONT_ID);
      addFaceId(renderer, entry, NOTOSERIF_12_FONT_ID);
      break;
    case 14:
      addFaceId(renderer, entry, NOTOSANS_14_FONT_ID);
      addFaceId(renderer, entry, NOTOSERIF_14_FONT_ID);
      break;
    case 16:
      addFaceId(renderer, entry, NOTOSANS_16_FONT_ID);
      addFaceId(renderer, entry, NOTOSERIF_16_FONT_ID);
      break;
    case 18:
      addFaceId(renderer, entry, NOTOSANS_18_FONT_ID);
      addFaceId(renderer, entry, NOTOSERIF_18_FONT_ID);
      break;
    default:
      break;
  }
}

// `withRoleAliases` registers the UI role ids this size serves. configure() passes
// true; a block-only size passes false (see addRoleAliases).
void registerAt(GfxRenderer& renderer, const uint8_t pointSize, const bool withRoleAliases) {
  auto font = std::unique_ptr<TtfEpdFont>(new (std::nothrow) TtfEpdFont());
  if (!font) {
    LOG_ERR("SFP", "OOM: TtfEpdFont @%upt", static_cast<unsigned>(pointSize));
    return;
  }
  font->addStreamSource(TtfEpdFont::Regular, readSource, &g_source, g_source.payloadSize);
  // PSRAM targets load 4-bit coverage so the 16-level canvas can use all 16
  // levels. CJK pages need hundreds of glyphs; the old 8KB/96-glyph cache was
  // built for a UI page, not a book page, and flushed mid-page on every turn.
  const bool fourBit = usesFourBitFaces();
  const size_t cacheBytes = cacheBytesFor(pointSize, fourBit);
  const uint16_t maxGlyphs = maxGlyphsFor(pointSize, fourBit);
  if (!font->load(pointSize, /*twoBit=*/true, cacheBytes, maxGlyphs, fourBit)) {
    LOG_ERR("SFP", "vector font load failed @%upt", static_cast<unsigned>(pointSize));
    return;
  }

  RegisteredFont entry;
  entry.font = std::move(font);
  entry.pointSize = pointSize;
  // Reader faces get their own id for every size; UI roles below are only bound
  // for 10..18, the reader asks for 20..34 directly.
  if (pointSize >= 10) addFaceId(renderer, entry, CJK_READER_SIZE_FONT_ID(pointSize));
  if (withRoleAliases) {
    addRoleAliases(renderer, entry);
    entry.roleAliases = true;
  }
  g_fonts.push_back(std::move(entry));
}

}  // namespace

namespace {
// A pack whose content changes (font update) lands under a new CRC-named file;
// the previous copy is dead weight of ~2.5 MB. Keep exactly the file this boot
// uses and drop the rest, so repeated firmware updates cannot fill the card.
void pruneStalePackFiles(const char* keepPath) {
  if (!Storage.ready()) return;
  constexpr const char* kDir = "/.crosspoint";
  for (const String& name : Storage.listFiles(kDir, 200)) {
    if (!name.startsWith("system_font_") || !name.endsWith(".dat")) continue;
    const String full = String(kDir) + "/" + name;
    if (full == keepPath) continue;
    if (Storage.remove(full.c_str())) LOG_INF("SFP", "removed stale font pack %s", name.c_str());
  }
}
}  // namespace

namespace system_font_pack {

bool begin(GfxRenderer& renderer) {
  (void)renderer;  // faces are registered by configure()
  if (g_ready) return true;
  size_t payloadSize = 0;

#if defined(CROSSPOINT_FONT_PACK_EMBEDDED)
  const size_t blobSize =
      static_cast<size_t>(kEmbeddedFontPackEnd - kEmbeddedFontPackStart);
  EmbeddedHeader header{};
  if (blobSize < sizeof(header)) {
    LOG_ERR("SFP", "embedded font pack is too small");
    return false;
  }
  std::memcpy(&header, kEmbeddedFontPackStart, sizeof(header));
  if (std::memcmp(header.magic, kEmbeddedMagic, sizeof(kEmbeddedMagic)) != 0 ||
      header.version != kVersion || header.headerSize != kHeaderSize || header.rawSize == 0 ||
      header.rawSize > kMaxRawBytes || header.compressedSize == 0 ||
      header.compressedSize > blobSize - kHeaderSize) {
    LOG_ERR("SFP", "embedded font pack header invalid");
    return false;
  }
  // The UI face is a few hundred KB and belongs in PSRAM for the whole
  // session. Keeping it there (instead of provisioning an SD copy and faulting
  // every glyph through the card) is what guarantees the system UI is never
  // slowed down by a slow or missing SD card. It also removes the first-boot
  // multi-MB write older builds performed.
  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (freePsram < header.rawSize + 256u * 1024u) {
    LOG_ERR("SFP", "not enough PSRAM for the UI font pack (%u free, need %u)",
            static_cast<unsigned>(freePsram), static_cast<unsigned>(header.rawSize + 256u * 1024u));
    return false;
  }
  auto* raw = static_cast<uint8_t*>(heap_caps_malloc(header.rawSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (raw == nullptr) {
    LOG_ERR("SFP", "font pack PSRAM allocation failed (%u)", static_cast<unsigned>(header.rawSize));
    return false;
  }
  if (tinfl_decompress_mem_to_mem(raw, header.rawSize, kEmbeddedFontPackStart + kHeaderSize,
                                  header.compressedSize, 0) != static_cast<int>(header.rawSize)) {
    LOG_ERR("SFP", "font pack inflate failed");
    heap_caps_free(raw);
    return false;
  }
  // crc32Update is the incremental update (no leading/trailing complement);
  // this is the same convention the spiffs path uses below.
  const uint32_t packCrc = crc32Update(UINT32_MAX, raw, header.rawSize) ^ UINT32_MAX;
  if (packCrc != header.rawCrc) {
    LOG_ERR("SFP", "font pack CRC mismatch");
    heap_caps_free(raw);
    return false;
  }
  g_packBuffer = raw;
  g_source = Source{};
  g_source.backend = Backend::Memory;
  g_source.memory = raw;
  g_source.payloadSize = header.rawSize;
  payloadSize = header.rawSize;
#else
  // The boot-time validation below reads the whole pack from SPI flash. That is
  // only legal from a task whose stack lives in internal RAM; on the render task
  // (PSRAM stack) the cache disable would pull the caller's own stack away.
  // Refuse before the first read and say "refused", never "missing": the pack is
  // still there, and the next boot from the UI task installs it normally.
  if (!memory::internalFlashAllowed(fault::Code::FontPackLoadRefused)) {
    LOG_ERR("SFP", "font pack load refused: this task stack is in PSRAM");
    return false;
  }
  const esp_partition_t* partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (partition == nullptr) {
    LOG_ERR("SFP", "no data/spiffs partition for system font pack");
    return false;
  }

  PackHeader header{};
  if (esp_partition_read(partition, 0, &header, sizeof(header)) != ESP_OK ||
      std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion ||
      header.headerSize != kHeaderSize || header.payloadSize == 0 ||
      header.payloadSize > partition->size - kHeaderSize) {
    LOG_ERR("SFP", "system font pack missing or invalid at spiffs offset 0");
    return false;
  }

  // Verify the payload once at boot. The pack is a trusted, rarely-changing
  // resource; a bad flash should fail loudly instead of producing wrong glyphs.
  uint32_t crc = UINT32_MAX;
  uint8_t chunk[512];
  size_t offset = 0;
  while (offset < header.payloadSize) {
    const size_t length = std::min<size_t>(sizeof(chunk), header.payloadSize - offset);
    if (esp_partition_read(partition, kHeaderSize + offset, chunk, length) != ESP_OK) {
      LOG_ERR("SFP", "system font pack read failed at %u", static_cast<unsigned>(offset));
      return false;
    }
    crc = crc32Update(crc, chunk, length);
    offset += length;
  }
  crc ^= UINT32_MAX;
  if (crc != header.payloadCrc) {
    LOG_ERR("SFP", "system font pack CRC mismatch: %08x != %08x", static_cast<unsigned>(crc),
            static_cast<unsigned>(header.payloadCrc));
    return false;
  }

  g_source.backend = Backend::Partition;
  g_source.partition = partition;
  g_source.dataOffset = kHeaderSize;
  g_source.payloadSize = header.payloadSize;
  payloadSize = header.payloadSize;
#endif

  g_fonts.reserve(std::size(kPointSizes));
  g_fourBit = usesFourBitFaces();
  g_ready = true;
  LOG_INF("SFP", "%s %s ready: %u bytes; faces load on demand (%s coverage)", kBuildMarker, kBackendMarker,
          static_cast<unsigned>(payloadSize), g_fourBit ? "4-bit" : "2-bit");
  return g_ready;
}

void configure(GfxRenderer& renderer, const uint8_t* pointSizes, const size_t count) {
  if (!g_ready || pointSizes == nullptr) return;

  bool wanted[41] = {};
  for (size_t i = 0; i < count; ++i) {
    const uint8_t pt = pointSizes[i];
    if (pt >= 8 && pt <= READER_MAX_POINT_SIZE) wanted[pt] = true;
  }
  for (uint8_t pt = 10; pt <= 34; ++pt) {
    if (g_pinnedPointSizes[pt]) wanted[pt] = true;
  }

  // Release first so a scale change never holds old and new faces at once.
  std::erase_if(g_fonts, [&](const RegisteredFont& entry) {
    if (wanted[entry.pointSize]) return false;
    for (const int id : entry.fontIds) {
      renderer.unregisterTtfFont(id);
      renderer.removeFont(id);
    }
    return true;
  });
  for (const uint8_t pt : kPointSizes) {
    if (pt < 8 || !wanted[pt]) continue;
    auto it = std::find_if(g_fonts.begin(), g_fonts.end(),
                           [pt](const RegisteredFont& entry) { return entry.pointSize == pt; });
    if (it == g_fonts.end()) {
      registerAt(renderer, pt, /*withRoleAliases=*/true);
    } else if (!it->roleAliases) {
      // A block-only face (layout M2) was already loaded at this size; add the UI
      // role ids now that a role wants it.
      addRoleAliases(renderer, *it);
      it->roleAliases = true;
    }
  }
}

bool ensurePointSize(GfxRenderer& renderer, const uint8_t pointSize) {
  if (!g_ready || pointSize < 10 || pointSize > 34) return false;
  const bool loaded = std::any_of(g_fonts.begin(), g_fonts.end(),
                                  [pointSize](const RegisteredFont& entry) { return entry.pointSize == pointSize; });
  if (!loaded) registerAt(renderer, pointSize, /*withRoleAliases=*/false);
  const bool ok = std::any_of(g_fonts.begin(), g_fonts.end(),
                              [pointSize](const RegisteredFont& entry) { return entry.pointSize == pointSize; });
  // Pin only a face that actually exists; a failed load must not make configure()
  // keep asking for a size the pack could not build.
  if (ok) g_pinnedPointSizes[pointSize] = true;
  return ok;
}

void releasePinnedPointSizes() {
  for (uint8_t pt = 10; pt <= 34; ++pt) g_pinnedPointSizes[pt] = false;
}

void release(GfxRenderer& renderer) {
  releasePreviewFaces(renderer);
  for (const auto& entry : g_fonts) {
    for (const int id : entry.fontIds) {
      renderer.unregisterTtfFont(id);
      renderer.removeFont(id);
    }
  }
  g_fonts.clear();
  releasePinnedPointSizes();
#if defined(CROSSPOINT_FONT_PACK_EMBEDDED)
  if (g_packBuffer != nullptr) {
    heap_caps_free(g_packBuffer);
    g_packBuffer = nullptr;
  }
#endif
  g_source = {};
  g_ready = false;
}

bool ready() { return g_ready; }

bool faceIsFourBit() { return g_fourBit; }

bool ownsFontId(const int fontId) {
  if (fontId == 0) return false;
  for (const auto& entry : g_fonts) {
    for (const int id : entry.fontIds) {
      if (id == fontId) return true;
    }
  }
  return false;
}

namespace {

void releasePreviewSlot(GfxRenderer& renderer, const int slot) {
  if (slot < 0 || slot > 1) return;
  if (g_previewId[slot] != 0) {
    renderer.unregisterTtfFont(g_previewId[slot]);
    renderer.removeFont(g_previewId[slot]);
    g_previewId[slot] = 0;
  }
  g_previewFont[slot].reset();
}

}  // namespace

int registerPreviewFace(GfxRenderer& renderer, const uint8_t pointSize, const bool fourBit) {
  if (!g_ready || pointSize < READER_MIN_POINT_SIZE || pointSize > READER_MAX_POINT_SIZE) return 0;
  const int slot = fourBit ? 1 : 0;
  releasePreviewSlot(renderer, slot);

  auto font = std::unique_ptr<TtfEpdFont>(new (std::nothrow) TtfEpdFont());
  if (!font) return 0;
  font->addStreamSource(TtfEpdFont::Regular, readSource, &g_source, g_source.payloadSize);
  const bool havePsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
  const size_t cacheBytes = havePsram ? cacheBytesFor(pointSize, fourBit) : (fourBit ? 16 * 1024 : 8 * 1024);
  const uint16_t maxGlyphs = havePsram ? maxGlyphsFor(pointSize, fourBit) : (fourBit ? 256 : 96);
  if (!font->load(pointSize, /*twoBit=*/true, cacheBytes, maxGlyphs, fourBit)) {
    LOG_ERR("SFP", "preview face load failed @%upt %s", static_cast<unsigned>(pointSize),
            fourBit ? "4-bit" : "2-bit");
    return 0;
  }

  const int id = fourBit ? kPreview4BitFontId : kPreview2BitFontId;
  renderer.removeFont(id);
  renderer.insertFont(id, font->family());
  renderer.registerTtfFont(id, font.get());
  g_previewFont[slot] = std::move(font);
  g_previewId[slot] = id;
  LOG_INF("SFP", "preview face @%upt %s id=%d (%u KB cache)", static_cast<unsigned>(pointSize),
          fourBit ? "4-bit" : "2-bit", id, static_cast<unsigned>(cacheBytes / 1024));
  return id;
}

void releasePreviewFaces(GfxRenderer& renderer) {
  releasePreviewSlot(renderer, 0);
  releasePreviewSlot(renderer, 1);
}

}  // namespace system_font_pack

#endif  // CROSSPOINT_SYSTEM_FONT_PACK && CROSSPOINT_VECTOR_FONTS
