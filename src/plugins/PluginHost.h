#pragma once

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <string>

#include "PluginManager.h"
#include "cpyos_plugin_api.h"

// Owns the single native-plugin execution slot: one ELF task at a time, a
// bounded input queue filled by the hosting activity, and a crash marker that
// survives a reboot so the next scan can say the previous run died.
class PluginHost {
 public:
  enum class State { Idle, Running, Finished, Failed };

  static PluginHost& instance();

  bool start(const native_plugins::PluginInfo& info);
  void requestExit();
  void stop();
  void pushEvent(const CpyosPluginEvent& event);

  State state() const { return state_.load(std::memory_order_acquire); }
  bool active() const { return state() == State::Running; }
  const char* lastError() const { return lastError_; }
  const native_plugins::PluginInfo& info() const { return info_; }

 private:
  PluginHost() = default;
  static void taskTrampoline(void* arg);
  void taskMain();
  void fail(const char* message);
  void freeAllocations();

  static void apiLog(const char* message);
  static void apiRequestExit();
  static int32_t apiExitRequested();
  static uint32_t apiNowMs();
  static void apiDelay(uint32_t ms);
  static int32_t apiPollEvent(CpyosPluginEvent* out);
  static void apiClear(int32_t black);
  static void apiFillRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t black);
  static void apiDrawText(int32_t x, int32_t y, int32_t font, const char* text, int32_t black);
  static int32_t apiTextWidth(int32_t font, const char* text);
  static void apiPresent();
  static void* apiAlloc(uint32_t bytes);
  static void apiRelease(void* pointer);
  static int32_t apiDataRead(const char* name, uint8_t* buffer, int32_t capacity);
  static int32_t apiDataWrite(const char* name, const uint8_t* data, int32_t length);

  native_plugins::PluginInfo info_{};
  CpyosPluginApi api_{};
  size_t allocatedBytes_ = 0;
  static constexpr size_t kMaxAllocations = 32;
  void* allocations_[kMaxAllocations] = {};
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> exitRequested_{false};
  TaskHandle_t task_ = nullptr;
  bool taskStackInPsram_ = false;
  const char* lastError_ = "";
};

#endif  // CROSSPOINT_PLUGIN_RUNTIME
