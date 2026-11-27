#pragma once
// Force-included first in [env:simulator] (-include). Supplies the ESP-IDF and
// FreeRTOS bits the host shims do not carry but firmware sources use
// unconditionally in bench-only paths. Kept to standard headers: pre-scripts
// force-include this into library translation units too, which do not see the
// simulator's include directories.
#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

#ifndef ESP_LOGE
#define ESP_LOGE(tag, ...) ((void)(tag))
#endif
#ifndef pdPASS
#define pdPASS 1
#define pdFAIL 0
#endif
#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#elif !defined(MALLOC_CAP_INTERNAL)
#define MALLOC_CAP_INTERNAL (1u << 11)
#endif

#if __has_include(<esp_system.h>)
#include <esp_system.h>
#else
inline void esp_restart() {}
#endif

inline uint32_t getCpuFrequencyMhz() { return 240; }

struct heap_caps_region_t {};
inline void heap_caps_print_heap_info(uint32_t) {}
#endif  // __cplusplus
