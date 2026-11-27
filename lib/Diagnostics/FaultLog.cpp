#include "FaultLog.h"

#include <FaultCodes.h>

#include <cstdio>  // snprintf: used by the formatters on both sides, and the
                   // Arduino/xtensa precompiled core does not pull it in
                   // indirectly the way the ESP-IDF headers do.

#if !defined(SIMULATOR)
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_timer.h>
#define FAULT_RTC RTC_NOINIT_ATTR
#else
// The host build (test/record_faults) has no RTC domain and no panic path: plain
// statics and a clock the test sets. The layout and every branch of raise/copy/
// format are the same code, which is the point of compiling this file twice.
#define FAULT_RTC static
#include <cstring>
#endif

namespace fault {
namespace {

static_assert(kCapacity <= 32, "the selection mask in copy() is a uint32_t");

constexpr uint32_t kMagic = 0x46415531u;  // 'FAU1' -- a fault ring, not a stale RTC hole

// The magic is separate from the slots so `armed()` can tell "this session really
// recorded something" from "RTC_NOINIT happened to contain this bit pattern".
FAULT_RTC uint32_t gMagic = 0;
FAULT_RTC uint32_t gSeq = 0;
FAULT_RTC uint32_t gTotal = 0;
FAULT_RTC Slot gSlots[kCapacity];
// Survives the reboot, because CrashActivity only runs after it: the assert's
// `file:line` is the most useful token a screenshot can carry.
FAULT_RTC char gAssertLocation[32] = "";
FAULT_RTC bool gCppFailureNoted = false;
#if !defined(SIMULATOR)
FAULT_RTC bool gBootNoted = false;
#endif

#if defined(SIMULATOR)
uint32_t gClockForTest = 0;
uint32_t nowSeconds() { return gClockForTest; }
#else
uint32_t nowSeconds() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000LL); }
#endif

// Bounded ASCII substring search, spelled out rather than strstr: this runs in the
// panic path where a libc call is the last thing worth trusting.
bool contains(const char* haystack, const char* needle) {
  if (haystack == nullptr || needle == nullptr) return false;
  for (uint16_t i = 0; haystack[i] != '\0'; ++i) {
    uint16_t j = 0;
    while (needle[j] != '\0' && haystack[i + j] == needle[j]) ++j;
    if (needle[j] == '\0') return true;
  }
  return false;
}

// IDF prints `assert failed: <expression> cache_utils.c:127 (function)`, and the
// expression can be long, so the location is the LAST colon in a bounded scan --
// reading only the first 31 characters would find the colon after "assert failed"
// and dress the word "failed" up as a file name.
void captureAssertLocation(const char* message) {
  gAssertLocation[0] = '\0';
  // 192 characters is longer than any assert message seen in the field; stopping
  // early can only mean "no location", never a wrong one.
  uint16_t lastColon = 0;
  bool found = false;
  for (uint16_t index = 0; index < 192 && message[index] != '\0'; ++index) {
    if (message[index] == ':') {
      lastColon = index;
      found = true;
    }
  }
  if (!found || lastColon < 2) return;
  // Read backwards from the colon to the start of the file token, then reverse.
  uint16_t out = 0;
  char scratch[32];
  for (int16_t i = static_cast<int16_t>(lastColon) - 1; i >= 0 && out < sizeof(scratch) - 1; --i) {
    const char c = message[i];
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                      c == '.' || c == '/' || c == ':';
    if (!keep) break;
    scratch[out++] = c;
  }
  // The token must end in `.c`, `.cpp` or `.h` to be a location at all; anything
  // else (a hex value, `1:27` inside parentheses) stays in the message rather than
  // being dressed up as a source line.
  bool looksLikeFile = out >= 3 && scratch[0] != '.' && scratch[0] != '_';
  if (looksLikeFile) {
    // Find the extension dot in the reversed token: it has to be the second or
    // third character back, and the character(s) before it must be c/p/h.
    uint16_t dot = 0;
    while (dot < out && scratch[dot] != '.') ++dot;
    looksLikeFile = dot >= 1 && dot <= 3 && (scratch[0] == 'c' || scratch[0] == 'p' || scratch[0] == 'h');
  }
  if (!looksLikeFile) return;
  // 24 characters of path, the separator the backwards scan consumed, and up to 6
  // digits fit in the field exactly. Without the separator the two halves merge
  // into "queue.c1632", which neither a tool nor a reader can split.
  uint16_t written = 0;
  for (int16_t k = static_cast<int16_t>(out) - 1; k >= 0 && written < sizeof(gAssertLocation) - 8; --k) {
    gAssertLocation[written++] = scratch[k];
  }
  gAssertLocation[written++] = ':';
  for (uint16_t k = static_cast<uint16_t>(lastColon) + 1; k < 192 && written < sizeof(gAssertLocation) - 1; ++k) {
    if (message[k] < '0' || message[k] > '9') break;
    gAssertLocation[written++] = message[k];
  }
  gAssertLocation[written] = '\0';
}

