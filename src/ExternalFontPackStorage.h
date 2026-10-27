#pragma once

#include "EpdFontData.h"

/// Load the external CJK faces from /.crosspoint/font_pack.dat into PSRAM.
/// Fills cjk12/title on success.  The pack is validated (magic/version/CRC) and
/// the returned EpdFontData pointers stay valid until releaseExternalFontPack().
bool loadExternalFontPack(EpdFontData& cjk12, EpdFontData& title);

/// Free the external font pack so PDF rendering can use its ~1.3 MB of PSRAM.
/// The caller must not draw with the external faces until loadExternalFontPack()
/// has been called again.
void releaseExternalFontPack();
