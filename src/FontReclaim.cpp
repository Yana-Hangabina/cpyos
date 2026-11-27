#include "FontReclaim.h"

#include "NetworkStartup.h"
#include "SdCardFontSystem.h"

namespace font_reclaim {

void releaseForMemory(GfxRenderer& renderer) {
  sdFontSystem.releaseLoadedFont(renderer);
  // Arm the pairing: font_health::tick() reloads when the radio is idle and no
  // worker holds the network. Arming is idempotent, so a caller that also runs
  // NetworkStartup::prepare() (which arms for its own reason) does no harm.
  NetworkStartup::rearmRenderMemoryRestore();
}

}  // namespace font_reclaim
