#include "StackSafety.h"

#include <FaultLog.h>

#if !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
#include <esp_cpu.h>
#include <esp_memory_utils.h>
#endif

namespace memory {
namespace {

// -1 = no override (ask the hardware), 0 = force Allowed, 1 = force Refused.
// A plain int rather than an enum so `debugOverrideFlashGuard(-1)` can mean
// "stop overriding" without inventing a third guard state.
volatile int gOverride = -1;

// The verdict alone, with no side effect: separated from the recording so the
// purity of the decision stays testable (test/stack_safety) and so nothing about
// how the answer is reached can depend on whether a code was written down.
FlashGuard decide() {
  const int forced = gOverride;
  if (forced >= 0) return forced ? FlashGuard::RefusedPsramStack : FlashGuard::Allowed;

#if defined(SIMULATOR) || defined(CROSSPOINT_EMULATED)
  // No PSRAM and no cache on the host/simulator: the invariant cannot be
  // violated there, and tests reach the refusing branch through the override.
  return FlashGuard::Allowed;
#else
  return esp_ptr_external_ram(reinterpret_cast<const void*>(esp_cpu_get_sp())) ? FlashGuard::RefusedPsramStack
                                                                              : FlashGuard::Allowed;
#endif
}

}  // namespace

void debugOverrideFlashGuard(const int value) { gOverride = value; }

FlashGuard guardInternalFlash(const fault::Code site, const uint32_t detail) {
  const FlashGuard verdict = decide();
  // Recording is why `site` has no default. A refusal normally keeps the device up,
  // and that is exactly what makes it a support blind spot: a refused language pack
  // is an English UI, a refused font carrier is tofu, a refused slot read is an
  // install that cannot be confirmed -- none of which reaches a user's photograph
  // unless it is counted here. raise() is allocation-free and lock-free, so this
  // stays legal on the render task and in the panic path.
  if (verdict != FlashGuard::Allowed) fault::raise(site, detail);
  return verdict;
}

}  // namespace memory
