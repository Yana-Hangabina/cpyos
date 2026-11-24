#pragma once

#include <cstdint>

// "Has this deadline passed?" for millis()-based budgets.
//
// The comparison is modular on purpose: millis() wraps every ~49 days, and both
// a plain `now >= deadline` and a `deadline - now > 0` form flip to "expired
// forever" or "never expires" across that point.  The signed difference is
// correct for any gap shorter than 2^31 ms (~24 days), which every budget here
// is by orders of magnitude.
namespace pdf_deadline {

// A zero deadline means "no budget", never "expired".
inline bool expired(const uint32_t deadlineMs, const uint32_t nowMs) {
  return deadlineMs != 0 && static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

}  // namespace pdf_deadline
