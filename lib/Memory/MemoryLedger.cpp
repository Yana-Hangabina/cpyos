#include "MemoryLedger.h"

#include <esp_heap_caps.h>
#include <esp_memory_utils.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

// The wrapped entry points. Only the symbols this build actually links with
// --wrap= may be defined: an unwrapped __wrap_malloc would still be reachable by
// anything that calls it by name, and __real_malloc would not exist. The wiring
// gate (scripts/tests/test_memory_ledger_wiring.py) keeps the flags and the
// macro from drifting apart, which is the difference between a measurement and
// an empty table that reads like an idle system.
#if defined(CROSSPOINT_MEMORY_LEDGER) && CROSSPOINT_MEMORY_LEDGER
extern "C" {
void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __real_calloc(size_t n, size_t m);
void* __real_realloc(void* ptr, size_t size);
void* __real_heap_caps_malloc(size_t size, uint32_t caps);
void* __real_heap_caps_calloc(size_t n, size_t m, uint32_t caps);
void* __real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void __real_heap_caps_free(void* ptr);
}
#endif

namespace {

// 16 bytes, kept outside the guard because baseline() answers for every build:
// an unwired image still has to be able to say what the heap looked like at rest.
memory::ledger::Watermark gBaseline;

#if defined(CROSSPOINT_MEMORY_LEDGER) && CROSSPOINT_MEMORY_LEDGER
// Slot fields are published through `ptr`, which is the only atomic in the row:
// a writer fills caller/bytes/region first, then stores the pointer with
// release, and a reader that matches the pointer loads the rest with acquire.
// No lock, because a lock here would put a mutex inside every allocator call,
// including the ones taken from ISRs and from inside the display feeder.
struct Slot {
  std::atomic<uintptr_t> ptr{0};
  uint32_t caller = 0;
  uint32_t bytes = 0;
  uint8_t region = 0;
};

// The table is 4,152 bytes of *static internal* RAM when it exists, which is the
// scarce region on every board here. It therefore lives behind the same macro as
// the wrappers: a build that cannot use the ledger must not pay for it either.
// It cannot move to PSRAM -- the allocator is called with the cache disabled
// during internal-flash writes, and a PSRAM table would vanish under the caller.
Slot gSlots[memory::ledger::kSlots];
std::atomic<uint32_t> gInternal{0};
std::atomic<uint32_t> gPsram{0};
std::atomic<uint32_t> gOther{0};
std::atomic<uint32_t> gBlocks{0};
std::atomic<uint32_t> gSteals{0};
std::atomic<uint32_t> gEarlySkips{0};
std::atomic<uint32_t> gUntrackedFrees{0};
std::atomic<uint32_t> gSmallBytes{0};
std::atomic<uint32_t> gSmallBlocks{0};
std::atomic<bool> gActive{false};

memory::ledger::Region classify(const void* ptr) {
  if (ptr == nullptr) return memory::ledger::Region::Other;
  return memory::ledger::regionOf(esp_ptr_external_ram(ptr), esp_ptr_internal(ptr));
}

void addBytes(const memory::ledger::Region region, const uint32_t bytes, const bool grew) {
  auto& counter = region == memory::ledger::Region::Internal
                      ? gInternal
                      : (region == memory::ledger::Region::Psram ? gPsram : gOther);
  if (grew) {
    counter.fetch_add(bytes, std::memory_order_relaxed);
    gBlocks.fetch_add(1, std::memory_order_relaxed);
  } else {
    counter.fetch_sub(bytes, std::memory_order_relaxed);
    gBlocks.fetch_sub(1, std::memory_order_relaxed);
  }
}

// Records one live block. `caller` is the return address of the allocating call,
// resolved to a function name offline against the ELF (exp/memledger_resolve.py):
// keeping it a raw address is what lets the ledger cover code that never heard of
// this module, including the IDF libraries.
void record(const void* ptr, const uint32_t bytes, const uint32_t caller) {
  if (ptr == nullptr) return;  // a failed allocation owns nothing; counting it would
                               // make the totals contradict the table
  if (!gActive.load(std::memory_order_relaxed)) {
    gEarlySkips.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto region = classify(ptr);
  auto& slot = gSlots[memory::ledger::slotFor(reinterpret_cast<uintptr_t>(ptr))];
  if (memory::ledger::insertAction(slot.ptr.load(std::memory_order_relaxed) != 0) ==
      memory::ledger::Action::Steal) {
    // Newest wins. The evicted block stays live but becomes untracked, and its
    // later free() is counted as untracked rather than silently subtracting
    // someone else's byte count.
    const uint32_t lost = slot.bytes;
    addBytes(static_cast<memory::ledger::Region>(slot.region), lost, false);
    gSteals.fetch_add(1, std::memory_order_relaxed);
  }
  slot.caller = caller;
  slot.bytes = bytes;
  slot.region = static_cast<uint8_t>(region);
  slot.ptr.store(reinterpret_cast<uintptr_t>(ptr), std::memory_order_release);
  addBytes(region, bytes, true);
}

// Sub-floor traffic is counted, never attributed: the byte total is what decides
// whether a gap is "the floor is too high" or "a path is not wrapped", and that is
// a question about volume, not about owners.
void noteSmall(const size_t bytes) {
  if (!gActive.load(std::memory_order_relaxed)) return;
  gSmallBytes.fetch_add(static_cast<uint32_t>(bytes), std::memory_order_relaxed);
  gSmallBlocks.fetch_add(1, std::memory_order_relaxed);
}

// The one entry point every wrapper calls. Keeping the floor decision here rather
// than at eight call sites is what stops one of them from drifting.
void note(const void* ptr, const size_t bytes, const uint32_t site) {
  if (ptr == nullptr || bytes == 0) return;
  if (memory::ledger::tracks(bytes)) {
    record(ptr, static_cast<uint32_t>(bytes), site);
  } else {
    noteSmall(bytes);
  }
}

void release(const void* ptr) {
  if (ptr == nullptr) return;
  if (!gActive.load(std::memory_order_relaxed)) return;
  auto& slot = gSlots[memory::ledger::slotFor(reinterpret_cast<uintptr_t>(ptr))];
  if (slot.ptr.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(ptr)) {
    // Either allocated before begin(), or by a path this build does not wrap, or
    // evicted. All three are reported by counters, none are guessed at.
    gUntrackedFrees.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t bytes = slot.bytes;
  const auto region = static_cast<memory::ledger::Region>(slot.region);
  slot.ptr.store(0, std::memory_order_release);
  addBytes(region, bytes, false);
}

#endif  // CROSSPOINT_MEMORY_LEDGER
}  // namespace

namespace memory {
namespace ledger {

bool wired() {
#if defined(CROSSPOINT_MEMORY_LEDGER) && CROSSPOINT_MEMORY_LEDGER
  return true;
#else
  return false;
#endif
}

void begin() {
#if defined(CROSSPOINT_MEMORY_LEDGER) && CROSSPOINT_MEMORY_LEDGER
  for (auto& slot : gSlots) slot.ptr.store(0, std::memory_order_relaxed);
  gInternal = 0;
  gPsram = 0;
  gOther = 0;
  gBlocks = 0;
  gSteals = 0;
  gEarlySkips = 0;
  gUntrackedFrees = 0;
  gBaseline.internalFree = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  gBaseline.internalLargest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  gBaseline.psramFree = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  gBaseline.psramLargest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  gActive.store(true, std::memory_order_relaxed);
#endif
}

Counters counters() {
  Counters c;
#if !defined(CROSSPOINT_MEMORY_LEDGER) || !CROSSPOINT_MEMORY_LEDGER
  return c;
#else
  c.internalBytes = gInternal.load(std::memory_order_relaxed);
  c.psramBytes = gPsram.load(std::memory_order_relaxed);
  c.otherBytes = gOther.load(std::memory_order_relaxed);
  c.blocks = gBlocks.load(std::memory_order_relaxed);
  c.steals = gSteals.load(std::memory_order_relaxed);
  c.earlySkips = gEarlySkips.load(std::memory_order_relaxed);
  c.untrackedFrees = gUntrackedFrees.load(std::memory_order_relaxed);
  c.smallAllocBytes = gSmallBytes.load(std::memory_order_relaxed);
  c.smallAllocBlocks = gSmallBlocks.load(std::memory_order_relaxed);
  return c;
#endif
}

Watermark baseline() { return gBaseline; }

Watermark measured() {
  Watermark w;
  w.internalFree = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  w.internalLargest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  w.psramFree = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  w.psramLargest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  return w;
}

CrossCheck check(const uint32_t toleranceBytes) {
  return crossCheck(wired(), gBaseline, measured(), counters(), toleranceBytes);
}

uint32_t attributed(const Region region) {
#if !defined(CROSSPOINT_MEMORY_LEDGER) || !CROSSPOINT_MEMORY_LEDGER
  (void)region;
  return 0;
#else
  switch (region) {
    case Region::Internal:
      return gInternal.load(std::memory_order_relaxed);
    case Region::Psram:
      return gPsram.load(std::memory_order_relaxed);
    case Region::Other:
      return gOther.load(std::memory_order_relaxed);
  }
  return 0;
#endif
}

size_t snapshot(Live* out, const size_t cap) {
#if !defined(CROSSPOINT_MEMORY_LEDGER) || !CROSSPOINT_MEMORY_LEDGER
  (void)out;
  (void)cap;
  return 0;
#else
  size_t written = 0;
  for (auto& slot : gSlots) {
    if (slot.ptr.load(std::memory_order_acquire) == 0) continue;
    size_t hit = cap;
    for (size_t i = 0; i < written; ++i) {
      if (out[i].caller == slot.caller) {
        out[i].bytes += slot.bytes;
        out[i].blocks += 1;
        hit = i;
        break;
      }
    }
    if (hit != cap) continue;
    if (written >= cap) {
      // Drop the lightest row and re-insert, so a full table still reports the
      // heaviest callers rather than whichever slots hashed first.
      size_t lightest = 0;
      for (size_t i = 1; i < written; ++i) {
        if (out[i].bytes < out[lightest].bytes) lightest = i;
      }
      if (slot.bytes <= out[lightest].bytes) continue;
      out[lightest] = {slot.caller, slot.bytes, 1};
      continue;
    }
    out[written++] = {slot.caller, slot.bytes, 1};
  }
  for (size_t i = 1; i < written; ++i) {
    const Live key = out[i];
    size_t j = i;
    while (j > 0 && out[j - 1].bytes < key.bytes) {
      out[j] = out[j - 1];
      --j;
    }
    out[j] = key;
  }
  return written;
#endif
}

}  // namespace ledger
}  // namespace memory

#if defined(CROSSPOINT_MEMORY_LEDGER) && CROSSPOINT_MEMORY_LEDGER
// Evaluated in the wrapper's own frame, so it is the address the *caller of the
// allocator* returns to. Reading it inside a helper instead would record the
// wrapper's own address: the first device dump after note() was introduced did
// exactly that, and every site resolved to __wrap_heap_caps_malloc+0x16 -- a
// table full of numbers that attribute nothing. It has to be a macro at each
// call site, not a function, because whether the helper gets inlined is the
// optimiser's decision and the address must not depend on it.
#define LEDGER_SITE (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))))

extern "C" {

void* __wrap_malloc(const size_t size) {
  void* ptr = __real_malloc(size);
  note(ptr, size, LEDGER_SITE);
  return ptr;
}

void __wrap_free(void* ptr) {
  release(ptr);
  __real_free(ptr);
}

void* __wrap_calloc(const size_t n, const size_t m) {
  void* ptr = __real_calloc(n, m);
  // Overflow in n*m would already have failed the allocation; guard the multiply
  // anyway because the recorded size is what every later subtraction uses.
  note(ptr, n != 0 && m <= SIZE_MAX / n ? n * m : 0, LEDGER_SITE);
  return ptr;
}

void* __wrap_realloc(void* ptr, const size_t size) {
  const uint32_t site = LEDGER_SITE;
  const bool hadSlot = ptr != nullptr;
  void* moved = __real_realloc(ptr, size);
  if (moved == nullptr) return moved;
  if (moved != ptr) {
    if (hadSlot) release(ptr);
    // A grow that lands elsewhere is attributed to whoever called realloc, not to
    // the original owner. Known blur, and it only ever moves a block that was
    // already big enough to be interesting.
    note(moved, size, site);
  } else if (memory::ledger::tracks(size)) {
    release(ptr);
    record(moved, static_cast<uint32_t>(size), site);
  }
  return moved;
}

void* __wrap_heap_caps_malloc(const size_t size, const uint32_t caps) {
  void* ptr = __real_heap_caps_malloc(size, caps);
  note(ptr, size, LEDGER_SITE);
  return ptr;
}

void* __wrap_heap_caps_calloc(const size_t n, const size_t m, const uint32_t caps) {
  void* ptr = __real_heap_caps_calloc(n, m, caps);
  note(ptr, n != 0 && m <= SIZE_MAX / n ? n * m : 0, LEDGER_SITE);
  return ptr;
}

void* __wrap_heap_caps_aligned_alloc(const size_t alignment, const size_t size, const uint32_t caps) {
  void* ptr = __real_heap_caps_aligned_alloc(alignment, size, caps);
  note(ptr, size, LEDGER_SITE);
  return ptr;
}

void __wrap_heap_caps_free(void* ptr) {
  release(ptr);
  __real_heap_caps_free(ptr);
}

}  // extern "C"
#endif
