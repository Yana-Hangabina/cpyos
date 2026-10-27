#pragma once

#include <cstddef>

/// MuPDF's external-CMap hook.  The patched pdf-cmap-load.c (built with
/// -DNO_CJK) calls this for every non-Identity CMap.  The returned bytes must
/// stay valid until MuPDF has parsed them; this loader keeps a one-entry PSRAM
/// cache and replaces it on the next miss.
///
/// Returns nullptr when the name is invalid or the file is absent/short.
extern "C" const unsigned char* crosspoint_cmap_load(const char* name, size_t* size);
