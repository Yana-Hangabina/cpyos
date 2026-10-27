#pragma once

#include <Memory.h>

#include <cstddef>
#include <memory>
#include <string>

// Detect esp_heap_caps.h here, in this header, rather than reusing
// CROSSPOINT_MEMORY_HAS_HEAP_CAPS from <Memory.h>.
//
// That macro looks public and is not: Memory.h defines it at line 13 and #undefs it at
// line 110, before the file ends, so it is never visible to an includer. This header
// included <Memory.h> and then tested the macro anyway, the test was always false, and
// every allocation below silently took the `::operator new` branch -- internal RAM.
//
// The consequence was that nothing in this app was ever in PSRAM. chapters_, shelf_,
// search_, books_, rows_, rowIds_ and PsramString all believed they were PSRAM-backed and
// all landed in the ~87 KB of internal RAM, next to the Wi-Fi stack, the TLS handshake and
// the job task's stack. Loading the shelf alone cost 58 KB of it (135 entries x 432 B),
// which was measured on the device and then misattributed to the vector buffers. Every
// symptom since -- a job task that could not be created for want of 12,288 contiguous
// bytes, a TLS handshake stalling at 3 KB free and taking DNS down with it, downloads
// reporting "网络错误" -- was this one silent fallback.
//
// A guard that fails silently toward the wrong allocator is the expensive kind, so the
// condition is now spelled out where it is used and cannot be undefined by another file.
#if defined(BOARD_HAS_PSRAM) && defined(__has_include)
#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define JJWXC_ALLOCATOR_HAS_HEAP_CAPS 1
#endif
#endif

// Fail the build rather than fall back quietly. A board that declares PSRAM but cannot
// reach esp_heap_caps.h means every container below would allocate from the ~87 KB of
// internal RAM while every reader of the code believes it is in PSRAM -- which is exactly
// what happened, and it cost four rounds of misdiagnosed "out of memory" and "network
// error" reports to find. There is no correct silent answer here, so there is no silence.
#if defined(BOARD_HAS_PSRAM) && !defined(JJWXC_ALLOCATOR_HAS_HEAP_CAPS)
#error "BOARD_HAS_PSRAM is set but <esp_heap_caps.h> was not found; PsramAllocator would silently use internal RAM"
#endif

namespace jjwxc {

// STL allocator that places a container's storage in PSRAM.
//
// The catalogue containers are the reason this exists: a 1500-chapter list of
// 144-byte ChapterEntry is 216 KB, and internal RAM has only ~106 KB free after
// boot (with 45 KB of it needing to stay contiguous for display DMA). Under
// -fno-exceptions a failed allocation is not a null return but abort(), so an
// ordinary std::vector here turns a long novel into a device reboot.
//
// A plain std::vector<std::string, PsramAllocator<...>> is only half a fix: the
// vector buffer moves but every string still allocates its own characters in
// internal RAM. Use PsramString below for those.
template <typename T>
struct PsramAllocator {
  using value_type = T;

  PsramAllocator() = default;
  template <typename U>
  PsramAllocator(const PsramAllocator<U>&) noexcept {}

  T* allocate(const size_t n) {
#ifdef JJWXC_ALLOCATOR_HAS_HEAP_CAPS
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    void* p = ::operator new(n * sizeof(T), std::nothrow);
#endif
    // allocate() is not allowed to return null: std::vector would call
    // std::__throw_bad_alloc, which aborts under -fno-exceptions anyway. Callers
    // must gate growth on psramHasHeadroom() first so this stays unreachable.
    if (!p) return nullptr;
    return static_cast<T*>(p);
  }

  void deallocate(T* p, size_t) noexcept {
#ifdef JJWXC_ALLOCATOR_HAS_HEAP_CAPS
    heap_caps_free(p);
#else
    ::operator delete(p);
#endif
  }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept {
  return true;
}
template <typename T, typename U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept {
  return false;
}

// std::string whose character buffer lives in PSRAM.
//
// This exists because of a failure measured on the device, not on principle. A
// 190-chapter list rebuilds into ~380 short-lived std::string heap blocks in
// internal RAM. Total free internal RAM was still 25,815 bytes afterwards, but
// the LARGEST contiguous block had been cut down to 12,276 -- and the job task
// needs 12,288 for its stack, so it missed by twelve bytes:
//
//   [1718744] wolfSSL TLS closed: free=25815 largest=12276
//   [1718745] job task stack (12288 B) not available; releasing the session and retrying once
//   [1718745] xTaskCreatePinnedToCore failed twice; internal RAM is exhausted
//
// When the stack did fit, it consumed that block outright and the TLS handshake
// then starved instead -- wolfSSL connected fine at largest=17396 and failed at
// largest=7156, after 62 seconds, which the UI could only report as "网络错误":
//
//   [1740909] wolfSSL TLS new: host=m.jjwxc.net free=12067 largest=7156
//   [1802983] [ERR] [HTTP] wolfSSL request connect failed
//
// So the bug was never the strings' size, it was their COUNT: hundreds of small
// allocations shatter the heap into blocks too small for either a task stack or
// a TLS handshake. Keeping them out of internal RAM keeps it in one piece.
using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

}  // namespace jjwxc
