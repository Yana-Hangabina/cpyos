// Host regression for the fault ring: lib/Diagnostics/FaultLog.{h,cpp} and the code
// table lib/Diagnostics/FaultCodes.h.
//
// Compiled against the REAL source, not an extraction, because half of what this
// file gets wrong would only show up in the real translation unit (RTC layout, the
// constexpr table checks, the panic-path text scanning). The device branches are
// selected with -DSIMULATOR, which swaps the storage class and the clock; every
// branch of raise/copy/format is shared.
//
// Why these particular cases exist: the ring is the artifact a user photographs when
// the device dies, so its guarantees are support guarantees -- a code that dedupes
// wrongly reads as "this happened 400 times, calmly", a ring that fills up and
// refuses to record hides the very fault that matters, and a truncated tag on the
// screen decodes to less than nothing.
#include <FaultCodes.h>
#include <FaultLog.h>

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;
int checks = 0;

void expect(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

void expectEq(uint32_t got, uint32_t want, const char* what) {
  ++checks;
  if (got != want) {
    ++failures;
    std::printf("FAIL: %s (got %u, want %u)\n", what, got, want);
  }
}

void clear() {
  // The deliberate-reset branch is what a clean boot takes; reusing it here keeps
  // the test honest about the same code path the device uses.
  fault::onBoot(true);
}

fault::Slot first{};

uint8_t readOne() {
  fault::Slot slots[fault::kCapacity];
  const uint8_t n = fault::copy(slots, fault::kCapacity);
  if (n > 0) first = slots[0];
  return n;
}

void line(char* buffer, uint16_t capacity, uint32_t* lastSec = nullptr) {
  fault::formatLine(buffer, capacity, fault::kCapacity, lastSec);
}

// Every character of the printed line must belong to a whole entry:
// "STK-08x2 STK-05x11". A photograph of "STK-0" is not a shortened report, it is a
// code that does not exist, so a truncated tail has to be caught structurally --
// a digit test alone can be satisfied by pure luck (the truncation can land on a
// count digit, which is exactly what a truncated-tag mutation does).
bool wellFormed(const char* text) {
  if (text[0] == '\0') return true;  // nothing printed is a valid answer for nothing recorded
  for (;;) {
    const char* p = text;
    for (int i = 0; i < 3; ++i) {
      if (*p < 'A' || *p > 'Z') return false;
      ++p;
    }
    if (*p != '-') return false;
    ++p;
    for (int i = 0; i < 2; ++i) {
      if (*p < '0' || *p > '9') return false;
      ++p;
    }
    if (*p != 'x') return false;
    ++p;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      ++p;
      ++digits;
    }
    if (digits == 0) return false;
    if (*p == '\0') return true;
    if (*p != ' ') return false;
    text = p + 1;
    if (*text == '\0') return false;  // a dangling separator
  }
}

}  // namespace

