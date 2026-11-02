#pragma once

#if defined(CROSSPOINT_MUPDF)

#if !defined(BOARD_HAS_PSRAM) && !defined(CROSSPOINT_EMULATED)
#error "CROSSPOINT_MUPDF is a PSRAM/readpico-only feature; do not enable it on C3"
#endif

/// First-boot installer for the PDF font pack and GB CMaps.  The compressed
/// assets live in app0/spiffs, so a freshly flashed device does not depend on
/// the user copying `/.crosspoint/font_pack.dat` or `/.crosspoint/cmaps/`.
bool ensurePdfAssets();

#endif
