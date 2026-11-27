#pragma once

#include <FaultCodes.h>

#include <cstdint>

// One owner for one invariant: an operation that makes SPI-flash erase/write
// (or any access that disables the cache) must not run on a task whose stack
// lives in PSRAM. Disabling the cache invalidates the caller's own stack, and
// IDF catches it with `esp_task_stack_is_sane_cache_disabled()` in
// `spi_flash_disable_interrupts_caches_and_other_cpu()` -- a panic and reboot,
// not an error return.
//
// Why a shared owner instead of a per-site `esp_ptr_external_ram()` check: the
// sites are many (font carriers, language packs, pinyin, hyphenation, web
// assets, OTA slots, the PDF engine, device secret) and the failure is
// silent-until-it-crashes, so a forgotten seventh site is a field crash. The
// same rule already exists ad hoc in `HalOtaSlot`, `HyphenationStore`,
// `DeviceSecret` and `system_core_font::installResident()`; those now call here.
//
// The guard answers a *capability* question, never a *content* question: on a
// PSRAM stack it says "you may not find out", and the caller must report
// "unknown" rather than "absent". Treating a refused read as missing data is
// how a working language pack or font carrier silently becomes tofu/English.
namespace memory {

enum class FlashGuard : uint8_t {
  Allowed,         // the caller's stack is in internal RAM: proceed
  RefusedPsramStack,  // the caller's stack is in PSRAM: do not touch internal flash
};

// The decision itself. Cheap (two comparisons) and allocation-free.
//
// `site` is mandatory and has no default, on purpose: a refusal is invisible
// except on serial, and serial is exactly what a field reporter cannot provide.
// Naming the site here is what makes "the guard said no" become a recorded fault
// code, and it makes a new flash site that forgot the question impossible to add
// quietly. See lib/Diagnostics/FaultCodes.h and scripts/tests/test_fault_codes.py.
//
// `detail` is whatever number the site has that a code alone cannot carry -- a
// byte count, a partition offset -- and it lands in crash_report.txt only.
FlashGuard guardInternalFlash(fault::Code site, uint32_t detail = 0);

// Convenience for the common `if (!memory::internalFlashAllowed(fault::Code::X)) { ... }`.
inline bool internalFlashAllowed(fault::Code site, uint32_t detail = 0) {
  return guardInternalFlash(site, detail) == FlashGuard::Allowed;
}

// Bench/test seam: force the answer so both branches are reachable from the
// serial console and from host tests. `value < 0` clears the override and
// restores the real decision. Not a runtime knob for product code paths.
void debugOverrideFlashGuard(int value);

}  // namespace memory
