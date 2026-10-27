#include "DeviceSecret.h"

#include <Logging.h>
#include <esp_random.h>
#include <nvs.h>

#if !defined(SIMULATOR)
#include <esp_cpu.h>
#include <esp_memory_utils.h>
#endif

#include <cstring>
#include <mutex>

bool deviceSecret(uint8_t (&out)[32]) {
  static uint8_t secret[32];
  static bool loaded = false;
  static std::mutex lock;  // web-server task and reader can both ask first
  std::lock_guard<std::mutex> guard(lock);

  // A failure is not cached: the next call retries.
  if (!loaded) {
#if !defined(SIMULATOR)
    // NVS lives in internal flash; reading it disables the caches for the
    // duration. A task whose stack is in PSRAM cannot run then -- ESP-IDF
    // asserts in spi_flash_disable_interrupts_caches_and_other_cpu(). The
    // render task's 32 KB stack is in PSRAM, and the library/thumbnail scan can
    // reach BookKey -> deviceSecret from there. Refuse instead of panicking;
    // setup() preloads the secret on the main task so this is only a backstop.
    if (esp_ptr_external_ram(esp_cpu_get_sp())) {
      LOG_ERR("DSEC", "refusing NVS secret read from a PSRAM-stack task");
      return false;
    }
#endif
    nvs_handle_t handle;
    if (nvs_open("devid", NVS_READWRITE, &handle) == ESP_OK) {
      size_t stored = sizeof(secret);
      const esp_err_t err = nvs_get_blob(handle, "secret", secret, &stored);
      if (err == ESP_OK) {
        loaded = stored == sizeof(secret);
      } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Only a missing secret is created. Any other read error leaves the
        // stored one alone: replacing it would orphan every wrapped book key.
        esp_fill_random(secret, sizeof(secret));
        loaded = nvs_set_blob(handle, "secret", secret, sizeof(secret)) == ESP_OK && nvs_commit(handle) == ESP_OK;
      }
      nvs_close(handle);
    }
    if (!loaded) {
      LOG_ERR("DSEC", "Device secret unavailable");
      return false;
    }
  }
  memcpy(out, secret, sizeof(out));
  return true;
}
