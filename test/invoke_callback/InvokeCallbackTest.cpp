// Host regression for lib/Callbacks/InvokeCallback.{h,cpp} -- the single owner of
// "calling an unset std::function must degrade, not abort".
//
// Field symptom this defends against: a screen reading only
//   abort() was called at PC 0x423569f3 on core 1
// with no backtrace.  0x423569f3 is `__cxxabiv1::__terminate` in the shipped
// 0.8.2 image: this build is -fno-exceptions, so libstdc++'s std::bad_function_call
// has no landing pad in any application frame, unwinds to std::terminate and
// abort() reboots the device.  A theme row formatter or a navigation callback
// that is legitimately unset (an empty list, a page shown before its host
// wired the callbacks, a board profile without an entry) therefore crashes
// instead of showing a blank column.
//
// Compiled against the real InvokeCallback.cpp with a Logging double that
// records lines (stubs/), so both halves of the contract are checked: the
// degradation and the visibility of the degradation.
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include <FaultLog.h>
#include <InvokeCallback.h>

#include "Logging.h"

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

void clearLog() { logstub::lines().clear(); }

int loggedFor(const std::string& needle) {
  int hits = 0;
  for (const auto& line : logstub::lines()) {
    if (line.find(needle) != std::string::npos) ++hits;
  }
  return hits;
}

// A formatter shaped exactly like the firmware ones: theme row titles are
// `std::function<const char*(int)>` and default to nullptr.
using RowTitle = std::function<const char*(int)>;

}  // namespace

// The ring the owner writes into, read back for the assertions in section 8/9.
uint32_t ringCount(fault::Code code) {
  fault::Slot slots[fault::kCapacity];
  const uint8_t count = fault::copy(slots, fault::kCapacity);
  for (uint8_t i = 0; i < count; ++i) {
    if (slots[i].code == static_cast<uint32_t>(code)) return slots[i].count;
  }
  return 0;
}

