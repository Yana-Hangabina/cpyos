#pragma once

// Who owns the single synchronous PDF job slot.
//
// This is the rule that lets a timed-out render become a *failed page* instead
// of a reboot.  A job that times out may still be running inside MuPDF and may
// still write through the pointer it was given (text jobs also write into
// caller containers), so the slot cannot be handed to the next job when the
// waiter gives up -- only when the worker itself says it is done.
//
// Two ways to get this wrong, both silent:
//   * recycle on timeout  -> the next job overwrites memory the worker is using;
//   * never recycle       -> after one slow page every later render is refused.
//
// Kept as pure logic (no FreeRTOS, no jobs) so the sequences can be pinned by a
// host test: submit / wait-times-out / worker-finally-finishes / submit again.

namespace pdf_job_slot {

struct State {
  bool inFlight = false;  // a job owns the slot
  bool done = false;      // that job's completion was published
};

enum class Verdict {
  Reuse,   // slot is free, or its owner finished: fill it for the next job
  Refuse,  // a job is still running in the worker: cannot touch the slot
};

inline Verdict submit(const State state) {
  if (!state.inFlight) return Verdict::Reuse;
  return state.done ? Verdict::Reuse : Verdict::Refuse;
}

// After waiting: completion returns the slot; a timeout does not.  The timeout
// case is why State.done is consulted on the *next* submit rather than here.
inline bool slotReturnedByWait(const bool completed) { return completed; }

// May the worker task be deleted right now?
//
// Never while a job it is executing is still in flight: vTaskDeleteWithCaps in
// the middle of a MuPDF page frees the stack and whatever the page holds, which
// is the heap corruption this project has already paid for twice.  An abandoned
// (timed-out) job is the case that makes this reachable from a *user* action --
// leaving the reader while such a job is still running.  A leaked task costs
// 128 KB of PSRAM until the next reboot; a corrupted heap costs the session.
inline bool mayStopWorker(const State state) { return !state.inFlight || state.done; }

}  // namespace pdf_job_slot
