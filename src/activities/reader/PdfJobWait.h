#pragma once

// The rule for waiting on the PDF worker task.
//
// `waitPdfJob()` used to be `while (!job.done) vTaskDelay(2)` with no deadline
// and no log.  That is how a page turn can go *completely silent*: the UI task
// spins in the delay (task state `block`), the worker is blocked on an empty
// queue because it is wedged inside MuPDF on a page it cannot allocate for, and
// nothing is ever printed.  On a device with no BOOT/RESET button an unbounded
// wait on another task is not a design, it is a brick with extra steps
// (docs/engineering/pdf-xip.md, "the reader stalls after a network session").
//
// The deadline is not the interesting part; the *object lifetime* is.  A job
// that times out may still be finished by the worker minutes later, and the
// worker writes through the pointer it was given.  So the job must never be a
// caller's stack temporary -- an abandoned wait would otherwise hand the worker
// a dead stack frame to write into.  Sync jobs therefore live in the activity's
// own storage, and the slot stays owned by the worker until its own `done` is
// observed; only then may the next job reuse it.
//
// On expiry the wait *returns* and the caller turns the page into a failed page
// ("cannot display, turn to skip").  The first version restarted the device
// instead, because the job lived on a caller's stack; with the static slot the
// restart is no longer needed and is a worse user outcome than a skip.
//
// Kept free of ESP headers so a host test can pin the arithmetic (including the
// 32-bit `millis()` wrap) instead of trusting a comment.

#include <cstdint>

namespace pdf_job_wait {

// Long enough for the worst page this reader is known to render (a 67 s
// embedded-font page, a 15 s bitmap page), short enough that a wedged worker
// produces a message while the user is still looking at the screen.
constexpr uint32_t kDefaultTimeoutMs = 180000;
// Progress is reported on this cadence so a slow page is visibly slow rather
// than indistinguishable from a hang.
constexpr uint32_t kProgressEveryMs = 10000;

struct Progress {
  bool announce;    // print `waiting` with the elapsed time and memory
  bool timedOut;    // give up, report the failure to the user
  uint32_t elapsed;  // ms since the wait began
};

// Modular elapsed time: `nowMs` and `startedAtMs` are both 32-bit `millis()`
// values and the difference is correct across the wrap, which happens about
// every 49 days of uptime (a real case here: the bench harness runs long).
inline uint32_t elapsedSince(const uint32_t nowMs, const uint32_t startedAtMs) {
  return nowMs - startedAtMs;
}

// `announced` is how many progress lines the caller has already printed.
// With zero it announces immediately: a wait that might become a diagnosis
// should say so at once rather than after ten seconds of silence, and the
// caller has the memory snapshot at the moment it starts waiting.
inline Progress decide(const uint32_t nowMs, const uint32_t startedAtMs, const uint32_t timeoutMs,
                       const unsigned announced) {
  const uint32_t elapsed = elapsedSince(nowMs, startedAtMs);
  const bool timedOut = elapsed >= timeoutMs;
  // `announced` is the count already printed, so the next line is due at
  // (announced + 1) * period.  Nothing is printed at the start: waiting on the
  // worker is how every page turn works, and a line per extraction would make
  // the log say "something is wrong" about the normal case -- which is exactly
  // how a real diagnosis gets drowned.  Silence means healthy; the first line
  // means the page has been busy for ten seconds.
  const bool due = elapsed >= static_cast<uint32_t>(announced + 1u) * kProgressEveryMs;
  return {due || timedOut, timedOut, elapsed};
}

}  // namespace pdf_job_wait
