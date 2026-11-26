// Host-build link stubs for symbols whose implementations are excluded from the
// simulator source set (network OTA, font extras, extra font faces). Never
// compiled for device targets.
#if defined(SIMULATOR)

#include "SystemFontPack.h"
#include "activities/settings/FontExtrasActivity.h"
#include "network/OtaUpdater.h"

OtaUpdater::OtaUpdaterError OtaUpdater::installDataPacksOnly(void (*)(void*), void*) { return INTERNAL_UPDATE_ERROR; }

void OtaUpdater::debugProbeDownloadMirrors() {}

bool FontExtrasActivity::wasPromptShownThisBoot() { return false; }

namespace system_font_pack {
bool ensurePointSize(GfxRenderer&, uint8_t) { return false; }
}  // namespace system_font_pack

#endif  // SIMULATOR
