#pragma once

#include <cstdint>

class GfxRenderer;

// Bundled monochrome emoji fallback (Noto Emoji subset, OFL).
//
// The face is read from the data partition once and registered with the renderer;
// GfxRenderer::drawText/getTextWidth split at emoji boundaries and use it for
// those segments only. Every allocation is checked and the face is simply not
// installed when there is no room or the pack is absent, so a missing/failed
// pack degrades to today's tofu, never to an abort.
namespace emoji_font {

bool begin(GfxRenderer& renderer, uint8_t pointSize);
// Reload at a new reader point size. No-op when unchanged. Call on the UI task
// before the next render (EpubReaderActivity::onEnter does).
void setPointSize(GfxRenderer& renderer, uint8_t pointSize);
uint8_t pointSize();
int fontId();

}  // namespace emoji_font
