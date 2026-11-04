#pragma once

#include <cstdint>

// The last chosen UI language, mirrored into NVS. Settings live on the SD card,
// so the SD-failure page mounts before any language can be read; this tiny cache
// is the only way to draw that page in the user's language instead of literal
// English. 0xFF means "never stored" (first boot, or NVS unavailable).
namespace bootlang {

void remember(uint8_t language);
uint8_t recall();

}  // namespace bootlang
