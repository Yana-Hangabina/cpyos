#pragma once
#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <Logging.h>

namespace ReaderUtils {
// Experiment only (CMD:NOFULL): demote every reader GC16 to the differential
// profile, including chapter boundaries, so a long run can answer whether the
// residue saturates. Never enable this in a shipped build path.
inline bool& fullRefreshSuppressed() {
  static bool suppressed = false;
  return suppressed;
}

inline HalDisplay::RefreshMode consumeRefreshMode(int& pagesUntilFullRefresh, bool deferClean = false) {
  if (deferClean && pagesUntilFullRefresh <= 1) {
    // Keep the debt: the next ordinary page must still clean.
    pagesUntilFullRefresh = 1;
    return HalDisplay::FAST_REFRESH;
  }
  // The due page cleans the panel. Read Pico needs GC16 (FULL_REFRESH): GL16 (HALF)
  // keeps an already-white background and never pre-blackens, so it cannot clear the
  // gray floor that differential turns accumulate -- it only changes the waveform.
  // FULL drives every pixel black then white, which is what actually resets the panel,
  // and it propagates: EpdiyLcdDriver::displayGray steps Fast up to Half but leaves Full
  // alone, so the base and grey planes go out together as GC16. Ordinary pages stay
  // FAST -> GL16: anti-aliasing preserved, no flash. Vendor firmware cleans the same way
  // every APP_GC16_EVERY = 14 pages.
#if FREEINK_DEVICE_READPICO
  const auto cleanMode = HalDisplay::FULL_REFRESH;
#else
  const auto cleanMode = HalDisplay::HALF_REFRESH;
#endif
  auto mode = (pagesUntilFullRefresh <= 1) ? cleanMode : HalDisplay::FAST_REFRESH;
  // Settings -> Display -> Periodic full refresh is off by default since
  // 2026-10-29 (60 differential pages left only the faint "thin paper" residue on
  // hardware). While it is off, every cadence clean -- including the one a
  // chapter start asks for through this counter -- is demoted to the
  // differential profile. The gray/image promotion (nextRefreshOverride) is a
  // different path and still cleans: midtones cannot be erased differentially.
  const bool demote = mode != HalDisplay::FAST_REFRESH &&
                      (fullRefreshSuppressed() || SETTINGS.periodicFullRefresh == 0);
  if (demote) mode = HalDisplay::FAST_REFRESH;
  if (pagesUntilFullRefresh <= 1) {
    if (!demote) LOG_INF("ERS", "page refresh: FULL cadence (period=%d pages)", SETTINGS.getRefreshFrequency());
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    pagesUntilFullRefresh--;
  }
  return mode;
}

// One helper, blocking or deferred: the async form starts the refresh and
// returns so the caller can overlap CPU work with the panel's refresh time.
// Async callers must not touch the framebuffer until
// renderer.waitRefreshComplete() and must rebuild the differential baseline
// before the next page turn (the tiled grayscale cleanup does).
inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool async = false) {
  const auto mode = consumeRefreshMode(pagesUntilFullRefresh);
  if (async) {
    renderer.displayBufferAsync(mode, DisplayRefreshContext::ContinuousReading);
  } else {
    renderer.displayBuffer(mode, DisplayRefreshContext::ContinuousReading);
  }
}

// Display the B/W base of a page whose grayscale pass follows. Panels that
// combine the base (Paper Mono) defer the activation so base + gray planes go
// out as one waveform — displaying the base separately makes the gray pass
// re-drive the whole text body (a visible flash). Other panels display
// normally. Same refresh-cadence bookkeeping as displayWithRefreshCycle.
inline void displayBaseWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool manualRefresh) {
  if (!renderer.supportsTextOnlyCombinedBase()) {
    displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    return;
  }
  if (renderer.supportsReaderTransitions()) renderer.waitRefreshComplete();
  const bool transition = !manualRefresh && renderer.canUseTextTransition();
  HalDisplay::RefreshMode mode;
  if (transition && pagesUntilFullRefresh == 0) {
    // A trusted entry starts the cycle; a real due counter (1) keeps its debt.
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    mode = HalDisplay::FAST_REFRESH;
  } else {
    mode = consumeRefreshMode(pagesUntilFullRefresh, transition);
  }
#if FREEINK_DEVICE_READPICO
  // Read Pico's anti-aliased text turn is GL16 (37 phases, ~415 ms of scan): it
  // is the lightest differential profile that still carries text mid-tones -- DU
  // at 20 phases leaves them flat -- and it is what the ~600 ms cached turn is
  // built on. Only the ordinary turn is promoted; a due clean keeps whatever the
  // cadence chose, so a real reset still happens on schedule.
  if (mode == HalDisplay::FAST_REFRESH) mode = HalDisplay::HALF_REFRESH;
#endif
  renderer.displayGrayscaleBase(mode, DisplayRefreshContext::TextOnlyAntiAliasing);
}
}  // namespace ReaderUtils
