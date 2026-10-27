// Host regression for GfxRenderer::getGlyphBitmap's resident-bitmap fallback.
//
// The device rebooted with "Cache error / MMU entry fault" while drawing a Chinese blurb.
// An SD-card face running in stub mode has EpdFontData::bitmap == nullptr (SdCardFont.cpp
// memsets stubData and never assigns it), and a missed overflow-ring lookup fell through to
// &fontData->bitmap[glyph->dataOffset]: &nullptr[large] is a NON-null, unmapped pointer that
// survived the caller's `== nullptr` check and faulted on the first read.
//
// residentGlyphBitmap() is the exact function the renderer now calls, so these checks pin the
// production code (not a copy). The first case fails loudly if the null guard is removed.
#include <cstdint>
#include <cstdio>

#include <ResidentGlyphBitmap.h>

namespace {
int failures = 0;

void check(const bool ok, const char* what) {
  if (ok) return;
  std::printf("FAIL: %s\n", what);
  ++failures;
}
}  // namespace

int main() {
  // 1. The crash vector: stub-mode SD face (bitmap == nullptr) carrying the large
  //    file-relative dataOffset an overflow-ring glyph has. Before the guard this returned
  //    (uint8_t*)0x40000 -- non-null, unmapped, and fatal on the first dereference.
  {
    EpdFontData stub = {};
    stub.is2Bit = true;
    stub.bitmap = nullptr;
    stub.glyphMissCtx = nullptr;  // the ring lookup already missed
    stub.groups = nullptr;
    stub.vectorBitmapHandler = nullptr;
    EpdGlyph glyph = {};
    glyph.dataOffset = 0x40000;
    check(residentGlyphBitmap(&stub, &glyph) == nullptr,
          "null bitmap base + large dataOffset must yield nullptr, not &nullptr[offset]");
  }

  // 2. Same null base with offset 0: the contract is explicit, not an accident of aliasing.
  {
    EpdFontData stub = {};
    stub.bitmap = nullptr;
    EpdGlyph glyph = {};
    glyph.dataOffset = 0;
    check(residentGlyphBitmap(&stub, &glyph) == nullptr, "null bitmap base + offset 0 must yield nullptr");
  }

  // 3. A resident bitmap still resolves to the right byte.
  {
    static const uint8_t data[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    EpdFontData font = {};
    font.bitmap = data;
    EpdGlyph glyph = {};
    glyph.dataOffset = 3;
    const uint8_t* p = residentGlyphBitmap(&font, &glyph);
    check(p == &data[3], "resident bitmap must resolve to &bitmap[dataOffset]");
    check(p != nullptr && *p == 13, "resolved byte must be the glyph data");
  }

  // 4. Offset 0 on a resident bitmap is the base address.
  {
    static const uint8_t data[2] = {7, 9};
    EpdFontData font = {};
    font.bitmap = data;
    EpdGlyph glyph = {};
    glyph.dataOffset = 0;
    check(residentGlyphBitmap(&font, &glyph) == data, "resident bitmap offset 0 must yield the base address");
  }

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("glyph bitmap null-base guard: all checks passed\n");
  return 0;
}
