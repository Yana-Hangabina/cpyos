#pragma once

// The download stall bound for the network PDF engine updater, as a pure rule.
//
// This exists because the first version of the watchdog was wrong twice in the
// same way, and both times the device -- not the code -- was what showed it:
//
//   1. The baseline was a member initialized to 0, so "no progress" was measured
//      from power-on. On a device already up for two minutes the very first tick
//      of the body stage reported a 45-second stall and cancelled a transfer that
//      had started 733 ms earlier (`download stalled at 0 / 1863304 bytes`,
//      0.7 s after `wolfSSL GET`).
//   2. The stage transition was not re-based either, so time spent in the
//      Manifest stage charged to the Body stage. On this AP the manifest alone
//      took ~50 s (TLS 1.2 fallback), which would have killed every download.
//
// So the rule takes the elapsed time as data and refuses to assume a baseline:
// a caller that has not established one passes `kNoBaseline`, and the answer is
// "not stalled". It also separates "no first byte yet" from "bytes stopped",
// because those are different failures with different patience: a mirror behind a
// cold TLS handshake is slow, a connection that delivered 900 KB and then froze is
// dead.
#include <cstdint>

namespace pdf_update {

enum class StallStage : uint8_t { Idle, Manifest, Body, Verify };

// "never observed this stage": no window has started, so nothing may expire.
constexpr uint32_t kNoBaseline = 0xFFFFFFFFu;

// A transport read timeout is 45 s and one route attempt can also block on
// connect; the first byte therefore gets two of those before we call it dead.
// Measured on the phone-hotspot AP: manifest served after 50 s, engine blob
// handshake in 0.7 s.
constexpr uint32_t kFirstByteMs = 90000;
// Once bytes are flowing, a gap longer than any single read timeout means the
// stream is stuck, and the user should not watch a spinner forever.
constexpr uint32_t kMidStreamMs = 45000;

struct StallDecision {
  bool fired = false;
  uint32_t limitMs = 0;
  uint32_t elapsedMs = 0;
};

/// Did the body stage go quiet for longer than its own allowance?
///
/// The elapsed time is the **modular** difference of two millis() stamps. That is
/// exact for any span under 2^32 ms (~49.7 days), which covers a download, and it
/// stays exact across the wrap. Two shapes are wrong and have been tried:
///   * a signed comparison: it turns a legitimately long wait into a negative
///     number and the stall then never fires;
///   * clamping to zero when `nowMs < baselineMs`: that is precisely the straddle
///     across the wrap, and the clamp hides a real 45-second silence.
inline StallDecision checkStall(const StallStage stage, const uint32_t downloaded,
                                const uint32_t nowMs, const uint32_t baselineMs) {
  StallDecision decision;
  if (stage != StallStage::Body || baselineMs == kNoBaseline) return decision;  /* WD-NOBASE */
  decision.limitMs = downloaded == 0 ? kFirstByteMs : kMidStreamMs;  /* WD-LIMIT */
  decision.elapsedMs = nowMs - baselineMs;  /* WD-MODULAR */
  decision.fired = decision.elapsedMs >= decision.limitMs;
  return decision;
}

/// The baseline a stage should be measured from: set it when the stage is entered,
/// and again whenever progress is actually made.
inline uint32_t nextBaseline(const StallStage stage, const StallStage previousStage,
                             const uint32_t nowMs, const uint32_t currentBaseline,
                             const uint32_t downloaded, const uint32_t previousDownloaded) {
  if (stage != StallStage::Body) return currentBaseline;
  if (previousStage != StallStage::Body) return nowMs;  /* WD-ENTRY */  // entering the body stage
  if (downloaded != previousDownloaded) return nowMs;   // real progress re-bases
  return currentBaseline;
}

}  // namespace pdf_update
