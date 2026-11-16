#include "cpyos_sd.h"

#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "config.h"
#include "metalio_sd.h"

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_private/esp_cache_private.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

static const char *TAG = "CPYOS-SD";
static sdmmc_card_t *s_card = nullptr;

void MetalioSd_ReleaseHold(void) {
    const gpio_num_t pins[] = {SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN,
                               SDMMC_D1_PIN, SDMMC_D2_PIN, SDMMC_D3_PIN};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_hold_dis(pins[i]);
    }
}

// Runtime recovery for the SD write wedge.
//
// A write that times out (CMD13 status 0x107) leaves the host in a state where
// every later write fails for the rest of the boot, while reads keep working.
// The health watchdog below calls this after the wedge is confirmed; unmounting
// drops every FATFS handle, so callers must have already abandoned their files.
// Returns true when a fresh card is mounted.
bool cpyos_sd_remount(void) {
    if (s_card != nullptr) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "remount: unmount failed: %s", esp_err_to_name(err));
            return false;
        }
        s_card = nullptr;
    }
    return cpyos_sd_mount();
}

bool cpyos_sd_is_mounted(void) { return s_card != nullptr; }

sdmmc_card_t *cpyos_sd_card(void) { return s_card; }

bool cpyos_sd_release_for_usb(void) {
    if (s_card == nullptr) return false;
    const BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv == 0xFF) {
        ESP_LOGE(TAG, "SD card has no FatFs drive to release");
        return false;
    }
    char drive[3] = {(char)('0' + pdrv), ':', 0};
    f_mount(NULL, drive, 0);
    ff_diskio_unregister(pdrv);
    esp_vfs_fat_unregister_path("/sdcard");
    ESP_LOGW(TAG, "SD detached from the app for USB mass storage");
    return true;
}

// SD transport policy mirrors the reference implementation for this exact board
// (kiikoread rc110, board metalio47). Its comments carry the field evidence:
//
//   * DMA stays in INTERNAL RAM: a 4096-byte aligned buffer owned here, the
//     SDK's own aligned allocation disabled, and every caller buffer checked for
//     internal placement. Handing the S31 external (PSRAM) memory for SD DMA is
//     the documented source of data corruption, and a bad data CRC surfaces
//     later as a dead card, not as a write error.
//   * A failed VFS mount already releases the host it created; the ladder must
//     NOT deinit again (that stops the SD clock at an arbitrary point, which is
//     how a healthy card got wedged into INVALID_CRC for the rest of a boot).
//   * 4-bit stays first: this board wires D1..D3 and the reference starts high
//     and only steps down. 1-bit profiles must clear D1..D3 to GPIO_NUM_NC or
//     the pads keep their previous direction.
//   * After mounting, prove the card really works (CMD13, one sector read, FAT
//     root listing) before any consumer sees it.
static void *s_sd_dma = NULL;

static bool sd_internal_dma_buffer(int slot, const void *buffer, size_t bytes) {
    return esp_ptr_internal(buffer) && sdmmc_host_check_buffer_alignment(slot, buffer, bytes);
}

static esp_err_t prepare_sd_dma(sdmmc_host_t *host) {
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    size_t alignment = 0;
    esp_err_t err = esp_cache_get_alignment(caps, &alignment);
    if (err != ESP_OK) {
        return err;
    }
    if (alignment < 4) {
        alignment = 4;
    }
    if (s_sd_dma == NULL) {
        s_sd_dma = heap_caps_aligned_alloc(alignment, 4096, caps);
        if (s_sd_dma == NULL) {
            ESP_LOGE(TAG, "SD DMA buffer: no internal DMA memory");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "SD DMA buffer: 4096 B internal, %u-byte aligned", (unsigned)alignment);
    }
    host->flags &= ~SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host->check_buffer_alignment = sd_internal_dma_buffer;
    host->dma_aligned_buffer = s_sd_dma;
    host->unaligned_multi_block_rw_max_chunk_size = 8;
    return ESP_OK;
}

