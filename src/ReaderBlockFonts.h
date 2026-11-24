#pragma once

#include <Epub/blocks/BlockFontProvider.h>

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Reader-side BlockFontProvider (layout milestone M2): maps a CSS font-size scale
// onto the system font pack's one-face-per-point-size ids, so headings and
// `font-size` rules lay out and draw at their real size.
//
// Scope and limitations, all deliberate:
//  * Only the system font pack is supported. An SD family loads exactly one reader
//    face, and an EPUB-embedded font is a single transient face, so both keep the
//    section font for every block (logged once per reader session).
//  * No PSRAM: the pack holds one small face, so block sizing is disabled entirely
//    (the C3 baseline is unchanged and pays nothing).
//  * At most kMaxBlockPointSizes extra faces are pinned per session; beyond that a
//    block falls back to the section font (logged once). Each packed face costs its
//    glyph cache (128-512 KB PSRAM, see SystemFontPack.cpp) plus one FreeType face,
//    so the bound is what keeps a pathological book from eating the heap.
class ReaderBlockFonts final : public BlockFontProvider {
 public:
  explicit ReaderBlockFonts(GfxRenderer& renderer);
  int resolveBlockFont(float scale) override;

 private:
  int resolveSystemPack(float scale);
  void logOnce(const char*& flag, const char* message);

  GfxRenderer& renderer_;
  // Snapshot of the reader face the section was built with. Recomputing it per query
  // would make a settings change mid-build mix two bases in one chapter.
  uint8_t basePointSize_ = 0;
  bool enabled_ = false;
  uint8_t extraPointSizes_[3] = {};
  uint8_t extraCount_ = 0;
  const char* loggedUnsupported_ = nullptr;
};
