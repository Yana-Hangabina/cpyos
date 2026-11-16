#include <inttypes.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CPYOS_PLUS_BSP
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cpyos_sd.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "metalio_epd.h"
#include "metalio_i2c1.h"
#include <fcntl.h>
#include <unistd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "metalio_touch.h"
#endif

#ifdef CPYOS_UI_PROBE
extern void cpyos_ui_probe(void);
extern void cpyos_ui_loop(void);
#endif
#ifdef CPYOS_READER_DEMO
extern void cpyos_reader_demo(void);
extern void cpyos_reader_loop(void);
#endif
#ifdef CPYOS_SHELL_DEMO
extern void cpyos_shell_demo(void);
extern void cpyos_shell_loop(void);
#endif

static const char *TAG = "CPYOS-PLUS";

#ifdef CPYOS_PLUS_BSP
static void draw_bringup_pattern(void) {
    EpdiyHighlevelState *hl = MetalioEpd_Hl();
    uint8_t *fb = epd_hl_get_framebuffer(hl);
    const int w = epd_width();
    const int h = epd_height();

    epd_hl_set_all_white(hl);
    EpdRect header = {40, 40, w - 80, 120};
    epd_fill_rect(header, 0x0, fb);
    for (int g = 0; g < 8; ++g) {
        EpdRect bar = {40, 200 + g * 60, (w - 80) / 8, 50};
        epd_fill_rect(bar, (uint8_t)(g * 2), fb);
    }
    EpdRect border = {20, 20, w - 40, h - 40};
    epd_draw_rect(border, 0x0, fb);

    epd_poweron();
    epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
    epd_poweroff();
    ESP_LOGI(TAG, "display bring-up pattern pushed (%dx%d)", w, h);
}
#endif

static void panel_color_test(void) {
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    if (hl == nullptr) return;
    uint8_t* fb = epd_hl_get_framebuffer(hl);
    const EpdRect all = {0, 0, epd_width(), epd_height()};
    epd_poweron();
    for (int cycle = 0; cycle < 2; ++cycle) {
        epd_hl_set_all_white(hl);
        if (cycle == 0) {
            epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
        } else {
            epd_hl_update_screen(hl, MODE_GC16, 25);
        }
        ESP_LOGI(TAG, "panel white cycle %d", cycle + 1);
        vTaskDelay(pdMS_TO_TICKS(1500));

        epd_fill_rect(all, 0x00, fb);
        epd_hl_update_screen(hl, MODE_GC16, 25);
        ESP_LOGI(TAG, "panel black cycle %d", cycle + 1);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    epd_hl_set_all_white(hl);
    epd_hl_update_screen(hl, MODE_GC16, 25);
    epd_poweroff();
    ESP_LOGI(TAG, "panel color test done: 2 x white/black, final white");
}

// Boot watchdog. The panel bring-up has been observed to stop between the I2C
// master coming up and the DRAM allocation, and the unit then answers neither
// the console nor esptool, so it used to need a physical power cycle. A task
// that is merely blocked (while interrupts still run) can restart itself; a
// lockup with interrupts off is covered by CONFIG_ESP_INT_WDT (800 ms).
// Arming this before MetalioEpd_Init() and disarming once the shell is routed
// turns a silent boot hang into a bounded reboot.
static constexpr int kBootWatchdogSeconds = 60;
static esp_timer_handle_t s_boot_watchdog;
// Survives a software reset, so one boot can be retried once before falling
// back to the other OTA slot.
RTC_NOINIT_ATTR static uint32_t s_bootFallbackTried;
static constexpr uint32_t kBootFallbackMagic = 0x46414C4CU;  // "FALL"

// Bench: prove the watchdog's slot fallback without a bad image. `boothang`
// stores this in RTC (survives the software reset it triggers), the next boot
// consumes it and stalls instead of initialising, the 60 s watchdog switches
// OTA slots, and the boot after that is normal because the flag was cleared
// before stalling.
RTC_NOINIT_ATTR static uint32_t s_bootHangDemo;
static constexpr uint32_t kBootHangMagic = 0x48414E47U;  // "HANG"
void plus_boot_hang_request(void) { s_bootHangDemo = kBootHangMagic; }
static bool plus_boot_hang_consume(void) {
    const bool hang = s_bootHangDemo == kBootHangMagic;
    s_bootHangDemo = 0;
    return hang;
}

static void boot_watchdog_cb(void* arg) {
    (void)arg;
    // A bad image must not strand the unit (no reset button on this board):
    // switch to the other OTA slot once, if it holds a plausible app image.
    if (s_bootFallbackTried != kBootFallbackMagic) {
        const esp_partition_t* running = esp_ota_get_running_partition();
        const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
        uint8_t magic = 0;
        if (running != NULL && other != NULL && running->address != other->address &&
            esp_partition_read(other, 0, &magic, 1) == ESP_OK && magic == 0xE9) {
            s_bootFallbackTried = kBootFallbackMagic;
            if (esp_ota_set_boot_partition(other) == ESP_OK) {
                esp_restart();
            }
        }
    }
    // Restart BEFORE logging: if the stall is in a path that holds the console
    // lock, logging here would block the very recovery this watchdog exists for.
    // The reset reason (and the breadcrumbs) survive in the next boot's log.
    esp_restart();
}

static void arm_boot_watchdog(void) {
    const esp_timer_create_args_t args = {
        .callback = boot_watchdog_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "boot_wd",
    };
    if (esp_timer_create(&args, &s_boot_watchdog) != ESP_OK) {
        ESP_LOGW(TAG, "boot watchdog unavailable");
        return;
    }
    esp_timer_start_once(s_boot_watchdog, (uint64_t)kBootWatchdogSeconds * 1000000ULL);
}

// Called by the shell once routing finished (console task started, first
// activity entered). Safe to call when the watchdog was never armed.
void plus_boot_done(void) {
    s_bootFallbackTried = 0;  // this image booted; forget any earlier fallback
    if (s_boot_watchdog != NULL) {
        esp_timer_stop(s_boot_watchdog);
        esp_timer_delete(s_boot_watchdog);
        s_boot_watchdog = NULL;
    }
}

static void scan_i2c1_bus(void) {
    ESP_LOGI(TAG, "I2C1 probe scan (SDA=0 SCL=1)");
    if (!metalio_i2c1_lock(1000)) {
        ESP_LOGE(TAG, "I2C1 scan: lock timeout");
        return;
    }
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (metalio_i2c1_probe_locked(I2C_NUM_1, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "  I2C1 0x%02X ACK", addr);
        }
    }
    metalio_i2c1_unlock();
}


