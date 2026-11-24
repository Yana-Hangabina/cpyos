#pragma once

#include <WiFi.h>

#include <cstdint>

class GfxRenderer;

namespace NetworkStartup {

// Hold the radio for the lifetime of a network operation (TLS request, download,
// upload, web-server work). While a hold is active the idle timer cannot tear
// the radio down; the reference count makes nested transport guards safe.
void hold();
void releaseHold();

class ActivityGuard {
 public:
  ActivityGuard() { hold(); }
  ~ActivityGuard() { releaseHold(); }
  ActivityGuard(const ActivityGuard&) = delete;
  ActivityGuard& operator=(const ActivityGuard&) = delete;
};

// Renew the idle deadline. Safe from any task; cheap (two relaxed stores).
void noteActivity();

// Main-loop idle check. Releases a connected STA after the configured quiet
// period when no hold, background job, or web server is using it.
void pollIdle(uint32_t nowMs);

// One snapshot per network lifecycle transition; no persistent buffers.
void logMemory(const char* stage);

// Preserve render memory when PSRAM/internal SRAM headroom is healthy; otherwise
// release it before the Wi-Fi driver starts allocating.
void prepare(GfxRenderer& renderer);

// The only entry point for enabling STA/AP mode in application code.
bool setMode(GfxRenderer& renderer, wifi_mode_t mode);

// True when the internal heap can plausibly carry the Wi-Fi driver.  setMode()
// applies this itself, after prepare() has reclaimed render memory -- a threshold
// checked before that reclaim has refused associations that then succeeded.
// Callers that want to name the reason use this; pass verbose=false to ask without
// writing the log line again (setMode has already written it).
bool canAffiliate(bool verbose = true);

// Drop the radio entirely (disconnect, WIFI_OFF, driver deinit). Idempotent.
// This is the immediate path: sleep/standby and screens that explicitly must
// not keep the radio use it. Normal "connect and leave" screens leave the STA
// up; pollIdle() releases it after SETTINGS.wifiIdleTimeoutSeconds of no
// network activity.
void releaseRadio();
// Full teardown: stop and deinit the driver, destroy the default netif, and leave
// the radio off.  This is the only form that gives the ~40 KB of driver/lwIP
// memory back to the heap, and it is the form that used to panic inside
// tcpip_thread -- an observation made before the module was stopped from zeroing
// the app's external-RAM bss (docs/engineering/pdf-xip.md, crash class 1).
// Reachable today only from the bench (CMD:WIFIDEINIT); promote it into
// releaseRadio() once it has run clean repeatedly in the degenerate state.
bool releaseRadioDeep();

/// The reader cannot call releaseRadioDeep() itself: onEnter runs on the
/// render task (PSRAM stack) and the PHY teardown is not safe there.  It
/// schedules it; the main loop (internal stack) performs it.
void requestDeepRelease();
bool flushDeepRelease();


// Nested-safe SNTP suspension for a radio session. setMode() takes one hold and
// releaseRadio() drops it, so no caller has to remember: a pending NTP DNS
// query at the moment something clears the DNS cache (on association) is what
// trips lwIP's "Required to lock TCPIP core functionality" assert. Activities
// that must keep the clock off for longer hold their own nested suspension.
void suspendTimeSync();
void resumeTimeSync();

}  // namespace NetworkStartup
