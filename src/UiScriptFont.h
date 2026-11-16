#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Small scalable face for the non-CJK UI scripts (Vietnamese, Czech, Polish,
// Turkish, Hebrew, Arabic, ...).  The CJK system pack cannot draw those
// codepoints, and downloading a 6 MB world-script font for a handful of menu
// labels is not reasonable.  This carrier is ~25 KB and is registered as a
// supplement at the UI sizes only:
//
//   CJK label        -> CJK pack (fewest missing)
//   Czech/Vietnamese -> this face (fewest missing)
//
// It is deliberately separate from the CJK family: merging the outlines into
// one file would mix Noto Sans CJK's Latin design with Noto Sans's and makes
// the coverage contract harder to audit.
namespace ui_script_font {

bool begin();  // validate + inflate the carrier into PSRAM (idempotent)
bool ready();

// Register one face per requested point size and attach it as a supplement to
// the CJK role ids. Safe to call when begin() failed (no-op).
void configure(GfxRenderer& renderer, const uint8_t* pointSizes, size_t count);

void release(GfxRenderer& renderer);

}  // namespace ui_script_font
