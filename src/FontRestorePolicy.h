#pragma once

#include <cstdint>

// Paired with the render-memory reclaim.
//
// NetworkStartup::prepare() releases the reader font state before the Wi-Fi
// driver allocates. On the system-font-pack targets that state is not merely
// reader-grade memory: the font-extension supplements -- which include the
// reading core released from the data partition -- are the only registered faces
// covering CJK beyond the ~1,500-codepoint UI subset baked into app0. A reclaim
// that is never undone therefore leaves every screen drawn after it showing
// tofu for characters the partition actually carries. That is the 0.8.2
// "晋江书籍列表一堆缺字" defect: the core was installed, released to the card and
// registered, and then dropped by the first network entry.
//
// The pairing rule lives here as pure logic so it has exactly one owner and can
// fail in a test rather than in the field: prepare() arms it, the main loop
// performs it, this function says when performing is safe. Call sites must call
// due() instead of restating any of these terms -- a rule only some callers
// apply is not a rule.
namespace font_restore {

// Floor on how long the reclaim must stay in effect. The driver keeps whatever
// it allocated once bring-up has finished, so restoring later cannot take
// memory away from a bring-up that already succeeded; the real protection is
// the "nothing is using the network" test below, and this only stops the
// restore from landing between the mode change and the join.
constexpr uint32_t kMinQuietMs = 1500;

struct Situation {
  // A reclaim happened and has not been undone yet.
  bool pending = false;
  // No esp_wifi driver command is executing right now.
  bool radioIdle = true;
  // Some screen or worker is mid bring-up (mode -> begin sequence).
  bool stationClaimed = false;
  // A TLS request, download or web-server operation holds the radio session.
  bool networkHeld = false;
  // Time since the reclaim was armed.
  uint32_t elapsedMs = 0;
};

// True when the font state can be brought back without competing with network
// work for the memory the reclaim was made for.
constexpr bool due(const Situation& situation) {
  return situation.pending && situation.radioIdle && !situation.stationClaimed && !situation.networkHeld &&
         situation.elapsedMs >= kMinQuietMs;
}

}  // namespace font_restore
