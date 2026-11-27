// Host regression for src/activities/network/WifiAttempt.h.
//
// The behaviour this pins is the one that produced "错误:通用故障" for a join
// that had not actually failed: WL_CONNECT_FAILED is a *stored* field in the
// Arduino core, so a previous network's rejection is still readable while a new
// association is in flight, and WiFi.begin() reports both "the driver refused"
// and "some earlier attempt failed" with the same value.
#include <cstdio>
#include <cstdint>

#include "WifiAttempt.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

using wifi_attempt::Snapshot;
using wifi_attempt::Verdict;

void testBeginRefusal() {
  const Snapshot before{wifi_attempt::kStatusDisconnected, 4};
  const Snapshot refused{wifi_attempt::kStatusConnectFailed, 4};
  check(wifi_attempt::classifyBegin(before, refused, true) == Verdict::DriverRefused,
        "a CONNECT_FAILED with no event is the driver refusing this join");

  const Snapshot rejected{wifi_attempt::kStatusConnectFailed, 5};
  check(wifi_attempt::classifyBegin(before, rejected, true) == Verdict::RejectedByAp,
        "an event this attempt owns outranks the refusal reading");
}

void testStaleFailureIsNotAFailure() {
  // The trap: the previous network auth-failed, so the core still reports 4.
  const Snapshot staleBefore{wifi_attempt::kStatusConnectFailed, 7};
  const Snapshot staleAfter{wifi_attempt::kStatusConnectFailed, 7};
  check(wifi_attempt::classifyBegin(staleBefore, staleAfter, true) == Verdict::Issued,
        "a parked CONNECT_FAILED must not end the new attempt");

  const Snapshot clean{wifi_attempt::kStatusDisconnected, 7};
  check(wifi_attempt::classifyBegin(clean, staleAfter, true) == Verdict::DriverRefused,
        "the same value is a real refusal when the attempt started clean");
}

void testIssuedPaths() {
  const Snapshot before{wifi_attempt::kStatusDisconnected, 2};
  const Snapshot accepted{wifi_attempt::kStatusDisconnected, 2};
  check(wifi_attempt::classifyBegin(before, accepted, true) == Verdict::Issued,
        "a normal begin() leaves the status at DISCONNECTED");

  const Snapshot connecting{wifi_attempt::kStatusNoSsidAvail, 3};
  check(wifi_attempt::classifyBegin(before, connecting, true) == Verdict::RejectedByAp,
        "an event during begin is reported even when the status is not 4");

  // Untracked builds (simulator, Plus compat layer) must keep the old behaviour
  // of never failing early, because the clock argument is always zero there.
  const Snapshot refused{wifi_attempt::kStatusConnectFailed, 0};
  const Snapshot zero{wifi_attempt::kStatusDisconnected, 0};
  check(wifi_attempt::classifyBegin(zero, refused, false) == Verdict::Issued,
        "without an event clock, begin() alone cannot prove a refusal");
}

void testPolledFailure() {
  const Snapshot attemptStart{wifi_attempt::kStatusDisconnected, 9};
  const Snapshot live{wifi_attempt::kStatusConnectFailed, 10};
  check(wifi_attempt::polledFailureIsLive(attemptStart, live, true), "an event makes the polled failure live");

  const Snapshot liveNoSsid{wifi_attempt::kStatusNoSsidAvail, 10};
  check(wifi_attempt::polledFailureIsLive(attemptStart, liveNoSsid, true), "NO_SSID_AVAIL is a failure status too");

  const Snapshot parked{wifi_attempt::kStatusConnectFailed, 9};
  const Snapshot start{wifi_attempt::kStatusConnectFailed, 9};
  check(!wifi_attempt::polledFailureIsLive(start, parked, true),
        "a status the attempt started with, with no new event, is not this attempt's verdict");

  check(!wifi_attempt::polledFailureIsLive(attemptStart, Snapshot{wifi_attempt::kStatusDisconnected, 9}, true),
        "DISCONNECTED is never a failure");
  check(!wifi_attempt::polledFailureIsLive(attemptStart, Snapshot{wifi_attempt::kStatusConnected, 9}, true),
        "CONNECTED is never a failure");

  check(wifi_attempt::polledFailureIsLive(attemptStart, parked, false),
        "without an event clock the previous behaviour stands");
}

void testStatusNumbersMatchTheCore() {
  // These are the values wl_status_t actually has; a silent change upstream
  // would make every rule above compare the wrong numbers.
  check(wifi_attempt::kStatusNoSsidAvail == 1, "WL_NO_SSID_AVAIL is 1");
  check(wifi_attempt::kStatusConnectFailed == 4, "WL_CONNECT_FAILED is 4");
  check(wifi_attempt::kStatusDisconnected == 6, "WL_DISCONNECTED is 6");
}

}  // namespace

int main() {
  testBeginRefusal();
  testStaleFailureIsNotAFailure();
  testIssuedPaths();
  testPolledFailure();
  testStatusNumbersMatchTheCore();
  std::printf("wifi_attempt: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
