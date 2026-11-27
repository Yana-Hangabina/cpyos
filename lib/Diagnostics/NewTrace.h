#pragma once

#include <cstddef>
#include <cstdint>

// Which allocation was in flight when `operator new` failed.
//
// The 0.8.4 field report said `PNC-04` (operator new failed) and nothing else:
// the abort PC is the new_handler itself, so even a symbolized ELF cannot say
// what size was asked for or how much heap was left. On a device whose heap is
// nearly gone that is the one fact that identifies the caller.
//
// Two globals and two linker wraps:
//   * `note()` is called from the wrapped `operator new` / `operator new[]`
//     (the only writer) and keeps a single relaxed 32-bit store;
//   * the new_handler `take()`s it once, pairs it with the heap numbers and
//     raises STR-04 / the bad_alloc detail, so a *later*, unrelated terminate
//     cannot inherit the size.
//
// CROSSPOINT_NEW_TRACE gates both the recording and the `--wrap=_Znwm` /
// `--wrap=_Znam` link flags (platformio.ini). Without it this is a no-op, so
// targets that do not carry the flags (the Plus CMake build, hosts) are
// byte-for-byte unchanged.
namespace newtrace {

void note(std::size_t size);

// The last recorded size, consumed (zeroed) by the caller.
std::size_t take();

}  // namespace newtrace