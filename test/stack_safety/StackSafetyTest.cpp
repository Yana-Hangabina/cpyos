// Host regression for lib/Memory/StackSafety.{h,cpp} -- the single owner of
// "internal flash needs an internal-RAM stack".
//
// Field symptom this defends against (0.8.0 -> 0.8.2 upgrade crashes):
//   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
//       cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())
// The render task runs on a 64 KB PSRAM stack (CFF needs a 16 KB frame), so any
// flash operation it performs takes away its own stack. A read path that forgets
// to ask this question does not fail; it reboots the device.
//
// Compiled against the real StackSafety.cpp with two ESP doubles (stubs/), so
// the verdict logic -- not a copy of it -- is what is being checked.
#include <cstdio>
#include <cstdint>

#include <FaultCodes.h>
#include <FaultLog.h>
#include <StackSafety.h>

#include "esp_cpu.h"
#include "esp_memory_utils.h"

namespace {

// One code for the whole suite: the site argument exists so a refusal can be
// recorded, and section 7 is where that is proven.
constexpr fault::Code kSite = fault::Code::CoreFontProbeRefused;

uint32_t countOf(const fault::Code code) {
  fault::Slot slots[fault::kCapacity];
  const uint8_t found = fault::copy(slots, fault::kCapacity);
  for (uint8_t i = 0; i < found; ++i) {
    if (slots[i].code == static_cast<uint32_t>(code)) return slots[i].count;
  }
  return 0;
}

uint32_t recordedDetail(const fault::Code code) {
  fault::Slot slots[fault::kCapacity];
  const uint8_t found = fault::copy(slots, fault::kCapacity);
  for (uint8_t i = 0; i < found; ++i) {
    if (slots[i].code == static_cast<uint32_t>(code)) return slots[i].detail;
  }
  return 0;
}

int failures = 0;
int checks = 0;

void expect(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

memory::FlashGuard verdictAt(uint32_t stackPointer) {
  stacktest::stackPointer() = stackPointer;
  return memory::guardInternalFlash(kSite);
}

const char* nameOf(memory::FlashGuard guard) {
  return guard == memory::FlashGuard::Allowed ? "allowed" : "refused-psram-stack";
}

}  // namespace

int main() {
  // 1. A stack inside internal SRAM may touch internal flash.
  const memory::FlashGuard dram = verdictAt(stacktest::kDramBase + 0x1234u);
  expect(dram == memory::FlashGuard::Allowed, "DRAM stack is allowed");
  stacktest::stackPointer() = stacktest::kDramBase + 0x1234u;
  expect(memory::internalFlashAllowed(kSite), "internalFlashAllowed() agrees with guardInternalFlash()");

  // 2. A stack inside PSRAM is refused -- the case that used to be an assert.
  const memory::FlashGuard psram = verdictAt(stacktest::kPsramBase + 0x50000u);
  expect(psram == memory::FlashGuard::RefusedPsramStack, "PSRAM stack is refused");
  stacktest::stackPointer() = stacktest::kPsramBase + 0x50000u;
  expect(!memory::internalFlashAllowed(kSite), "convenience predicate follows the refusal");

  // 3. The window edges, where an off-by-one in the range test hides.
  expect(verdictAt(stacktest::kPsramBase) == memory::FlashGuard::RefusedPsramStack,
         "PSRAM base address itself is refused");
  expect(verdictAt(stacktest::kPsramBase - 4u) == memory::FlashGuard::Allowed,
         "one word below PSRAM is still allowed");
  expect(verdictAt(stacktest::kPsramEnd - 4u) == memory::FlashGuard::RefusedPsramStack,
         "last word of the PSRAM window is refused");
  expect(verdictAt(stacktest::kPsramEnd) == memory::FlashGuard::Allowed, "first address past PSRAM is allowed");
  expect(verdictAt(0u) == memory::FlashGuard::Allowed, "a null/garbage SP is not treated as PSRAM");

  // 4. A sweep across the map: only the PSRAM window may ever refuse.  This is
  //    the check that catches a widened range (which would silently disable
  //    every flash read, e.g. the language pack, and look like lost data).
  int refusedBands = 0;
  bool contiguous = true;
  uint32_t firstRefused = 0;
  uint32_t lastRefused = 0;
  for (uint32_t address = 0x3A000000u; address < 0x40000000u; address += 0x40000u) {
    const bool refused = verdictAt(address) == memory::FlashGuard::RefusedPsramStack;
    if (!refused) continue;
    if (refusedBands == 0) firstRefused = address;
    lastRefused = address;
    ++refusedBands;
  }
  contiguous = lastRefused >= firstRefused && (lastRefused - firstRefused) == (refusedBands - 1) * 0x40000u;
  expect(contiguous, "refusals form one contiguous band inside the PSRAM window");
  expect(firstRefused >= stacktest::kPsramBase && lastRefused < stacktest::kPsramEnd,
         "the refusal band never leaves the PSRAM window");
  expect(refusedBands > 0, "the sweep actually found PSRAM addresses (a vacuous green would hide a bug)");

  // 5. The decision is pure: repeated calls answer the same way, so a caller may
  //    ask twice (probe then read) without a second opinion.
  stacktest::stackPointer() = stacktest::kPsramBase + 1u;
  expect(memory::guardInternalFlash(kSite) == memory::guardInternalFlash(kSite), "no state carried between decisions");
  expect(nameOf(memory::guardInternalFlash(kSite)) == nameOf(memory::guardInternalFlash(kSite)), "labels are stable");

  // 6. The bench seam reaches both branches from one boot.  CMD:CRASHTEST relies
  //    on exactly this to prove the degraded path on hardware.
  memory::debugOverrideFlashGuard(1);
  stacktest::stackPointer() = stacktest::kDramBase + 8u;
  expect(memory::guardInternalFlash(kSite) == memory::FlashGuard::RefusedPsramStack, "override can force refusal");
  memory::debugOverrideFlashGuard(0);
  stacktest::stackPointer() = stacktest::kPsramBase + 8u;
  expect(memory::guardInternalFlash(kSite) == memory::FlashGuard::Allowed, "override can force permission");
  memory::debugOverrideFlashGuard(-1);
  expect(memory::guardInternalFlash(kSite) == memory::FlashGuard::RefusedPsramStack,
         "clearing the override hands the answer back to the hardware, at a PSRAM SP");
  stacktest::stackPointer() = stacktest::kDramBase + 8u;
  expect(memory::guardInternalFlash(kSite) == memory::FlashGuard::Allowed,
         "and at a DRAM SP once the override is cleared");

  // 7. A refusal is written down, which is the reason the guard takes a site code
  //    at all. A refusal normally keeps the device up, so nothing else would ever
  //    report it: a language pack that could not be read is an English UI, and a
  //    user cannot photograph a serial line.
  {
    const uint32_t before = countOf(kSite);
    stacktest::stackPointer() = stacktest::kPsramBase + 0x1000u;
    expect(memory::guardInternalFlash(kSite, 4096) == memory::FlashGuard::RefusedPsramStack,
           "a refusal still answers the question that was asked");
    expect(countOf(kSite) == before + 1, "the refusal is recorded in the fault ring");
    expect(recordedDetail(kSite) == 4096, "the site's number reaches the report");
    stacktest::stackPointer() = stacktest::kDramBase + 0x1000u;
    (void)memory::guardInternalFlash(kSite);
    expect(countOf(kSite) == before + 1, "an allowed decision records nothing");
    memory::debugOverrideFlashGuard(1);
    (void)memory::guardInternalFlash(kSite);
    expect(countOf(kSite) == before + 2, "a forced refusal records too, so CMD:CRASHTEST is not invisible");
    memory::debugOverrideFlashGuard(-1);
  }

  std::printf("stack safety host regression: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
