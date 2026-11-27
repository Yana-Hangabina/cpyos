#include <NewTrace.h>

#include <atomic>

#if defined(CROSSPOINT_NEW_TRACE)
#include <cstdlib>
#endif

namespace newtrace {
namespace {
std::atomic<std::size_t> gLastSize{0};
}  // namespace

void note(std::size_t size) { gLastSize.store(size, std::memory_order_relaxed); }

std::size_t take() { return gLastSize.exchange(0, std::memory_order_relaxed); }

}  // namespace newtrace

#if defined(CROSSPOINT_NEW_TRACE)
// `new` and `new[]` are replaceable; --wrap routes every reference from this
// image through these shims, which forward to the real libstdc++ operator and
// only add one store. They must not allocate or log: they run on every
// allocation and again inside the failure path.
//
// The mangled names carry the size type, and on this 32-bit xtensa ABI size_t is
// `unsigned int`, so the symbols are `_Znwj` / `_Znaj` -- NOT the `_Znwm` /
// `_Znam` of a 64-bit target. Wrapping the wrong name links clean and records
// nothing (verified against the 0.8.5 ELF: `nm | grep Znw` shows `_Znwj`), which
// is exactly the kind of silent no-op scripts/tests/test_new_trace_wiring.py now
// refuses.
extern "C" void* __real__Znwj(unsigned int size);
extern "C" void* __real__Znaj(unsigned int size);

extern "C" void* __wrap__Znwj(unsigned int size) {
  newtrace::note(size);
  return __real__Znwj(size);
}

extern "C" void* __wrap__Znaj(unsigned int size) {
  newtrace::note(size);
  return __real__Znaj(size);
}
#endif