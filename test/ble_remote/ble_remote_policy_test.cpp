// Host check for the BLE reader radio policy (src/BleRemotePolicy.h):
//   - a fresh boot (bond list not read yet) must still allow the reader radio,
//     otherwise an already-paired remote could never be reached again;
//   - once the host has reported the bond list, the radio is only allowed for a
//     paired remote (upstream d5b6c74d: no remote chosen => keep it off);
//   - a remote forgotten while the host is up flips the answer back immediately.
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
}  // namespace

int main() {
  using bleinput::BondState;
  using bleinput::readerRadioAllowedFor;

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

  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("ble reader radio policy: fresh-boot grace, no-remote off, paired on\n");
  return 0;
}
