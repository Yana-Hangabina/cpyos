#pragma once

#include <FaultCodes.h>

#include <cstdint>

// The last faults before the device died, kept across the reboot.
//
// Field reports are photographs (AGENTS 71): the user cannot attach a serial log,
// and the crash screen only described the *abort*. Everything the guard and
// callback work added is a refusal -- a crash that did not happen -- and a refusal
// that is invisible except on serial is a support blind spot: "the UI is English
// and my books are tofu" with no clue that the cause was a refused flash read.
//
// So every recorded fault goes into a small RTC_NOINIT ring that survives the
// panic reboot, and CrashActivity prints it before the panic itself. The device
// that survives to write this ring is the device that averted something: the ring
// answers "what was going wrong in the minutes before it died".
//
// Three properties decide the shape of this API:
//   * it must be safe at the worst possible moment. raise() takes no lock,
//     allocates nothing and calls no libc, so the panic hooks and the scan
//     watchdog can use it before the restart they cause. It is deliberately NOT in
//     IRAM: an assert or abort reaches IDF's handler with the caches still
//     enabled -- the same assumption lib/hal/HalSystem.cpp documents for its own
//     hook -- and spending scarce internal RAM to insure against a case that does
//     not exist would take bytes away from the Wi-Fi driver.
//   * it must be small. RTC slow memory is a few kilobytes and already carries the
//     panic record plus 16 log lines, so the ring is 16 slots (~400 B) and the
//     assert location is 32 B. A larger one could fail the link for reasons that
//     have nothing to do with diagnostics.
//   * it must degrade honestly. Overflow recycles the least-recently-touched slot
//     instead of silently refusing to record, and an unlisted code contributes no
//     tag at all: half a "STK-" on a photograph decodes to less than nothing.
namespace fault {

// One distinct code. `count` is how many times it happened this session, so a
// 400-frame underrun is one line rather than 400 of them. `seq` is a monotonic
// touch stamp used both for ordering and for eviction; the 32-bit fields keep every
// update to a single aligned store, which is what makes a concurrent raise() from
// two tasks merely imprecise instead of corrupt.
struct Slot {
  uint32_t code = 0;
  uint32_t count = 0;
  uint32_t firstSec = 0;  // uptime seconds of the first occurrence
  uint32_t lastSec = 0;   // ...and of the most recent one
  uint32_t detail = 0;    // the raising site's number: a byte count, an error, an index
  uint32_t seq = 0;       // touch order, newest highest
};

constexpr uint8_t kCapacity = 16;

// Record one occurrence. `detail` is whatever number the site has that a photo
// cannot convey (requested bytes, esp_err, chapter index); pass 0 when there is
// none. Never fails, never blocks, never allocates.
void raise(Code code, uint32_t detail = 0);

// Copy the ring out, most recently touched first. Returns the number of slots
// written (<= min(kCapacity, max)).
uint8_t copy(Slot* out, uint8_t max);

// True once anything has been recorded and not cleared by a clean boot.
bool armed();

uint16_t totalOccurrences();

// Called from HalSystem::begin(). A boot the device asked for (flash, restart,
// deep-sleep wake) discards the previous session's trail -- it is not evidence
// about this one -- while a panic/watchdog/brown-out boot keeps what the crash
// left behind. Idempotent within a boot.
void onBoot(bool deliberateReset);

// The panic hooks' entry point: classify the abort/panic message, record the
// matching code, and keep the assert's `file:line` when the message has one.
// Runs inside IDF's panic path, so it is bounded, allocation-free text scanning
// only; it must never be the reason a crash gets worse.
void notePanicAbort(const char* message);

// Same, for the C++ runtime hooks (terminate / new_handler) where the type name is
// known but there is no panic message to classify.
// `detail` rides along with the code; the bad_alloc hook passes the requested
// size so the next photograph names the allocation that failed.
void noteCppFailure(const char* exceptionName, uint32_t detail = 0);

// The number recorded most recently for `code` (0 when absent). The crash
// screen uses it for the failed allocation's size and the heap at that moment.
uint32_t detailOf(Code code);

// "cache_utils.c:127" for an assert, "" when the death was not an assert. Points
// into static storage; valid until the next notePanicAbort().
const char* assertLocation();

// Formats the ring into `line` (capacity kLineCapacity) as
//   "STK-08x2 STK-05x11 TSK-03x1"
// newest first, up to `maxItems` entries, and returns how many entries were
// written. Used by both the crash screen and crash_report.txt, so a photograph and
// a file cannot disagree about what the ring contained.
// A full ring entry is `STK-08x1` (8 characters) plus a separator; the worst
// realistic case is 16 entries whose counters ran into five digits, i.e. ~13
// characters each, so 224 holds the whole ring and formatLine never has to drop an
// entry because of the buffer. The screen prints fewer entries on purpose
// (`maxItems`), the report must not lose any.
constexpr uint8_t kLineCapacity = 224;
uint8_t formatLine(char* line, uint16_t capacity, uint8_t maxItems, uint32_t* lastSecondOut);

// A longer, human-readable form for crash_report.txt only (never the screen):
//   "STK-08 x2  last 41s  language pack read refused; UI falls back to English"
// Takes the slot, not an index: the caller has already decided the order.
uint16_t formatDetail(char* out, uint16_t capacity, const Slot& slot);

#if defined(SIMULATOR)
// Host-only seam (test/record_faults): the device clock is uptime seconds, which a
// host test cannot advance. This sets it directly so first/last/eviction behaviour
// is checkable deterministically. Never compiled into firmware.
void setClockForTest(uint32_t seconds);
#endif

}  // namespace fault
