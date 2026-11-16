#pragma once

#include <cstdint>

#include "WeReadClient.h"
#include "WeReadReadingClock.h"

namespace weread_time_sync {

// Global opt-in state, mirrored in /.crosspoint/weread/timesync.bin.
void begin();
bool enabled();
bool consentGiven();
// Turning the switch on the first time requires the caller to collect consent
// and then call setConsent(); both persist immediately.
bool setEnabled(bool on);
bool setConsent();
uint32_t lastUploadSeconds();

// Per-book ledger of effective seconds that the server has not confirmed yet.
uint32_t pendingSeconds(const char* bookId);
// Adds effective reading seconds (read-modify-write, safe across tasks).
bool addPending(const char* bookId, uint32_t seconds);
// Removes seconds the server confirmed, or the submitted seconds after an
// ambiguous report (the WeRead policy is "never replay an uncertain report").
bool settle(const char* bookId, uint32_t appliedSeconds, uint32_t submittedSeconds, bool reportSent);

// Uploads `input.readingSeconds` in the background when Wi-Fi is already up.
// Returns false when the request was rejected (disabled/offline/already busy).
bool kick(const char* bookId, const WeReadClient::ProgressSyncInput& input);
bool busy();
// Foreground WeRead network work (app sync, manual progress sync) takes the
// network exclusively; background time uploads back off while it runs.
void setForegroundBusy(bool busy);

}  // namespace weread_time_sync