static void configure_sd_bus(sdmmc_host_t *host, sdmmc_slot_config_t *slot, const int khz, const int width) {
    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    host->slot = SDMMC_HOST_SLOT_0;
    host->max_freq_khz = khz;
    slot->width = width;
    slot->clk = SDMMC_CLK_PIN;
    slot->cmd = SDMMC_CMD_PIN;
    slot->d0 = SDMMC_D0_PIN;
    slot->d1 = width == 4 ? SDMMC_D1_PIN : GPIO_NUM_NC;
    slot->d2 = width == 4 ? SDMMC_D2_PIN : GPIO_NUM_NC;
    slot->d3 = width == 4 ? SDMMC_D3_PIN : GPIO_NUM_NC;
    slot->cd = GPIO_NUM_NC;
    slot->wp = GPIO_NUM_NC;
    // The carrier does not guarantee external pull-ups on CMD/DAT.
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    if (width == 1) {
        host->flags &= ~(SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_8BIT);
        host->flags |= SDMMC_HOST_FLAG_1BIT;
    } else {
        host->flags &= ~(SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_8BIT);
        host->flags |= SDMMC_HOST_FLAG_4BIT;
    }
}

// CMD13 + one real sector read + a FAT root listing: the three things a broken
// card or a half-negotiated bus gets wrong while still reporting a successful
// mount.
static bool verify_mounted_card(void) {
    if (sdmmc_get_status(s_card) != ESP_OK) {
        ESP_LOGW(TAG, "verify: CMD13 failed");
        return false;
    }
    if (s_card->csd.sector_size != 512) {
        ESP_LOGW(TAG, "verify: unexpected sector size %u", (unsigned)s_card->csd.sector_size);
        return false;
    }
    if (sdmmc_read_sectors(s_card, s_sd_dma, 0, 1) != ESP_OK) {
        ESP_LOGW(TAG, "verify: sector read failed");
        return false;
    }
    const BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv == 0xFF) {
        ESP_LOGW(TAG, "verify: card has no FatFs drive");
        return false;
    }
    char path[8];
    snprintf(path, sizeof(path), "%u:", (unsigned)pdrv);
    FF_DIR root;
    if (f_opendir(&root, path) != FR_OK) {
        ESP_LOGW(TAG, "verify: FAT root open failed");
        return false;
    }
    FILINFO entry;
    const int read_error = f_readdir(&root, &entry);
    const int close_error = f_closedir(&root);
    return read_error == FR_OK && close_error == FR_OK;
}

bool cpyos_sd_mount(void) {
    if (s_card != nullptr) {
        return true;
    }

    // High speed four-bit first (twice: a cold card often needs the second
    // probe), then step the clock down, then fall back to one data line.
    static const struct {
        int khz;
        int width;
    } kProfiles[] = {
            {40000, 4}, {40000, 4}, {20000, 4}, {10000, 4}, {5000, 4}, {1000, 4}, {5000, 1}, {1000, 1},
    };
    const size_t profileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 16,
        .allocation_unit_size = 16 * 1024,
    };

    MetalioSd_ReleaseHold();
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    const esp_err_t dma_error = prepare_sd_dma(&host);
    if (dma_error != ESP_OK) {
        return false;
    }
    const int native_flags = host.flags;

    esp_err_t err = ESP_FAIL;
    for (size_t attempt = 0; attempt < profileCount; ++attempt) {
        const int khz = kProfiles[attempt].khz;
        const int width = kProfiles[attempt].width;
        host.flags = native_flags;
        sdmmc_slot_config_t slot;
        configure_sd_bus(&host, &slot, khz, width);

        s_card = nullptr;
        err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &cfg, &s_card);
        if (err == ESP_OK) {
            if (!verify_mounted_card()) {
                // Nothing has been published to consumers yet, so unmounting
                // here is safe and the ladder can keep stepping down.
                esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
                s_card = nullptr;
                err = ESP_FAIL;
            } else {
                ESP_LOGI(TAG, "mounted %d-bit SD at %d kHz (attempt %u)", width, khz, (unsigned)attempt + 1);
                sdmmc_card_print_info(stdout, s_card);
                return true;
            }
        }
        ESP_LOGW(TAG, "%d-bit SD mount at %d kHz failed: %s (attempt %u)", width, khz, esp_err_to_name(err),
                 (unsigned)attempt + 1);
        s_card = nullptr;
        if (err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_ARG) {
            break;
        }
        // The failed call already released its own host; give the card bus time
        // to settle instead of tearing the host down from here.
        if (attempt + 1 < profileCount) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    ESP_LOGE(TAG, "SD mount failed for every profile");
    return false;
}
