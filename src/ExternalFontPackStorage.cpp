#include "ExternalFontPackStorage.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include "ExternalFontPack.h"

#if defined(CROSSPOINT_MUPDF)
#include "pdf/PdfAssets.h"
#endif

namespace {
constexpr const char* kPackPath = "/.crosspoint/font_pack.dat";
constexpr size_t kMaxPackBytes = 4u * 1024u * 1024u;

// The parsed EpdFontData points into this buffer; it must outlive every font.
uint8_t* g_packBuffer = nullptr;
ExternalFontPack g_pack;
}  // namespace

bool loadExternalFontPack(EpdFontData& cjk12, EpdFontData& title) {
#if defined(CROSSPOINT_MUPDF)
  // First boot after flashing: unpack the compressed font/CMaps from app0+
  // spiffs to the SD card before the normal loader looks for them.
  ensurePdfAssets();
#endif
  if (g_packBuffer) {
    const EpdFontData* a = g_pack.get("cjk12");
    if (!a) return false;
    cjk12 = *a;
    const EpdFontData* b = g_pack.get("title");
    title = b ? *b : *a;
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("FONT", kPackPath, file)) {
    LOG_ERR("FONT", "external font pack missing: %s", kPackPath);
    return false;
  }
  const size_t size = file.fileSize();
  if (size < ExternalFontPack::kHeaderSize || size > kMaxPackBytes) {
    LOG_ERR("FONT", "external font pack has implausible size: %u", static_cast<unsigned>(size));
    file.close();
    return false;
  }

  uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) {
    LOG_ERR("FONT", "external font pack: PSRAM allocation failed (%u bytes)", static_cast<unsigned>(size));
    file.close();
    return false;
  }
  const int got = file.read(buffer, size);
  file.close();
  if (got != static_cast<int>(size)) {
    LOG_ERR("FONT", "external font pack short read: %d/%u", got, static_cast<unsigned>(size));
    heap_caps_free(buffer);
    return false;
  }
  if (!g_pack.parse(buffer, size)) {
    LOG_ERR("FONT", "external font pack rejected: %u bytes", static_cast<unsigned>(size));
    heap_caps_free(buffer);
    return false;
  }
  const EpdFontData* a = g_pack.get("cjk12");
  if (!a) {
    LOG_ERR("FONT", "external font pack lacks cjk12");
    heap_caps_free(buffer);
    return false;
  }
  const EpdFontData* b = g_pack.get("title");
  if (!b) LOG_INF("FONT", "external font pack has no title face; using cjk12");

  g_packBuffer = buffer;
  cjk12 = *a;
  title = b ? *b : *a;
  LOG_INF("FONT", "external font pack: %u bytes, cjk12%s loaded", static_cast<unsigned>(size),
          b ? "+title" : "");
  return true;
}

void releaseExternalFontPack() {
  if (!g_packBuffer) return;
  heap_caps_free(g_packBuffer);
  g_packBuffer = nullptr;
  LOG_INF("FONT", "external font pack released for PDF render");
}
