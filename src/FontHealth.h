#pragma once

class GfxRenderer;
class ActivityManager;

// Font-state health, owned in one place and called by every target's main loop.
//
// Two policies used to live inline in src/main.cpp:
//   1. the *pairing* of NetworkStartup::prepare()'s font reclaim -- prepare()
//      releases the reader/supplement faces to make room for the radio and arms a
//      restore; the main loop is what actually gives that memory back once the
//      radio is idle (0.8.2 shipped the reclaim without the restore, which is the
//      "boxes after using the network" report);
//   2. a role watchdog: several paths release or rebuild font state, and whether
//      the UI gets its faces back must not depend on each of them remembering to
//      restore. Once a second the roles the UI draws with are checked and rebuilt
//      if any is unregistered.
//
// Both used to be readpico-only, because the Plus port has its own main
// (ports/metalio_plus/main/shell_demo.cpp) and src/main.cpp is not part of that
// build -- the same "shared policy, one target only" trap that HalDisplay and
// HalSystem hit. Call tick() from every loop; it throttles itself.
namespace font_health {

void tick(GfxRenderer& renderer, ActivityManager& activityManager);

}  // namespace font_health