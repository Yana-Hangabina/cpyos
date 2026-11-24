#pragma once

// What a serial PDF bench command should do given who is in front.
//
// The three outcomes used to be inline branches, and the difference between two
// of them is invisible in a log: "the reader is not open" (nothing to do) and
// "a keyboard is covering the reader" (dismiss it and the command becomes
// reachable).  Both printed "ignored: foreground activity is ...", so an
// unattended script kept firing into a modal it could not see.
//
// Pure so the mapping is pinned: Forward / DismissAndResend / Ignore.

namespace pdf_bench {

enum class Route { Forward, DismissAndResend, Ignore };

inline Route route(const bool readerIsForeground, const bool readerIsInStack) {
  if (readerIsForeground) return Route::Forward;
  if (readerIsInStack) return Route::DismissAndResend;
  return Route::Ignore;
}

}  // namespace pdf_bench
