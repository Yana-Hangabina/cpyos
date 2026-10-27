#pragma once

#if defined(CROSSPOINT_MUPDF)

/// First-boot installer for the PDF font pack and GB CMaps.  The compressed
/// assets live in app0/spiffs, so a freshly flashed device does not depend on
/// the user copying `/.crosspoint/font_pack.dat` or `/.crosspoint/cmaps/`.
bool ensurePdfAssets();

#endif
