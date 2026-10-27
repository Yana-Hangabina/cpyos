#pragma once
/// Re-apply the external CJK faces after releaseExternalFontPack().
void refreshExternalFontFaces();
/// Point the CJK faces at built-in glyph data while the external pack is freed.
void useBuiltinFontFallback();
