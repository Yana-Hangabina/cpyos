#pragma once

// When does "this page failed" become "this file cannot be opened here"?
//
// A single failed page is normal: one scanned spread can be too big for the
// free PSRAM while the rest of the book is fine, and the card offers a turn to
// skip past it.  A book where *three consecutive* pages fail is not a page
// problem -- it is this device, this file, or this memory shape, and asking the
// user to turn pages one at a time to discover that is the "dead end" the card
// exists to prevent.  At that point the card escalates: say the file cannot be
// shown here and offer Back, instead of another page they cannot read.
//
// The count resets on any success, so an isolated bad page in an otherwise
// readable book never escalates.

namespace pdf_fail_streak {

constexpr int kEscalateAfter = 3;

inline bool escalate(const int consecutiveFailures) {
  return consecutiveFailures >= kEscalateAfter;
}

// A success anywhere clears the streak; modelled so the reset rule is explicit
// rather than a `= 0` someone can drop.
inline int afterPage(const int streak, const bool rendered) { return rendered ? 0 : streak + 1; }

}  // namespace pdf_fail_streak
