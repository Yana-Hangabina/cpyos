#pragma once

#include <WiFi.h>

class GfxRenderer;

namespace NetworkStartup {

// One snapshot per network lifecycle transition; no persistent buffers.
void logMemory(const char* stage);

// Preserve render memory when PSRAM/internal SRAM headroom is healthy; otherwise
// release it before the Wi-Fi driver starts allocating.
void prepare(GfxRenderer& renderer);

// The only entry point for enabling STA/AP mode in application code.
bool setMode(GfxRenderer& renderer, wifi_mode_t mode);

// Drop the radio entirely (disconnect, WIFI_OFF, driver deinit). Idempotent.
// A connected STA keeps drawing power and blocks the power manager from
// downclocking, so screens that only "connect and leave" (Home Wi-Fi chip, the
// settings Wi-Fi page) call this on exit. Activities that own the session
// (JJWXC, WeRead) already tear it down themselves.
void releaseRadio();

}  // namespace NetworkStartup