int main() {
  // 1. The crash itself: an unset formatter.  Before the guard this was the
  //    abort above; now it is a fallback value plus one visible error line.
  clearLog();
  RowTitle unset;
  const char* title = callbacks::callOr<const char*>("theme.rowTitle", unset, "fallback", 2);
  expect(title != nullptr && std::strcmp(title, "fallback") == 0, "unset callback returns the fallback");
  expect(loggedFor("empty callback skipped: theme.rowTitle") == 1, "the skipped callback is reported once");

  // 2. Not per frame.  A list repaints on every key press; the report is per
  //    boot, per site.
  for (int i = 0; i < 50; ++i) {
    title = callbacks::callOr<const char*>("theme.rowTitle", unset, "fallback", 2);
  }
  expect(title != nullptr && std::strcmp(title, "fallback") == 0, "the degraded value stays stable");
  expect(loggedFor("empty callback skipped: theme.rowTitle") == 1, "50 more repaints add no log lines");

  // 3. A different site is a different problem and gets its own line.
  clearLog();
  const std::function<void(int)> unsetVoid;
  callbacks::call("nav.press", unsetVoid, 3);
  callbacks::call("nav.press", unsetVoid, 4);
  callbacks::call("nav.release", unsetVoid, 5);
  expect(loggedFor("empty callback skipped: nav.press") == 1, "void callback reports its own tag once");
  expect(loggedFor("empty callback skipped: nav.release") == 1, "each call site is tracked separately");

  // 4. A set callback still runs, with its arguments, and its result is the
  //    value the caller sees.  Guarding must not quietly stop calling people.
  clearLog();
  int seenIndex = -1;
  int seenMultiplier = 0;
  const RowTitle set = [&](int index) -> const char* {
    seenIndex = index;
    return index == 2 ? "third" : "row";
  };
  title = callbacks::callOr<const char*>("theme.rowTitle", set, "fallback", 2);
  expect(seenIndex == 2, "the callback was invoked with the caller's argument");
  expect(title != nullptr && std::strcmp(title, "third") == 0, "the callback's value reaches the caller");
  expect(logstub::lines().empty(), "a working callback logs nothing");

  // 5. Arguments forward by value and by reference, and the void path invokes.
  const std::function<void(int, int)> adder = [&](int a, int b) { seenMultiplier = a + b; };
  seenMultiplier = 0;
  callbacks::call("nav.continuous", adder, 20, 22);
  expect(seenMultiplier == 42, "two arguments reach a void callback unchanged");

  std::string scratch = "mutated";
  const std::function<void(std::string&)> mutator = [](std::string& text) { text += "!"; };
  callbacks::call("row.value", mutator, scratch);
  expect(scratch == "mutated!", "a reference argument is not copied by the wrapper");

  // 6. Returning values are converted to the declared type, which is why Ret is
  //    written by hand: an int-returning lambda used for a long slot must not
  //    deduce the fallback to int.
  const std::function<int(int)> wide = [](int index) { return index * 2; };
  const long widened = callbacks::callOr<long>("row.tail", wide, -1L, 21);
  expect(widened == 42, "the result converts to the caller's declared type");
  const std::function<int(int)> wideEmpty;
  expect(callbacks::callOr<long>("row.tail.empty", wideEmpty, -1L, 21) == -1L,
         "the same declared type degrades to the fallback");

  // 7. Tag identity is by address, not by text: two sites that happen to carry
  //    the same characters are two different bugs, and each must speak up.
  clearLog();
  static const char siteA[] = "dup";
  static const char siteB[] = "dup";
  expect(+siteA != +siteB, "the fixture really has two addresses with identical text");
  expect(callbacks::noteEmptyOnce(siteA), "first sight of a tag reports");
  expect(!callbacks::noteEmptyOnce(siteA), "second sight of the same tag is silent");
  expect(callbacks::noteEmptyOnce(siteB), "the identically-named other site reports too");

  // 8. The skips are recorded in the fault ring, not only logged. A photograph of
  //    the crash screen cannot carry a serial line, so "a callback was missing"
  //    has to reach the ring -- counted per occurrence, with the site's table index
  //    as detail so crash_report.txt can name the sites. This links the real
  //    FaultLog.cpp, not a stand-in.
  {
    fault::Slot slots[fault::kCapacity];
    const uint8_t before = fault::copy(slots, fault::kCapacity);
    uint32_t emptyBefore = 0;
    for (uint8_t i = 0; i < before; ++i) {
      if (slots[i].code == static_cast<uint32_t>(fault::Code::CallbackEmpty)) emptyBefore = slots[i].count;
    }
    std::function<void(int)> missing;
    callbacks::call("ring.site-a", missing, 1);
    callbacks::call("ring.site-b", missing, 2);
    callbacks::call("ring.site-a", missing, 3);  // the same site again
    fault::Slot after[fault::kCapacity];
    const uint8_t recorded = fault::copy(after, fault::kCapacity);
    uint32_t emptyAfter = 0;
    uint32_t detail = callbacks::kUnknownSite;
    for (uint8_t i = 0; i < recorded; ++i) {
      if (after[i].code == static_cast<uint32_t>(fault::Code::CallbackEmpty)) {
        emptyAfter = after[i].count;
        detail = after[i].detail;
      }
    }
    expect(emptyBefore != 0, "the ring already counted the skips from the sections above");
    expect(emptyAfter - emptyBefore == 3, "every skip is counted, distinct sites included");
    expect(detail < callbacks::kKnownTags, "CB-01's detail is a site index inside the table");
    expect(callbacks::siteTagAt(0)[0] != '\0', "a recorded site resolves back to its tag");
    expect(callbacks::siteTagAt(callbacks::kKnownTags)[0] == '\0', "and an index past the table says so");
    expect(ringCount(fault::Code::CallbackTableFull) == 0, "a tabled site never claims the table is full");
  }

  // 9. The fixed table must bound, not overflow.  Nothing on the device can
  //    allocate here (this runs on the render task), so exceeding the table is
  //    expected to degrade to repeated reports -- never to a write past the end,
  //    and never to silence.  ASan/UBSan fail the run if the bound is removed.
  int reported = 0;
  reported += callbacks::noteEmptyOnce("saturation-0");
  reported += callbacks::noteEmptyOnce("saturation-1");
  reported += callbacks::noteEmptyOnce("saturation-2");
  reported += callbacks::noteEmptyOnce("saturation-3");
  reported += callbacks::noteEmptyOnce("saturation-4");
  reported += callbacks::noteEmptyOnce("saturation-5");
  reported += callbacks::noteEmptyOnce("saturation-6");
  reported += callbacks::noteEmptyOnce("saturation-7");
  reported += callbacks::noteEmptyOnce("saturation-8");
  reported += callbacks::noteEmptyOnce("saturation-9");
  reported += callbacks::noteEmptyOnce("saturation-10");
  reported += callbacks::noteEmptyOnce("saturation-11");
  reported += callbacks::noteEmptyOnce("saturation-12");
  reported += callbacks::noteEmptyOnce("saturation-13");
  reported += callbacks::noteEmptyOnce("saturation-14");
  reported += callbacks::noteEmptyOnce("saturation-15");
  reported += callbacks::noteEmptyOnce("saturation-16");
  reported += callbacks::noteEmptyOnce("saturation-17");
  reported += callbacks::noteEmptyOnce("saturation-18");
  reported += callbacks::noteEmptyOnce("saturation-19");
  expect(reported == 20, "every new tag reports at least once, table full or not");
  expect(!callbacks::noteEmptyOnce("theme.rowTitle"), "the first tag of the run is still deduped after the table filled");
  expect(!callbacks::noteEmptyOnce("saturation-1"), "a tag stored early is still deduped after the table filled");
  expect(callbacks::noteEmptyOnce("saturation-overflow-a"), "a new tag past the table capacity still reports");
  expect(callbacks::noteEmptyOnce("saturation-overflow-b"), "reporting does not stop once the table is full");
  // Saturation is itself a fault: the report ran out of room, so a later crash
  // screen cannot name which site was skipped. Recorded, not silently degraded.
  std::function<void(int)> beyond;
  callbacks::call("saturation-overflow-c", beyond, 1);
  expect(ringCount(fault::Code::CallbackTableFull) != 0, "a site beyond the table records TSK-07");

  std::printf("invoke callback host regression: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
