#pragma once

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "Print.h"

// Arduino string-type glue used by ArduinoJson and other libraries when
// ARDUINO is defined: flash strings are ordinary strings on this SoC, and
// Printable is the interface a streamable object implements.
class __FlashStringHelper;
#define F(string_literal) (reinterpret_cast<const __FlashStringHelper*>(PSTR(string_literal)))
#ifndef PSTR
#define PSTR(s) (s)
#endif

class Printable {
 public:
  virtual ~Printable() = default;
  virtual size_t printTo(Print &p) const = 0;
};

// Minimal digital I/O surface used by BoardConfig helpers. The Plus owns its
// pins through the vendor BSP, so these are inert.
#ifndef HIGH
#define HIGH 0x1
#endif
#ifndef LOW
#define LOW 0x0
#endif
#ifndef INPUT
#define INPUT 0x01
#endif
#ifndef OUTPUT
#define OUTPUT 0x03
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 0x05
#endif
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return HIGH; }

#define PROGMEM
#define pgm_read_byte_near(address) (*(const unsigned char *)(address))
#define pgm_read_byte(address) (*(const unsigned char *)(address))
#define pgm_read_word(address) (*(const unsigned short *)(address))
#define pgm_read_dword(address) (*(const unsigned long *)(address))
// AVR progmem routines used by the vendored image codecs. Flash is
// memory-mapped on this SoC, so the ordinary routines are correct.
#define memcpy_P memcpy
#define ICACHE_RODATA_ATTR
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif
#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif
#define SET_LOOP_TASK_STACK_SIZE(bytes)
#define PGM_P const char *
#define PSTR(s) (s)

using boolean = bool;

inline unsigned long millis() {
  return static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
}

inline unsigned long micros() {
  return static_cast<unsigned long>(esp_timer_get_time());
}

inline void delay(unsigned long ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}

inline void delayMicroseconds(unsigned long us) {
  esp_rom_delay_us(static_cast<uint32_t>(us));
}

inline void yield() { taskYIELD(); }

inline void configTzTime(const char *, const char *) {}

// The Plus BSP does not expose the Arduino GPIO layer; all direct reads are
// treated as released, matching the idle pull-up state.


#include "HardwareSerial.h"
#include "Print.h"
#include "WString.h"

struct ESPMock {
  uint32_t getFreeHeap() { return esp_get_free_heap_size(); }
  uint32_t getPsramSize() const { return static_cast<uint32_t>(esp_psram_get_size()); }
  uint32_t getFreePsram() const {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  uint32_t getMaxAllocPsram() const {
    return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  }
  void restart() { esp_restart(); }
  uint32_t getHeapSize() {
    return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
  }
  uint32_t getMinFreeHeap() { return esp_get_minimum_free_heap_size(); }
  uint32_t getMaxAllocHeap() {
    return static_cast<uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  const char *getChipModel() const { return "ESP32-S31"; }
  uint32_t getChipRevision() const { return 0; }
  uint32_t getFlashChipSize() const { return 16U * 1024U * 1024U; }
};
inline ESPMock ESP;

namespace simulator_arduino_detail {
// Plain static, NOT thread_local: the RISC-V port places a task's TLS block
// inside that task's stack, and this engine is 2.5 KB. A TLS engine therefore
// overflows every small task stack (the 1 KB IPC task corrupted the heap at
// boot on ESP32-S31). Arduino's random() is not thread-safe anyway.
inline std::mt19937 &randomEngine() {
  static std::mt19937 engine{esp_random()};
  return engine;
}
} // namespace simulator_arduino_detail

inline long random(long max) {
  if (max <= 0) {
    return 0;
  }
  return std::uniform_int_distribution<long>(0, max - 1)(
      simulator_arduino_detail::randomEngine());
}

inline long random(long min, long max) {
  return min < max ? min + random(max - min) : min;
}

inline void randomSeed(unsigned long seed) {
  simulator_arduino_detail::randomEngine().seed(seed);
}

template <typename A, typename B>
constexpr auto max(A a, B b) -> decltype(a > b ? a : b) {
  return a > b ? a : b;
}

template <typename A, typename B>
constexpr auto min(A a, B b) -> decltype(a < b ? a : b) {
  return a < b ? a : b;
}
