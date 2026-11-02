#pragma once

#include <atomic>
#include <cstdint>

// Cross-activity counter for work that must finish before standby light-sleep
// or automatic deep-sleep can safely suspend the CPU/radio.
//
// This deliberately avoids including the JJWXC headers from StandbyActivity:
// the counter is the only thing standby needs to know. Long-running workers
// call begin()/end() (or hold a Guard); StandbyActivity treats a non-zero
// count as "do not light-sleep, do not auto deep-sleep" and shows a small
// status badge instead.
namespace background_work {

void begin();
void end();
bool active();

class Guard {
 public:
  Guard() { begin(); }
  ~Guard() { end(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

}  // namespace background_work
