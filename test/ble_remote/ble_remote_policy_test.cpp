// Host check for the BLE reader radio policy (src/BleRemotePolicy.h):
//   - a fresh boot (bond list not read yet) must still allow the reader radio,
//     otherwise an already-paired remote could never be reached again;
//   - once the host has reported the bond list, the radio is only allowed for a
//     paired remote (upstream d5b6c74d: no remote chosen => keep it off);
//   - a remote forgotten while the host is up flips the answer back immediately;
//   - a bonded but absent remote must not keep the radio up forever: 30 000 ms
//     without a link or an attempt in flight stops it (upstream fecee610), and the
//     boundary is exactly "expires at the limit";
//   - the reader start retry budget is bounded (5 s apart, six per book visit,
//     reset by a success or a new visit), so a refused start stops asking.
//
// Run: bash test/ble_remote/run_ble_remote_policy_test.sh

#include <BleRemotePolicy.h>

#include <cstdio>

namespace {
int failures = 0;

void check(const bool condition, const char* what) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

// Mirrors src/main.cpp's reader retry accounting, using the production budget.

}  // namespace

int main() {
  using bleinput::BondState;
  using bleinput::readerRadioAllowedFor;
  using bleinput::radioIdleExpired;

  const BondState freshBoot{};
  check(!freshBoot.known && !freshBoot.hasRemote, "default state is 'not read yet'");
  check(readerRadioAllowedFor(freshBoot), "fresh boot: radio allowed so the bond list can be read");

  const BondState noRemote{true, false};
  check(!readerRadioAllowedFor(noRemote), "known, no remote: reader keeps the radio off");

  const BondState paired{true, true};
  check(readerRadioAllowedFor(paired), "known, remote paired: reader keeps the radio up");

  // Settings page freshly unpaired a remote mid-session: the answer must follow.
  check(!readerRadioAllowedFor(BondState{true, false}), "remote forgotten mid-session: radio off");
  check(readerRadioAllowedFor(BondState{true, true}), "remote paired mid-session: radio up");

  // --- idle-off boundary (kIdleOffMs = 30 000) --------------------------------
  check(!radioIdleExpired(false, false, 0), "just started: not idle");
  check(!radioIdleExpired(false, false, 29'999), "29 999 ms: still allowed");
  check(radioIdleExpired(false, false, 30'000), "exactly 30 000 ms: stop");
  check(radioIdleExpired(false, false, 30'001), "past the limit: stop");
  check(!radioIdleExpired(true, false, 3'600'000), "linked remote quiet for an hour: never idle");
  check(!radioIdleExpired(false, true, 30'000), "scan/connect in flight at the boundary: keep it up");
  check(!radioIdleExpired(true, true, 3'600'000), "linked and busy: keep it up");
  check(!radioIdleExpired(false, false, 999, 1000), "custom limit: 999 of 1 000 ms");
  check(radioIdleExpired(false, false, 1000, 1000), "custom limit: exactly 1 000 of 1 000 ms");

  // --- bounded reader start retries -------------------------------------------
  using bleinput::ReaderStartBudget;
  ReaderStartBudget budget;
  uint32_t now = 1000;
  check(budget.allowed(now), "first attempt allowed");
  for (uint8_t attempt = 0; attempt < bleinput::kReaderStartRetryLimit; ++attempt) {
    check(budget.allowed(now), "attempt within the budget is allowed");
    budget.refusedAt(now);
    check(!budget.allowed(now + bleinput::kReaderStartRetryIntervalMs - 1), "next attempt waits the full 5 s");
    now += bleinput::kReaderStartRetryIntervalMs;
  }
  check(!budget.allowed(now), "after six refusals this visit stops asking");
  check(!budget.allowed(now + 600'000), "and it stays quiet for the rest of the visit");
  budget.reset();
  check(budget.allowed(now), "a new book visit gets a fresh budget");

  budget.refusedAt(now);
  budget.reset();
  check(budget.allowed(now), "a success clears the budget immediately");

  // Wrap-safe deadline: a visit kept open across the 32-bit rollover must not park
  // the next attempt 49 days away (or consider it due instantly).
  ReaderStartBudget wrapped;
  wrapped.refusedAt(0xFFFFFF00u);
  check(!wrapped.allowed(0xFFFFFF01u), "just after a rollover the 5 s wait still applies");
  check(wrapped.allowed(0x00001300u), "and it elapses normally across the rollover");

  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("ble reader radio policy: bond grace, idle-off boundary, bounded start retries\n");
  return 0;
}