// A confirmed SD write timeout leaves every later write failing for the rest of
// the boot (0x107), while reads keep working; the user otherwise has to
// power-cycle. Probe with a tiny write and remount the card when it fails
// twice; if the remount itself cannot produce a working card, restart.
static volatile bool s_sdHealthForceFail = false;
static volatile bool s_sdHealthEnabled = true;

void plus_sd_health_enable(int enabled) { s_sdHealthEnabled = enabled != 0; }

// A failing card wedges the write path on every boot. Reboot at most a few
// times per power cycle, otherwise the device would reset in a loop and never
// become usable. RTC_NOINIT survives esp_restart() but not a power cycle.
RTC_NOINIT_ATTR static uint32_t s_sdRebootCount;
static constexpr uint32_t kSdRebootMagic = 0x53445242U;  // "SDRB"
static constexpr uint32_t kSdMaxReboots = 3;

// Bench hook: make the next health probes use an unwritable path so the
// watchdog's detection + remount path can be exercised on demand.
void plus_sd_health_force_fail(void) { s_sdHealthForceFail = true; }

static void sdHealthTask(void*) {
    int consecutiveFailures = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (!s_sdHealthEnabled) { consecutiveFailures = 0; continue; }
        const char* probePath = s_sdHealthForceFail ? "/nonexistent/.sdhealth" : "/sdcard/.sdhealth";
        const int fd = open(probePath, O_RDWR | O_CREAT, 0644);
        bool ok = false;
        if (fd >= 0) {
            const uint8_t probe[8] = {0x5A, 0xA5, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
            ok = write(fd, probe, sizeof(probe)) == (ssize_t)sizeof(probe);
            close(fd);
        }
        if (ok) {
            if (consecutiveFailures) ESP_LOGW(TAG, "SD health: writes recovered");
            consecutiveFailures = 0;
            continue;
        }
        ++consecutiveFailures;
        ESP_LOGW(TAG, "SD health: probe write failed (%d)", consecutiveFailures);
        if (consecutiveFailures < 2) continue;
        // Do NOT unmount here: other tasks hold open FATFS files, and closing
        // them from this task trips `_lock_close` (xQueueGetMutexHolder(h) == 0).
        // A reboot closes everything safely and the card mounts clean, which is
        // exactly the manual power cycle this replaces.
        if (s_sdRebootCount != kSdRebootMagic) {
            s_sdRebootCount = kSdRebootMagic;
            s_sdRebootCount = kSdRebootMagic + 1;
        } else {
            ++s_sdRebootCount;
        }
        if (s_sdRebootCount - kSdRebootMagic <= kSdMaxReboots) {
            ESP_LOGE(TAG, "SD health: write path wedged; restarting (attempt %u)",
                     (unsigned)(s_sdRebootCount - kSdRebootMagic));
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        ESP_LOGE(TAG, "SD health: card still failing after %u restarts; stopping auto-recovery",
                 (unsigned)kSdMaxReboots);
        vTaskDelete(nullptr);
    }
}

// One-shot card format requested from the console (`sdformat`): the flag lives in
// NVS (internal flash, not the card) and is consumed here before any file is
// opened, so the FATFS unmount/format/remount in esp_vfs_fat_sdcard_format() is
// safe.
#define kNvsNamespace "cpyos"
#define kFormatKey "sdformat"

bool plus_sd_format_requested(void) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t value = 0;
    const esp_err_t err = nvs_get_u8(handle, kFormatKey, &value);
    nvs_close(handle);
    return err == ESP_OK && value != 0;
}

