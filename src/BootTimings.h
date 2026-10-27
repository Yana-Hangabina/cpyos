#pragma once
#include <cstddef>
#include <cstdint>

// Boot stage timing (docs/07 D2).
//
// setup() stamps millis() at each milestone into RTC memory, and the record is
// frozen as soon as the first frame is physically on the panel. Two questions
// this answers that nothing else in the tree could:
//   * "why is boot slow?" — one log line with every stage's timestamp.
//   * "where did it die?" — a boot that never painted (hang, panic, watchdog)
//     leaves its trail behind, and the crash screen can show the last stage.
//
// The record is 16-bit milliseconds, so a stage reached after 65.5 s clamps;
// every real boot reaches its stages far below that.
namespace boottimings {

// Member names must dodge the project's object-like macros (`#define Storage
// HalStorage::getInstance()` in lib/hal/HalStorage.h would otherwise rewrite the
// enumerator), which is why they are spelled SdCard/Stores/Panel.
enum class Stage : uint8_t {
  BootStart = 0,  // setup() entry
  Board,          // expander/PMU, power manager, tilt sensor, board clock
  SdCard,         // SD mount (includes the Sdmmc retry path)
  Stores,         // settings, persisted stores, i18n, theme, trusted time
  Panel,          // panel and fonts
  Splash,         // first frame requested (splash, or the routed activity)
  FirstPaint,     // first frame physically painted
  Count,
};

// Starts a new record and ages the previous one. Call first thing in setup().
void begin();

// Records the first arrival at `stage`; later marks for the same stage are
// ignored, so per-frame callers cost one compare. It is a no-op after
// markFirstPaint() has frozen the record.
void mark(Stage stage);

// Records FirstPaint, freezes the record and logs the one summary line. Safe to
// call on every frame: it acts once.
void markFirstPaint();

// True when the previous boot never reached a first paint (hang, panic or
// watchdog reset), i.e. its trail is still available.
bool hasIncompletePrevious();

// Writes "sd 410ms" for the incomplete previous boot, 0 and an empty string
// when there is none.
size_t incompletePrevious(char* out, size_t cap);

}  // namespace boottimings
