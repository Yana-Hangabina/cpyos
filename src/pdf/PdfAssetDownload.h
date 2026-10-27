#pragma once

#if defined(CROSSPOINT_MUPDF)

/// Optional first-boot network provisioning.  The current build has no
/// release URL yet, so this returns false without touching the radio.
bool tryDownloadPdfAssets();

#endif