void plus_sd_format_clear(void) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_key(handle, kFormatKey);
    nvs_commit(handle);
    nvs_close(handle);
}

static void sdHealthStart(void) {
    xTaskCreate(sdHealthTask, "sd-health", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
}

void app_main(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGW(TAG, "Metalio E-INK4 Plus bring-up build - UNTESTED ON HARDWARE");
    ESP_LOGI(TAG, "chip=%d cores=%d flash=%" PRIu32 " psram=%u", chip.model,
             chip.cores, flash_size, (unsigned)esp_psram_get_size());

#ifdef CPYOS_PLUS_BSP
    arm_boot_watchdog();
    if (plus_boot_hang_consume()) {
        ESP_LOGE(TAG, "boothang: stalling before init; the boot watchdog should switch OTA slots");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (MetalioEpd_Init()) {
#if !defined(CPYOS_SHELL_DEMO) && !defined(CPYOS_READER_DEMO) && !defined(CPYOS_UI_PROBE)
        // Bring-up diagnostics only. The UI builds paint a real first frame, and
        // the white/black cycles cost ~12 s of panel time on every boot — which
        // a silent restart into the reader (JJWXC chapter hand-off) must not pay.
        panel_color_test();
        draw_bringup_pattern();
#endif
    } else {
        ESP_LOGE(TAG, "display init failed; check TPS65185/TCA9555 and VCOM");
    }

    // Diagnostic only: list every ACK on the shared I2C1 bus. This distinguishes
    // "bus dead" from "TCA/TPS missing" on the first unit.
    scan_i2c1_bus();

    if (metalio_touch_init()) {
        ESP_LOGI(TAG, "FT6336U touch ready");
    } else {
        ESP_LOGE(TAG, "FT6336U touch init failed");
    }

    if (cpyos_sd_mount()) {
        ESP_LOGI(TAG, "SD mount ok");
        nvs_flash_init();
        if (plus_sd_format_requested()) {
            ESP_LOGW(TAG, "SD format requested: formatting the card now");
            const esp_err_t fmt = esp_vfs_fat_sdcard_format("/sdcard", cpyos_sd_card());
            ESP_LOGW(TAG, "SD format: %s", esp_err_to_name(fmt));
            plus_sd_format_clear();
        }
        sdHealthStart();
    } else {
        ESP_LOGE(TAG, "SD mount failed");
    }
#endif

#if defined(CPYOS_SHELL_DEMO)
    cpyos_shell_demo();
#elif defined(CPYOS_READER_DEMO)
    cpyos_reader_demo();
#elif defined(CPYOS_UI_PROBE)
    cpyos_ui_probe();
#endif

    while (true) {
#if defined(CPYOS_SHELL_DEMO)
        cpyos_shell_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
#elif defined(CPYOS_READER_DEMO)
        cpyos_reader_loop();
        vTaskDelay(pdMS_TO_TICKS(30));
#elif defined(CPYOS_UI_PROBE)
        cpyos_ui_loop();
        vTaskDelay(pdMS_TO_TICKS(30));
#else
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive");
#endif
    }
}
