#pragma once

// Mirror the ESP_LOG stream to the SD card.
//
// The USB-Serial/JTAG console has gone dead three times in one day while the
// application kept running (screen responsive, esptool unable to sync). With
// every diagnostic on that console, a dead port leaves no evidence at all.
// A small RAM ring plus a periodic append to /.crosspoint/logs/boot.log keeps
// the last minutes of boot/runtime diagnostics on the card, so the next
// occurrence can be read by pulling the card instead of guessing.
//
// Only ESP_LOG output is captured (the bench console's own printf stays on the
// console). Log lines must never contain credentials; this port's logging does
// not print cookies or passwords.

void plusLogMirrorStart();
