#pragma once

#include <cstdint>

// Why one Wi-Fi association attempt failed is not answerable from
// `WiFi.status()` alone, and the wrong answer costs the user a generic error:
//
//   * `STAClass::status()` is just the last value an *event* stored
//     (libraries/WiFi/src/STA.cpp:251), so a `WL_CONNECT_FAILED` left by an
//     earlier network is still readable while a new join is in flight.
//   * `WiFiSTAClass::begin()` returns that same stale field, and *also* returns
//     `WL_CONNECT_FAILED` when the driver refused to issue the join at all
//     (WiFiSTA.cpp:72-103). A refusal emits no event.
//
// The two cases are separable only against the disconnect-event clock, which
// `NetworkStartup` owns (see its header) because the join that must be dated is
// frequently not the Wi-Fi sheet's: the standby screen's silent candidate loop
// reads the same stored status, and a queue worker may have moved the driver in
// between. These helpers hold the decision so it can be tested without a radio.
//
// Callers pick the verdict that matters to them: the sheet ends the attempt on
// DriverRefused and on a live RejectedByAp, while standby skips only a candidate
// the driver refused outright -- it cannot see a rejection at begin() time,
// because it asks the driver to configure the station without associating.
namespace wifi_attempt {

// Mirrors wl_status_t (libraries/WiFi/src/WiFiType.h:44-52). Pinned by
// test/wifi_attempt against the installed core header.
constexpr int kStatusNoSsidAvail = 1;
constexpr int kStatusConnected = 3;
constexpr int kStatusConnectFailed = 4;
constexpr int kStatusDisconnected = 6;

struct Snapshot {
  int status = kStatusDisconnected;
  uint32_t disconnectEvents = 0;
};

enum class Verdict : uint8_t {
  Issued,         // the join reached the driver; keep waiting for an event
  DriverRefused,  // begin() failed without producing any event
  RejectedByAp,   // a disconnect event this attempt owns explains the failure
};

inline bool isFailureStatus(const int status) {
  return status == kStatusConnectFailed || status == kStatusNoSsidAvail;
}

// `eventsTracked` is false where the build cannot count STA disconnect events
// (simulator, and the Plus compat layer which logs the reason itself); the
// clock argument then means nothing, so these helpers must not invent a
// failure that the previous behaviour would not have reported.
inline Verdict classifyBegin(const Snapshot& before, const Snapshot& after, const bool eventsTracked) {
  if (!eventsTracked) return Verdict::Issued;
  if (after.disconnectEvents != before.disconnectEvents) return Verdict::RejectedByAp;
  // The value became CONNECT_FAILED while nothing produced an event, so begin()
  // is the only thing that could have written it: the driver refused.
  if (after.status == kStatusConnectFailed && before.status != kStatusConnectFailed) return Verdict::DriverRefused;
  // CONNECT_FAILED with no event *and* no change: that is the previous
  // attempt's verdict still parked in the core's field. Nothing here says the
  // new join failed, so ending it would be a guess.
  return Verdict::Issued;
}

// True when a polled failure status belongs to this attempt.
inline bool polledFailureIsLive(const Snapshot& attemptStart, const Snapshot& now, const bool eventsTracked) {
  if (!isFailureStatus(now.status)) return false;
  return !eventsTracked || now.disconnectEvents != attemptStart.disconnectEvents;
}

}  // namespace wifi_attempt
