#pragma once

#include <cstdint>

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

// A radio nobody connected to stops after this long (upstream fecee610's
// kIdleOffMs): left up, it keeps the CPU at full speed, so a bonded remote that
// is switched off or left at home would cost the whole reading session.
inline constexpr uint32_t kReaderRadioIdleOffMs = 30u * 1000u;

// Whether an idle radio should be stopped now.
//   linked: a remote is connected. A remote sits quiet between two presses for a
//           long time and that is not idleness.
//   busy:   a scan or connect attempt is in flight. Asking the peer is activity,
//           so a discovery started near the boundary is not cut short.
//   idleMs: milliseconds since the last link/busy activity, or since the radio
//           came up. Callers pass a difference of millis(); the comparison is
//           wrap-safe because it only compares two uint32 values.
constexpr bool radioIdleExpired(const bool linked, const bool busy, const uint32_t idleMs,
                                const uint32_t limitMs = kReaderRadioIdleOffMs) {
  return !linked && !busy && idleMs >= limitMs;
}

// Wrap-safe "has this deadline passed?" for a millis() clock: true while `now` is
// before `deadline` in the half-range sense, so a 32-bit rollover does not park the
// budget 49 days in the future.
constexpr bool millisBefore(const uint32_t now, const uint32_t deadline) {
  return static_cast<uint32_t>(now - deadline) >= 0x80000000u;
}

// The reader's start retry budget (upstream fecee610: kRetryMs 5 s, kRetryLimit 6).
// A start refused for memory is retried while the page build settles, then the visit
// stops asking, so a starved reader does not release its SD font caches every pass
// for the whole session. A successful start or a new book visit clears the budget.
inline constexpr uint8_t kReaderStartRetryLimit = 6;
inline constexpr uint32_t kReaderStartRetryIntervalMs = 5000;

struct ReaderStartBudget {
  uint8_t refused = 0;
  bool waiting = false;
  uint32_t nextAttemptAt = 0;

  constexpr bool allowed(const uint32_t now) const {
    return refused < kReaderStartRetryLimit && (!waiting || !millisBefore(now, nextAttemptAt));
  }
  constexpr void refusedAt(const uint32_t now) {
    if (refused < kReaderStartRetryLimit) ++refused;
    waiting = true;
    nextAttemptAt = now + kReaderStartRetryIntervalMs;
  }
  constexpr void reset() {
    refused = 0;
    waiting = false;
  }
};

}  // namespace bleinput
