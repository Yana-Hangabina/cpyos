#pragma once
#include <cstdio>

#include "Arduino.h"
#include "Print.h"
#include "Stream.h"
#include "WString.h"
class HWCDC : public Stream {
public:
  void begin(unsigned long baud) {}
  void setTxTimeoutMs(uint32_t timeoutMs) {}
  // stderr instead of std::cerr: pulling <iostream> into every TU adds
  // libstdc++'s std::cin/cout global constructor, which on this RISC-V port
  // runs just before esp_ipc_init and leaves the internal heap unusable.
  size_t write(uint8_t c) override {
    fputc(c, stderr);
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    fwrite(buffer, 1, size, stderr);
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  template <typename... Args> void printf(const char *format, Args... args) {
    if constexpr (sizeof...(Args) == 0) {
      fputs(format, stderr);
    } else {
      char buf[256];
      snprintf(buf, sizeof(buf), format, args...);
      fputs(buf, stderr);
    }
  }
  operator bool() const { return true; }
};

// CrossPoint uses HardwareSerial when ARDUINO_USB_CDC_ON_BOOT is not defined.
// The simulator has a single stderr-backed serial endpoint, so both Arduino
// serial types intentionally resolve to the same host implementation.
using HardwareSerial = HWCDC;

extern HWCDC Serial;
