#pragma once

// Remembered JJWXC payment password (6 ASCII digits), wrapped with a key
// derived from this device's secret (AES-256-GCM). It lives on the SD card at
// /.crosspoint/jjwxc/paykey and is useless on another device or after a full
// flash erase. No plaintext password is written to the card or any log.
namespace jjwxcpay {

bool available();
// out receives 6 digits + NUL. Wipes out and removes a corrupt file on failure.
bool load(char (&out)[7]);
bool save(const char* code);
bool clear();

}  // namespace jjwxcpay
