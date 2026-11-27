#include "DownloadProbe.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with the lwip
// headers other includes reach first -- the same ordering hazard OtaUpdater.cpp
// pins with a comment. Keep these two above any other project header.
#include "network/HttpDownloader.h"
#include <Logging.h>
// clang-format on

namespace download_probe {

size_t bestIndex(const std::vector<Measurement>& measurements) {
  size_t best = SIZE_MAX;
  uint32_t bestBps = 0;
  uint32_t bestFirstByte = UINT32_MAX;
  for (size_t i = 0; i < measurements.size(); ++i) {
    const Measurement& m = measurements[i];
    if (!m.ok) continue;
    if (best == SIZE_MAX || m.bytesPerSecond > bestBps ||
        (m.bytesPerSecond == bestBps && m.firstByteMs < bestFirstByte)) {
      best = i;
      bestBps = m.bytesPerSecond;
      bestFirstByte = m.firstByteMs;
    }
  }
  return best;
}

std::string selectUrl(const std::vector<download_routes::Candidate>& candidates, const std::string& advertised,
                      const size_t sampleBytes, const int probeTimeoutMs, const char* logTag) {
  if (candidates.empty()) return advertised;
  if (candidates.size() == 1) return candidates.front().url;

  std::vector<Measurement> measurements;
  measurements.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    const auto probe = HttpDownloader::probeUrl(candidate.url, sampleBytes, probeTimeoutMs);
    Measurement measurement;
    measurement.ok = probe.ok;
    measurement.bytesPerSecond = probe.bytesPerSecond();
    measurement.firstByteMs = probe.firstByteMs;
    if (!probe.ok) {
      LOG_INF(logTag, "probe %s: failed after %u ms", candidate.label.c_str(), static_cast<unsigned>(probe.elapsedMs));
    } else {
      LOG_INF(logTag, "probe %s: %u KB in %u ms (%u KB/s, first byte %u ms)", candidate.label.c_str(),
              static_cast<unsigned>(probe.bytes / 1024), static_cast<unsigned>(probe.elapsedMs),
              static_cast<unsigned>(measurement.bytesPerSecond / 1024), static_cast<unsigned>(probe.firstByteMs));
    }
    measurements.push_back(measurement);
  }

  const size_t best = bestIndex(measurements);
  if (best == SIZE_MAX) {
    LOG_ERR(logTag, "all download routes failed to probe; falling back to %s", advertised.c_str());
    return advertised;
  }
  // A route that delivered bytes but no measurable throughput is not a winner, it is
  // the least-bad survivor: on the 2026-11-27 bench every candidate to
  // raw.githubusercontent broke mid-transfer (5 KB, then status -1) while the same URL
  // streamed 144 KB/s from the Mac on the same AP. Saying "download route: github
  // (0 KB/s)" would read like a decision; the download that follows is expected to fail
  // and must be reported as a transfer failure, not as a storage one.
  if (measurements[best].bytesPerSecond == 0) {
    LOG_ERR(logTag, "no route measured any throughput; using %s (first byte %u ms, transfer broke)",
            candidates[best].label.c_str(), static_cast<unsigned>(measurements[best].firstByteMs));
  } else {
    LOG_INF(logTag, "download route: %s (%u KB/s)", candidates[best].label.c_str(),
            static_cast<unsigned>(measurements[best].bytesPerSecond / 1024));
  }
  return candidates[best].url;
}

}  // namespace download_probe
