#pragma once

class GfxRenderer;

// The one way to give up font memory *for memory pressure*.
//
// Four screens used to call SdCardFontSystem::releaseLoadedFont() by hand when
// they wanted RAM (BLE start, the browser, WeRead, the boot path, SD updates).
// Each of them forgot the second half of the contract: the pairing that makes the
// main loop load the faces back once the pressure is gone. After leaving such a
// screen to Home, characters only the 3,500-glyph core carries (冥/霄/瀚; 邪/世纪
// in titles) stayed tofu until a reader happened to relink.
//
// Releasing for a *different* reason -- the text-settings preview budget, the
// reader re-entering with another family, the PDF engine needing room -- is not
// this call: those paths reload synchronously and must not arm a delayed restore.
namespace font_reclaim {

void releaseForMemory(GfxRenderer& renderer);

}  // namespace font_reclaim