void reset() {
  gMagic = 0;
  gSeq = 0;
  gTotal = 0;
  gCppFailureNoted = false;
  gAssertLocation[0] = '\0';
  for (uint8_t i = 0; i < kCapacity; ++i) gSlots[i] = Slot{};
}

uint8_t findOrClaim(const uint32_t value, bool& fresh) {
  fresh = false;
  for (uint8_t i = 0; i < kCapacity; ++i) {
    if (gSlots[i].code == value) return i;
  }
  uint8_t victim = 0;
  uint32_t oldest = UINT32_MAX;
  for (uint8_t i = 0; i < kCapacity; ++i) {
    if (gSlots[i].code == 0) {
      fresh = true;
      return i;
    }
    // Recycling the least-recently-touched entry, rather than refusing to record.
    // A ring that silently stops at 16 distinct codes is the failure this file
    // exists to prevent, and the oldest entry is the one least likely to explain a
    // crash that is happening now.
    if (gSlots[i].seq < oldest) {
      oldest = gSlots[i].seq;
      victim = i;
    }
  }
  fresh = true;
  return victim;
}

}  // namespace

void raise(const Code code, const uint32_t detail) {
  const uint32_t value = static_cast<uint32_t>(code);
  bool fresh = false;
  const uint8_t slot = findOrClaim(value, fresh);
  const uint32_t now = nowSeconds();
  // Sequence matters for a reboot landing in the middle: the code is written
  // before the count, so a half-recorded slot reads as an absent one rather than
  // as an entry whose numbers disagree.
  if (fresh) {
    gSlots[slot].firstSec = now;
    gSlots[slot].code = value;
    gSlots[slot].count = 1;
  } else {
    __atomic_fetch_add(&gSlots[slot].count, 1u, __ATOMIC_RELAXED);
  }
  gSlots[slot].lastSec = now;
  gSlots[slot].detail = detail;
  gSlots[slot].seq = ++gSeq;
  __atomic_fetch_add(&gTotal, 1u, __ATOMIC_RELAXED);
  gMagic = kMagic;
}

uint8_t copy(Slot* out, const uint8_t max) {
  if (out == nullptr || gMagic != kMagic) return 0;
  const uint8_t want = max < kCapacity ? max : kCapacity;
  uint8_t written = 0;
  uint32_t taken = 0;
  while (written < want) {
    // Highest untouched seq. With 16 entries a selection pass beats both a sort
    // and a second array, and it needs no scratch memory.
    int8_t best = -1;
    uint32_t bestSeq = 0;
    for (uint8_t i = 0; i < kCapacity; ++i) {
      if (gSlots[i].code == 0 || (taken & (1u << i)) != 0) continue;
      if (best < 0 || gSlots[i].seq > bestSeq) {
        best = static_cast<int8_t>(i);
        bestSeq = gSlots[i].seq;
      }
    }
    if (best < 0) break;
    taken |= 1u << best;
    out[written++] = gSlots[best];
  }
  return written;
}

bool armed() { return gMagic == kMagic && gTotal != 0; }

uint16_t totalOccurrences() { return static_cast<uint16_t>(gTotal > 0xFFFFu ? 0xFFFFu : gTotal); }

void onBoot(const bool deliberateReset) {
#if !defined(SIMULATOR)
  if (gBootNoted) return;  // idempotent: a second call must not count the same death twice
#endif
  if (deliberateReset) {
    // A restart the user or a flashing tool asked for ends the previous session's
    // evidence. Keeping it would let an old fault explain a reboot that had
    // nothing to do with it, which is worse than showing nothing.
    reset();
  } else if (gMagic != kMagic) {
    // Nothing recorded, or RTC_NOINIT is random on a cold power-on: start clean so
    // `armed()` can never be satisfied by garbage.
    reset();
  }
#if !defined(SIMULATOR)
  gBootNoted = true;
  // A watchdog or brown-out never reaches a panic hook, so this is the only place
  // those two deaths can be named at all. esp_reset_reason() reads the cause the
  // reset itself wrote, so the record is made after the fact but is not a guess.
  const esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT) {
    raise(Code::WatchdogReset);
  } else if (reason == ESP_RST_BROWNOUT) {
    raise(Code::BrownoutReset);
  }
