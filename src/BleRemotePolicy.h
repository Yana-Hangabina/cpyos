#pragma once

namespace bleinput {

// What the app knows about bonded BLE remotes. The HID host only loads its bond
// list inside begin(), so at boot the answer is unknown: a reader must be allowed
// to start the radio once to learn it, and refusing that first start would leave
// an already-paired remote permanently unreachable.
struct BondState {
  bool known = false;      // the host has begun at least once this boot
  bool hasRemote = false;  // ... and it reported at least one bonded peer
};

// Whether a book may keep the BLE radio up. The HID host is central-role, so with
// no bonded remote it can only scan — never link — and a book that leaves it
// running drains the battery for the whole session. Upstream gates the same way on
// its configured peer address; here the bond list is the equivalent answer, with
// the one-start grace period above while it is still unknown.
constexpr bool readerRadioAllowedFor(const BondState& state) { return !state.known || state.hasRemote; }

}  // namespace bleinput
