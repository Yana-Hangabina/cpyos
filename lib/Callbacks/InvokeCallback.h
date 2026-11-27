#pragma once

#include <functional>
#include <utility>

// Calling an empty std::function is fatal here, not catchable.
//
// This build uses -fno-exceptions: libstdc++ still *throws*
// std::bad_function_call, but no application frame has a landing pad, so the
// exception reaches std::terminate and abort() reboots the device. The field
// symptom is a screen reading only `abort() was called at PC 0x423569f3`
// (that PC is `__cxxabiv1::__terminate` in the 0.8.2 image) with no backtrace.
//
// So the guard is mandatory at every invocation of a callback that can
// legitimately be unset: theme row formatters, navigation callbacks, parser
// hooks, popup handlers. scripts/tests/test_callback_invocation_guard.py is the
// machine gate that stops new sites from re-introducing the bare call.
//
// Skipping a callback is a *degradation* and must stay visible: the first time
// each tag is seen it logs an error, so "the value column is empty" is never
// mistaken for intended behaviour.
namespace callbacks {

// True the first time `tag` is seen, false afterwards. One line per call site
// per boot: a per-frame log would bury everything else, silence would hide the
// bug. Defined in InvokeCallback.cpp so this header needs no logging dependency.
bool noteEmptyOnce(const char* tag);
void reportEmpty(const char* tag);

// Capacity of the site table, and the detail value recorded when a site arrives
// after it is full. Both are compile-time constants because a skipped callback
// can happen on the render task, where growing a container is the very thing that
// would abort the device.
constexpr size_t kKnownTags = 16;
constexpr size_t kUnknownSite = kKnownTags;

// The one call a skipped callback needs: it records the occurrence in the fault
// ring (CB-01, detail = the site's table index; TSK-07 when the index could not be
// kept) and logs the site once per boot. The ring is what makes the skip visible
// in a photograph -- the serial line is not.
void noteEmpty(const char* tag);

// Resolve a recorded index back to the site's tag for crash_report.txt; "" for an
// index the table never held.
const char* siteTagAt(size_t index);

// Invoke `fn`, or return `fallback` when it is unset. `Ret` is written
// explicitly by the caller: it keeps the fallback type from decaying (a string
// literal would otherwise deduce to an array reference) and makes the degraded
// result type obvious at the call site.
template <typename Ret, typename Fn, typename... Args>
Ret callOr(const char* tag, const Fn& fn, Ret fallback, Args&&... args) {
  if (!fn) {
    noteEmpty(tag);
    return fallback;
  }
  return static_cast<Ret>(fn(std::forward<Args>(args)...));
}

// Same, for void callbacks.
template <typename Fn, typename... Args>
void call(const char* tag, const Fn& fn, Args&&... args) {
  if (!fn) {
    noteEmpty(tag);
    return;
  }
  fn(std::forward<Args>(args)...);
}

}  // namespace callbacks
