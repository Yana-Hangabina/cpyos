#pragma once

#include <WiFi.h>

#include <cstdint>

#include "activities/network/WifiAttempt.h"

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

// ---------------------------------------------------------------------------
// Radio ownership
//
// Two separate problems, so two separate rules.
//
//  1. Interleaved commands. esp_wifi does not tolerate a second mode/begin/
//     disconnect/scan while one is executing (ESP_ERR_WIFI_STATE -- observed on
//     device, and recorded in main.cpp's CMD:WIFICONNECT comment). Callers read
//     that refusal as a dead network, which is how a healthy screen ended up on
//     「连接失败 / <SSID> / 错误:通用故障」. Every driver command therefore runs
//     under a short exclusive lock, inside this module, never held across a
//     RenderLock or a wait.
//  2. Overlapping attempts. A lock does not order them: a background queue and
//     a foreground sheet can each "mode then begin" and still be associating at
//     the same moment, which costs the sheet a refusal and a stall. So whoever
//     is bringing the station up holds a *claim* for the whole sequence, and
//     background work asks stationClaimed() and stays off the radio. The claim
//     is a counter (nested claims are free) and it never blocks a foreground
//     screen -- a screen is not supposed to lose the station to a queue.
//
// Rule for callers: create a StationClaim from the UI task's loop/onEnter path,
// never while holding the RenderLock (the same contract as RenderLock itself).
// ---------------------------------------------------------------------------

// True while any screen or worker holds the station.
bool stationClaimed();
// The non-RAII half, for owners whose lifetime is an Activity's onEnter/onExit
// rather than a scope. releaseStation() is clamped at zero and a double claim
// from one owner counts twice, so callers must pair them; the sheet keeps a flag
// for exactly that.
void claimStation(uint32_t quietTimeoutMs = 1200);
void releaseStation();
// True while a driver command is executing (used to wait, not to decide).
bool radioCommandActive();
// True when no driver command is executing. This is the gate on the periodic
// idle policy work (NetworkStartup::pollIdle re-asserts WIFI_PS_NONE once a
// second, and that is a driver call), so that neither main loop asks the driver
// to do two things at once. A *claim* is deliberately not part of the test --
// see the comment on the implementation, because skipping the modem-sleep
// re-assertion for as long as a screen is open would be the silent kind of
// regression. Both main loops call this instead of restating the rule.
bool radioIdle();
// Wait for command activity to stop. Returns false if it never did, which is
// reported rather than swallowed: a wedged driver must be visible.
bool waitForRadioQuiet(uint32_t timeoutMs);

class StationClaim {
 public:
  // Waits (bounded) for an in-flight command before claiming, so the sequence
  // starts on a quiet driver. Never blocks on another task's *claim*: a screen
  // must not lose the station to a queue.
  explicit StationClaim(uint32_t quietTimeoutMs = 1200);
  ~StationClaim();
  StationClaim(const StationClaim&) = delete;
  StationClaim& operator=(const StationClaim&) = delete;
};

// Driver commands that are not part of a mode change. These exist so that no
// caller has to reach for the Arduino API directly and miss the lock; each one
// is a thin, guarded wrapper.
//
// joinStation() returns exactly what WiFi.begin() returned (a wl_status_t as an
// int) so the caller keeps its own classification -- the value is ambiguous by
// design, see activities/network/WifiAttempt.h.
//
// connect=false is the standby screen's silent-candidate form: the driver is
// asked to configure the station without starting the association, because that
// screen runs its own connect/poll state machine and cycles candidates.
int joinStation(const char* ssid, const char* password, bool connect = true);
// Asynchronous scan start: true when the driver accepted the scan.
bool scanStationAsync();
// Ask the driver to keep the station awake (esp_wifi_set_ps(WIFI_PS_NONE)),
// under the gate. setMode() already does this for every bring-up and the idle
// path re-asserts it once a second, so this is for the session that must not
// stall the moment it opens: the web server's loss-recovery loop and a KOReader
// sync transaction. A raw WiFi.setSleep() from those places is a driver command
// like any other, which is why they ask here instead.
void requestNoModemSleep();
// Drop the association without turning the radio off.
void dropAssociation();
// Full station shutdown, under one gate: disconnect, WiFi.mode(WIFI_OFF), then
// esp_wifi_deinit().  This is the sequence the app screens used to write out by
// hand when they owned the whole radio session; it lives here so that the gate
// rule cannot be forgotten at a later call site.  Screens that merely borrowed a
// session keep using releaseRadio().
void shutdownStation();

// Mode-off only, under the gate. For the path whose intent is literally "turn
// the radio off" -- silentRestart()'s touch-device variant, which stops the
// session instead of rebooting -- where the extra disconnect and driver deinit
// of shutdownStation() would be a behaviour change, not a fix.
void powerDownRadio();

// ---------------------------------------------------------------------------
// Radio event clock
//
// A wl_status_t on its own cannot say which attempt it belongs to: the Arduino
// core stores the value the last event wrote (libraries/WiFi/src/STA.cpp:251),
// so a WL_CONNECT_FAILED left by an earlier network is still readable while a new
// join is in flight -- and WiFi.begin() returns that same stale field as well as
// reporting a genuine driver refusal. Counting STA disconnect events is what
// separates "this attempt was rejected" from "the previous one was", and
// wifi_attempt::classifyBegin()/polledFailureIsLive() are the rules that consume
// it.
//
// It lives here rather than in one screen because it is global driver state and
// because the join that needs it is often not the sheet's: the standby silent
// reconnect and the background queue both read a status they did not write. The
// clock starts with the first radio command any caller makes through this module.
//
// Targets without an Arduino event API (the simulator, and the Plus compat
// layer, which logs the IDF reason inside its own WiFi.cpp) report
// tracksRadioEvents() == false, and wifi_attempt then keeps a failure to the
// attempt it can date.
// ---------------------------------------------------------------------------
bool tracksRadioEvents();
// The two values an attempt is bracketed by: the status the core currently
// stores, and how many disconnect events have been seen since boot.
wifi_attempt::Snapshot radioSnapshot();
// Last 802.11 reason code the clock saw (0 when untracked). Logged, never
// shown: 「错误:通用故障」 cannot tell a wrong password from an auth timeout.
uint8_t lastDisconnectReason();

// Background workers (queue services, download jobs) are not Activities and so
// hold no renderer reference. Registering the app's renderer once at boot lets
// them reach the same entry point instead of writing a second, weaker version of
// it. The SNTP hold is the one difference that matters most: it is what keeps a
// pending NTP DNS callback from aborting lwIP at association. Without a
// registration the renderer-free overload still applies every other rule (BLE
// stop, SNTP hold, modem-sleep policy, affordability) and only skips the
// render-memory reclaim.
void registerRenderer(GfxRenderer* renderer);
bool setMode(wifi_mode_t mode);

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