int main() {
  using fault::Code;

  // --- the table itself -----------------------------------------------------------------
  // A relation, not a number: the table must cover every code that is not reserved,
  // tags must be exactly 6 visible characters, and each code's tag prefix must match
  // the domain range its value sits in. That is what lets a screenshot be decoded
  // without a tool, and it is what a duplicated or misranged entry would break.
  expect(fault::kTableSize > 40, "the table declares the whole crash-adjacent vocabulary");
  bool tableSane = true;
  uint16_t rangeMismatch = 0;
  for (uint16_t i = 0; i < fault::kTableSize; ++i) {
    const char* tag = fault::kTable[i].tag;
    if (std::strlen(tag) != 6) tableSane = false;
    const uint16_t value = static_cast<uint16_t>(fault::kTable[i].code);
    // The bands are the ones documented in FaultCodes.h's section comments; the
    // gate scripts/tests/test_fault_codes.py reads those comments directly, so a
    // disagreement between the two is a red gate rather than a silent drift.
    const char* want = nullptr;
    if (value <= 0x1F) want = "PNC";
    else if (value <= 0x41) want = "STK";
    else if (value <= 0x5F) want = "CBK";
    else if (value <= 0x7F) want = "TSK";
    else if (value <= 0x9F) want = "PNL";
    else if (value <= 0xBF) want = "STR";
    if (want != nullptr && std::strncmp(tag, want, 3) != 0) ++rangeMismatch;
  }
  expect(tableSane, "every tag is 6 characters (screen layout and photo legibility)");
  expectEq(rangeMismatch, 0, "every tag prefix matches its code's domain range");
  expect(fault::tagOf(Code(0x999))[0] == '\0', "an unlisted code has no tag");
  expect(std::strcmp(fault::whatOf(Code(0x999)), "unlisted fault code") == 0, "an unlisted code says so");

  // Positive and negative controls for the checker the truncation guarantee rests
  // on; without them "wellFormed" could reject nothing and the suite would be green.
  expect(wellFormed("STK-08x1 STK-05x2"), "the line checker accepts whole entries");
  expect(!wellFormed("STK-08x1 STK-0"), "the line checker rejects a half tag");
  expect(!wellFormed("STK-08x1 "), "and a dangling separator");

  // --- empty ---------------------------------------------------------------------------
  clear();
  expect(!fault::armed(), "a cleared ring is not armed");
  expectEq(readOne(), 0, "nothing to copy out of an empty ring");
  char buffer[128];
  line(buffer, sizeof(buffer));
  expect(buffer[0] == '\0', "an empty ring formats to an empty line, not a stray space");
  expectEq(fault::totalOccurrences(), 0, "no occurrences recorded");

  // --- one occurrence ------------------------------------------------------------------
  clear();
  fault::setClockForTest(10);
  fault::raise(Code::LanguagePackReadRefused);
  expect(fault::armed(), "one raise arms the ring");
  expectEq(readOne(), 1, "one slot occupied");
  expectEq(first.code, static_cast<uint32_t>(Code::LanguagePackReadRefused), "the code is stored");
  expectEq(first.count, 1, "counted once");
  expectEq(first.firstSec, 10, "first occurrence keeps its own timestamp");
  expectEq(first.lastSec, 10, "and the last matches while there is only one");
  line(buffer, sizeof(buffer));
  expect(std::strcmp(buffer, "STK-08x1") == 0, "the screen line is the tag and the count");

  // --- repeats do not multiply slots ---------------------------------------------------
  fault::setClockForTest(41);
  for (int i = 0; i < 20; ++i) fault::raise(Code::LanguagePackReadRefused);
  expectEq(readOne(), 1, "21 raises of one code still occupy one slot");
  expectEq(first.count, 21, "and count all of them");
  expectEq(first.firstSec, 10, "first timestamp is not overwritten by later hits");
  expectEq(first.lastSec, 41, "last timestamp follows the newest hit");
  expectEq(fault::totalOccurrences(), 21, "the total counts occurrences, not distinct codes");

  // --- detail ---------------------------------------------------------------------------
  clear();
  fault::raise(Code::RenderStackFallback, 40);  // pdMS_TO_TICKS-style number, arbitrary
  readOne();
  expectEq(first.detail, 40, "the raising site's number survives");
  expectEq(fault::formatDetail(buffer, sizeof(buffer), first), std::strlen(buffer),
           "formatDetail returns the length it wrote");
  expect(std::strstr(buffer, "TSK-01 x1") == buffer, "the report line starts with tag and count");
  expect(std::strstr(buffer, "PSRAM render stack refused") != nullptr, "the report line carries the meaning");

  // --- capacity: recycle, never stop recording ------------------------------------------
  clear();
  for (uint16_t value = 0x20; value <= 0x2F; ++value) {
    fault::setClockForTest(value);
    fault::raise(Code(value));
  }
  expectEq(readOne(), fault::kCapacity, "16 distinct codes fill the ring exactly");
  // One more distinct code: the least-recently-touched must go, and the new one must
  // be present. A ring that dropped the newcomer would hide exactly the fault that
  // is about to explain a crash.
  fault::setClockForTest(0x30);
  fault::raise(Code(0x30));
  fault::Slot slots[fault::kCapacity];
  const uint8_t n = fault::copy(slots, fault::kCapacity);
  expectEq(n, fault::kCapacity, "still full after an overflow");
  bool newcomerPresent = false;
  bool oldestGone = true;
  for (uint8_t i = 0; i < n; ++i) {
    if (slots[i].code == 0x30) newcomerPresent = true;
    if (slots[i].code == 0x20) oldestGone = false;  // 0x20 was touched first
  }
  expect(newcomerPresent, "the newest distinct code is recorded even when the ring was full");
  expect(oldestGone, "the least-recently-touched entry is the one evicted");
  expectEq(slots[0].code, 0x30, "ordering is newest-touched first");

  // --- ordering ---------------------------------------------------------------------------
  // Deliberately repeats the code that landed in the *second* slot: a copy that
  // walked the array by index instead of by touch stamp still answers "the first
  // thing recorded is the first line" here, which is the opposite of what the
  // screen promises.
  clear();
  fault::raise(Code::PinyinDbRefused);
  fault::raise(Code::PanicCacheStack);
  fault::raise(Code::PanicCacheStack);
  expectEq(readOne(), 2, "two distinct codes, two slots");
  expectEq(first.code, static_cast<uint32_t>(Code::PanicCacheStack), "a repeat re-orders by touch, not by insertion");

  // --- line budget: never a partial tag ---------------------------------------------------
  clear();
  for (uint16_t value = 0x20; value <= 0x2F; ++value) fault::raise(Code(value));
  char tight[24];
  const uint8_t fit = fault::formatLine(tight, sizeof(tight), fault::kCapacity, nullptr);
  expect(fit >= 2, "a tight buffer still records what it can");
  expect(wellFormed(tight), "a truncated line ends with a whole entry, never half a tag");
  char wide[fault::kLineCapacity];
  uint32_t lastSec = 0;
  const uint8_t all = fault::formatLine(wide, sizeof(wide), fault::kCapacity, &lastSec);
  expect(all == fault::kCapacity, "the documented line capacity holds the whole ring");
  expect(wellFormed(wide), "the full ring formats as whole entries");
  expect(lastSec != 0, "the newest entry's timestamp is reported");

  // --- maxItems ---------------------------------------------------------------------------
  const uint8_t two = fault::formatLine(wide, sizeof(wide), 2, nullptr);
  expectEq(two, 2, "maxItems bounds the entries printed (screen space is finite)");

  // --- panic classification ---------------------------------------------------------------
  struct Case {
    const char* message;
    Code want;
    const char* location;
    const char* label;
  };
  const Case cases[] = {
      {"assert failed: esp_cache_msync((void*)0), cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())",
       Code::PanicCacheStack, "cache_utils.c:127", "the 0.8.2 field crash"},
      {"assert failed: xTaskGetCurrentTaskHandle() != NULL queue.c:1632 (xQueueSemaphoreTake)", Code::PanicAssert,
       "queue.c:1632", "a generic assert keeps its location"},
      {"Stack canary watchpoint triggered (ActivityManagerRender)", Code::PanicStackOverflow, "", "stack overflow"},
      {"abort() was called at PC 0x423569f3 on core 1", Code::PanicAbortGeneric, "",
       "the other 0.8.2 report: terminate reached abort"},
      {"assert failed: rc != 0 at 1:27", Code::PanicAssert, "", "a colon that is not a file stays out of the field"},
  };
  for (const auto& c : cases) {
    clear();
    fault::notePanicAbort(c.message);
    expectEq(readOne(), 1, c.label);
    expectEq(first.code, static_cast<uint32_t>(c.want), c.label);
    expect(std::strcmp(fault::assertLocation(), c.location) == 0, "assert location extracted exactly (or not at all)");
  }

  clear();
  fault::notePanicAbort(nullptr);
  expectEq(readOne(), 1, "a null panic message still records something");
  expectEq(first.code, static_cast<uint32_t>(Code::PanicAbortGeneric), "and says it was unclassifiable");

  // --- C++ runtime causes ------------------------------------------------------------------
  struct CppCase {
    const char* name;
    Code want;
  };
  const CppCase cppCases[] = {
      {"St17bad_function_call", Code::PanicBadFunctionCall},
      {"St11bad_alloc", Code::PanicBadAlloc},
      {"operator new failed (bad_alloc)", Code::PanicBadAlloc},
      {"St12length_error", Code::PanicLengthError},
      {"St8bad_cast", Code::PanicOtherException},
      {nullptr, Code::PanicOtherException},
  };
  for (const auto& c : cppCases) {
    clear();
    fault::noteCppFailure(c.name);
    expectEq(readOne(), 1, "C++ failure classified");
    expectEq(first.code, static_cast<uint32_t>(c.want), "C++ failure maps to its own code");
  }

  // One death, one entry. The terminate handler raises and then calls abort(), whose
  // message would otherwise classify again as PNC-06 and make a single crash look
  // like two different ones.
  clear();
  fault::noteCppFailure("St17bad_function_call");
  fault::notePanicAbort("abort() was called at PC 0x423569f3 on core 1");
  expectEq(readOne(), 1, "the abort after a C++ failure does not add a second entry");
  expectEq(first.code, static_cast<uint32_t>(Code::PanicBadFunctionCall), "and the real cause is what stays");

  std::printf("fault ring host regression: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
