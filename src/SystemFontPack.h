#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Flash-resident scalable font pack for no-PSRAM targets.
//
// The pack lives in the data/spiffs partition and contains one TrueType font
// file. TtfEpdFont streams glyphs from the partition, so the multi-MB CJK face
// never sits in internal RAM. The same face is registered at the sizes used by
// the type scale; changing the UI scale only selects another registered size,
// it never falls back to a different font family.
namespace system_font_pack {

// Load and validate the pack (spiffs or the embedded carrier). No faces are
// registered here: typography::apply() calls configure() with the sizes the
// current UI scale and reader setting need, so a PSRAM build does not pay for
// six FreeType faces when three are in use.
bool begin(GfxRenderer& renderer);

// Register the wanted point sizes and release faces that are no longer needed.
// Sizes outside 10..34 are ignored. Safe to call when the pack is not ready.
void configure(GfxRenderer& renderer, const uint8_t* pointSizes, size_t count);

void release(GfxRenderer& renderer);
bool ready();

// True when the registered reader faces carry 4-bit coverage (the 16-level
// text path's prerequisite). Always false on the no-PSRAM bitmap profile.
bool faceIsFourBit();

// True when the id belongs to a face this pack registered. Lets the reader
// decide whether its live reader face came from the pack (fourBit) or from an
// SD family (.cpfont / TTF) without guessing from the family name.
bool ownsFontId(int fontId);

// Debug-only comparison faces (CMD:FONTDEMO). They are registered outside the
// reader/UI set, use the same streamed source and the same point size, and are
// released as a pair. Returns 0 when the pack is unavailable or the face fails.
int registerPreviewFace(GfxRenderer& renderer, uint8_t pointSize, bool fourBit);
void releasePreviewFaces(GfxRenderer& renderer);

}  // namespace system_font_pack
