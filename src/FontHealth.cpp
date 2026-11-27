#include "FontHealth.h"

#include <Arduino.h>

#include <FaultLog.h>
#include <GfxRenderer.h>
#include <SystemCoreFont.h>
#include <SystemFontPack.h>

#include "BackgroundWork.h"
#include "CrossPointSettings.h"
#include "FontRestorePolicy.h"
#include "Logging.h"
#include "NetworkStartup.h"
#include "SdCardFontSystem.h"
#include "Typography.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "fontIds.h"

namespace font_health {
namespace {

// The roles every UI screen draws through. If one of these is not registered,
// the text that uses it renders as nothing (not tofu -- tofu means the face
// exists and lacks the codepoint), which is the "settings labels blank" report.
constexpr int kUiRoles[] = {UI_10_FONT_ID, UI_12_FONT_ID, SMALL_FONT_ID, CONTROL_18_FONT_ID};

// Both repair paths can legitimately fail (system pack unavailable, PSRAM
// exhausted). Rebuilding a multi-second font set every second and repainting on
// failure turns a degrade into a permanent freeze loop, so each path paces
// itself with a bounded backoff and only repaints when something changed.
struct Pacing {
  uint32_t nextMs = 0;
  uint32_t intervalMs = 1000;
};

constexpr uint32_t kMinBackoffMs = 1000;
constexpr uint32_t kMaxBackoffMs = 60000;

void backOff(Pacing& pacing) {
  const uint32_t doubled = pacing.intervalMs * 2;
  pacing.intervalMs = doubled > kMaxBackoffMs ? kMaxBackoffMs : doubled;
  pacing.nextMs = millis() + pacing.intervalMs;
}

bool due(Pacing& pacing, uint32_t now) {
  if (static_cast<int32_t>(now - pacing.nextMs) < 0) return false;
  pacing.nextMs = now + pacing.intervalMs;
  return true;
}

bool anyRoleMissing(GfxRenderer& renderer) {
  RenderLock lock;
  for (const int id : kUiRoles) {
    if (!renderer.isFontRegistered(id)) return true;
  }
  return false;
}

void restoreReclaimedFonts(GfxRenderer& renderer, ActivityManager& activityManager, Pacing& pacing) {
  font_restore::Situation situation;
  situation.pending = NetworkStartup::renderMemoryRestorePending();
  situation.radioIdle = NetworkStartup::radioIdle();
  situation.stationClaimed = NetworkStartup::stationClaimed();
  situation.networkHeld = NetworkStartup::networkHeld() || background_work::active();
  situation.elapsedMs = static_cast<uint32_t>(millis() - NetworkStartup::renderMemoryArmedAtMs());
  if (!font_restore::due(situation)) return;
  if (!due(pacing, millis())) return;

  NetworkStartup::clearRenderMemoryRestore();
  const unsigned long reloadStart = millis();
  {
    // The independent render task shares the font maps and resident font objects.
    RenderLock lock;
    sdFontSystem.ensureLoaded(renderer);
    // unloadAll() cleared the role->pack bindings (SdCardFontManager), and the
    // reclaim path has no other owner that re-applies them: without this the
    // labels that draw with the smallText role stay blank until some screen
    // happens to re-apply typography. Idempotent and cheap.
    typography::apply(renderer, SETTINGS.uiFontScale);
  }

  const bool rolesBack = !anyRoleMissing(renderer);
#if CROSSPOINT_VECTOR_FONTS
  // A missing supplement is only a failure when a core face exists to load: with
  // no card face there is nothing to stand up, and retrying would burn PSRAM for
  // a degrade the user cannot avoid.
  const bool supplementsOk = sdFontSystem.hasSupplementFonts() || !system_core_font::faceOnSd();
  if (!rolesBack || !supplementsOk) {
    LOG_ERR("MAIN", "font restore incomplete; retrying in %lu ms",
            static_cast<unsigned long>(pacing.intervalMs * 2));
    NetworkStartup::rearmRenderMemoryRestore();
    backOff(pacing);
    return;  // no repaint for a repair that did not happen
  }
#else
  (void)rolesBack;
#endif
  pacing.intervalMs = kMinBackoffMs;
  pacing.nextMs = millis() + 500;
  // INFO, not DBG: this is the line that proves the reclaim was reverted, and the
  // serial default level hides debug output (the 0.8.2 report was exactly a
  // missing revert nobody could see).
  LOG_INF("MAIN", "render memory reclaimed for the network was given back: %lu ms",
          static_cast<unsigned long>(millis() - reloadStart));
  activityManager.requestUpdate();
}

void repairUnboundRoles(GfxRenderer& renderer, ActivityManager& activityManager, Pacing& pacing) {
  // typography binds roles to the system pack; without it there is nothing to
  // rebuild. Report once per backoff instead of freezing on a retry loop.
  if (!system_font_pack::ready()) {
    LOG_ERR("MAIN", "UI font roles unbound; system font pack unavailable, retrying in %lu ms",
            static_cast<unsigned long>(pacing.intervalMs * 2));
    backOff(pacing);
    return;
  }
  const unsigned long repairStart = millis();
  {
    RenderLock lock;
    sdFontSystem.ensureLoaded(renderer);
    typography::apply(renderer, SETTINGS.uiFontScale);
  }
  const bool stillMissing = anyRoleMissing(renderer);
  LOG_ERR("MAIN", "UI font roles were unbound; rebuilt in %lu ms%s", static_cast<unsigned long>(millis() - repairStart),
          stillMissing ? " (still incomplete: system font pack unavailable)" : "");
  fault::raise(fault::Code::FontRolesRebuilt, stillMissing ? 1u : 0u);
  if (stillMissing) {
    backOff(pacing);
    return;
  }
  pacing.intervalMs = kMinBackoffMs;
  pacing.nextMs = millis() + 1000;
  activityManager.requestUpdate();
}

}  // namespace

void tick(GfxRenderer& renderer, ActivityManager& activityManager) {
  static Pacing restorePacing;
  static Pacing repairPacing;
  const uint32_t now = millis();

  // The restore pairing is cheap to test every half second (a flag plus the idle
  // situation); the expensive part runs at most once per pacing interval.
  if (static_cast<int32_t>(now - restorePacing.nextMs) >= 0) {
    restoreReclaimedFonts(renderer, activityManager, restorePacing);
  }

  if (static_cast<int32_t>(now - repairPacing.nextMs) >= 0) {
    if (anyRoleMissing(renderer)) {
      repairUnboundRoles(renderer, activityManager, repairPacing);
    } else if (repairPacing.intervalMs != kMinBackoffMs) {
      repairPacing.intervalMs = kMinBackoffMs;
      repairPacing.nextMs = now + kMinBackoffMs;
    }
  }
}

}  // namespace font_health