#endif
}

void notePanicAbort(const char* message) {
  // One death, one location: a previous crash's file:line must not be printed
  // against this record.
  gAssertLocation[0] = '\0';
  if (message == nullptr) {
    if (!gCppFailureNoted) raise(Code::PanicAbortGeneric);
    return;
  }
  // The C++ handlers know the real cause and already recorded it; re-classifying
  // their abort() would add a second, vaguer entry for one death.
  if (gCppFailureNoted) return;
  // The location is captured independently of the classification below, because the
  // crash this whole file exists for -- 0.8.2's `cache_utils.c:127` -- is recognised
  // by its *function* name, not by the word "assert", and its file:line is the most
  // useful token the message carries.
  const bool wasAssert = contains(message, "assert failed") || contains(message, "Assertion failed");
  if (wasAssert) captureAssertLocation(message);
  if (contains(message, "esp_task_stack_is_sane_cache_disabled") || contains(message, "cache_utils.c")) {
    raise(Code::PanicCacheStack);
  } else if (contains(message, "Stack canary watchpoint")) {
    raise(Code::PanicStackOverflow);
  } else if (wasAssert) {
    raise(Code::PanicAssert);
  } else if (contains(message, "abort() was called")) {
    raise(Code::PanicAbortGeneric);
  } else {
    // IDF also arrives here with a bare `Guru Meditation`-style reason. It is an
    // assert-shaped death with a message we cannot classify, so keep the text as
    // the evidence and say so, rather than dropping the entry.
    raise(Code::PanicAssert);
  }
}

void noteCppFailure(const char* exceptionName) {
  gCppFailureNoted = true;
  if (exceptionName == nullptr) {
    raise(Code::PanicOtherException);
    return;
  }
  // Matched on the distinctive substring, not the whole mangled symbol, so a
  // toolchain rename cannot silently lose a class.
  if (contains(exceptionName, "bad_function_call")) {
    raise(Code::PanicBadFunctionCall);
  } else if (contains(exceptionName, "bad_alloc") || contains(exceptionName, "operator new failed")) {
    raise(Code::PanicBadAlloc);
  } else if (contains(exceptionName, "length_error")) {
    raise(Code::PanicLengthError);
  } else {
    raise(Code::PanicOtherException);
  }
}

const char* assertLocation() { return gAssertLocation; }

uint8_t formatLine(char* line, const uint16_t capacity, const uint8_t maxItems, uint32_t* lastSecondOut) {
  if (line == nullptr || capacity < 8) return 0;
  Slot slots[kCapacity];
  const uint8_t found = copy(slots, kCapacity);
  const uint8_t want = found < maxItems ? found : maxItems;
  uint16_t used = 0;
  line[0] = '\0';
  uint8_t written = 0;
  for (uint8_t i = 0; i < want; ++i) {
    const char* tag = tagOf(static_cast<Code>(slots[i].code));
    if (tag[0] == '\0') continue;  // an unlisted code is the gate's problem, not a crash
    char piece[16];
    const int length = snprintf(piece, sizeof(piece), i == 0 ? "%sx%u" : " %sx%u", tag,
                                static_cast<unsigned>(slots[i].count));
    if (length <= 0) continue;
    // Stop before a partial tag. Half of "STK-0" on a photograph is worse than one
    // fewer entry, because it looks like a code that does not exist.
    if (used + static_cast<uint16_t>(length) + 1 > capacity) break;
    for (int k = 0; k < length; ++k) line[used++] = piece[k];
    line[used] = '\0';
    if (lastSecondOut != nullptr && written == 0) *lastSecondOut = slots[i].lastSec;
    ++written;
  }
  return written;
}

uint16_t formatDetail(char* out, const uint16_t capacity, const Slot& slot) {
  if (out == nullptr || capacity < 16) return 0;
  const char* tag = tagOf(static_cast<Code>(slot.code));
  const char* what = whatOf(static_cast<Code>(slot.code));
  const int length = snprintf(out, capacity, "%s x%u  last %us  %s", tag, static_cast<unsigned>(slot.count),
                              static_cast<unsigned>(slot.lastSec), what);
  if (length < 0) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<uint16_t>(length);
}

#if defined(SIMULATOR)
void setClockForTest(const uint32_t seconds) { gClockForTest = seconds; }
#endif

}  // namespace fault
