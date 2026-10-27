#pragma once
#include <cstdint>

#include "EpdFontData.h"

// Address of a glyph's bitmap inside a font's resident bitmap array.
//
// The null check is not defensive noise: an SD-card face running in stub mode has
// EpdFontData::bitmap == nullptr (its resident mini-arena was dropped under internal-heap
// pressure; SdCardFont.cpp memsets stubData and never assigns bitmap). Such a face serves
// glyphs only through its overflow ring, and glyph->dataOffset is then a large file-relative
// value. If the ring lookup in GfxRenderer::getGlyphBitmap() misses -- e.g. the ring was
// cleared on another task between getGlyph() and the lookup -- indexing the null base yields
// &nullptr[dataOffset]: a non-null, unmapped pointer that survives the caller's `== nullptr`
// check and faults on the first read (observed as a Cache error / MMU entry fault that
// rebooted the device while drawing a Chinese blurb). There is no resident bitmap in that
// case, so report "no glyph" and let the caller skip it.
//
// Header-only and dependency-free (EpdFontData.h is just <cstdint>) so it can be unit-tested
// on the host against the same code the renderer runs.
inline const uint8_t* residentGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) {
  if (fontData->bitmap == nullptr) return nullptr;
  return &fontData->bitmap[glyph->dataOffset];
}
