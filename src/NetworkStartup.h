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

// Stop SNTP, disconnect, power the radio off and release the WiFi driver.
// Idempotent and safe when the radio was never started. Used when the owner of
// a WiFi session goes away: an STA mode left behind forces the CPU to stay at
// full speed (HalPowerManager) and keeps the modem powered for nothing.
void releaseRadio();

}  // namespace NetworkStartup
