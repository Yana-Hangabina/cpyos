#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "network/DownloadRoutes.h"

namespace download_probe {

// One measured route. `ok` is false when the sample never completed, in which
// case the timings describe the attempt that failed.
struct Measurement {
  bool ok = false;
  uint32_t bytesPerSecond = 0;
  uint32_t firstByteMs = 0;
};

// Pure half of the selection: given a measurement per candidate, which index
// wins? Failed routes lose to any working one; between two working routes the
// higher goodput wins, and an equal goodput is broken by the faster first byte
// (a route that starts sooner is the one a small file should use). Returns
// SIZE_MAX when nothing worked.
//
// Ties resolve to the earlier candidate, so an all-failed or all-equal set keeps
// the shortlist's own order rather than picking an arbitrary mirror.
size_t bestIndex(const std::vector<Measurement>& measurements);

// Impure half: sample each candidate with `sampleBytes` and return the URL of
// the winner, or `advertised` when nothing answered. `logTag` names the feature
// in the log so a field report can say which route it chose and why.
//
// This is the only place that measures route speed. OTA, font packs and any
// later bulk download must call it instead of writing the loop again: the two
// copies this replaced had already drifted (OTA probed, font packs tried routes
// in order and gave up on the first slow one).
std::string selectUrl(const std::vector<download_routes::Candidate>& candidates, const std::string& advertised,
                      size_t sampleBytes, int probeTimeoutMs, const char* logTag);

}  // namespace download_probe
