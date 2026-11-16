// USB mass storage for the Metalio Plus (single LUN over the microSD card).
//
// One-way handoff: the app's FAT/VFS is detached without deinitializing the
// SDMMC host or card, then TinyUSB exposes the raw card. There is no remount
// path — the shell freezes storage use and a reboot restores normal operation.
//
// The USB-Serial/JTAG console may disappear once the OTG peripheral takes over
// the PHY; that is expected (power-cycle to recover), exactly like the
// reference firmware's U-disk page.
//
// UNTESTED ON HARDWARE.

#include "usb_msc.h"

#include <atomic>

#include <esp_log.h>
#include <tinyusb.h>
#include <tinyusb_msc.h>

#include "cpyos_sd.h"
extern "C" {
#include "epd_board_metalio_eink4_plus.h"
}

namespace usb_msc {
namespace {

constexpr const char* TAG = "PLUS-MSC";

tinyusb_msc_storage_handle_t s_storage = nullptr;
bool s_started = false;
std::atomic<bool> s_hostEjected{false};

// The host dropping the volume (eject, or a clean unmount before unplug) makes
// esp_tinyusb remount the filesystem on the app side. That is the safe point to
// leave MSC mode; Read Pico reboots here too, so the console/PHY come back.
void on_storage_event(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t* event, void* arg) {
    (void)handle;
    (void)arg;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE &&
        event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        ESP_LOGW(TAG, "host released the volume; rebooting to restore the app");
        s_hostEjected.store(true);
    }
}

void teardown() {
    if (s_storage != nullptr) {
        tinyusb_msc_delete_storage(s_storage);
        s_storage = nullptr;
    }
    tinyusb_msc_uninstall_driver();
    tinyusb_driver_uninstall();
    // Return the PHY to USB-Serial/JTAG. The FAT side stays detached; the shell
    // requires a reboot after any MSC session.
    epd_board_metalio_eink4_plus_ana_sw_set(true);
    s_started = false;
}

}  // namespace

bool start() {
    if (s_started) return true;
    sdmmc_card_t* card = cpyos_sd_card();
    if (card == nullptr) {
        ESP_LOGE(TAG, "no mounted SD card to expose");
        return false;
    }
    if (!cpyos_sd_release_for_usb()) {
        ESP_LOGE(TAG, "could not detach the app filesystem");
        return false;
    }

    // Vendor wiring: the ESP32-S31 exposes HS UTMI through the ANA_SW analog
    // switch on GPIO36. It idles high (USB-Serial/JTAG); MSC must pull it low
    // before TinyUSB takes the PHY. This esp_tinyusb revision declares only
    // TINYUSB_PORT_FULL_SPEED_0 on S31 (HIGH_SPEED_0 is compiled only when
    // SOC_USB_OTG_PERIPH_NUM > 1), so that is the port we must pass. The vendor
    // advertises HS; verify the negotiated speed on hardware.
    epd_board_metalio_eink4_plus_ana_sw_set(false);
    tinyusb_config_t config = {};
    config.port = TINYUSB_PORT_FULL_SPEED_0;
    // The vendor MSC task is 16 KiB; MSC transactions run FAT + TinyUSB on the
    // same stack, so 4 KiB is not enough for safe bursts.
    config.task.size = 16384;
    config.task.priority = 5;
    config.task.xCoreID = 0;
    if (tinyusb_driver_install(&config) != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed");
        epd_board_metalio_eink4_plus_ana_sw_set(true);
        return false;
    }

    tinyusb_msc_driver_config_t driver = {};
    if (tinyusb_msc_install_driver(&driver) != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_install_driver failed");
        tinyusb_driver_uninstall();
        epd_board_metalio_eink4_plus_ana_sw_set(true);
        return false;
    }

    tinyusb_msc_set_storage_callback(on_storage_event, nullptr);
    tinyusb_msc_storage_config_t storage = {};
    storage.medium.card = card;
    storage.fat_fs.config.format_if_mount_failed = false;
    storage.fat_fs.config.max_files = 4;
    storage.fat_fs.config.allocation_unit_size = 0;
    storage.fat_fs.config.disk_status_check_enable = false;
    storage.fat_fs.do_not_format = true;
    storage.fat_fs.format_flags = 0;
    storage.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    if (tinyusb_msc_new_storage_sdmmc(&storage, &s_storage) != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc failed");
        tinyusb_msc_uninstall_driver();
        tinyusb_driver_uninstall();
        epd_board_metalio_eink4_plus_ana_sw_set(true);
        return false;
    }
    s_started = true;
    ESP_LOGW(TAG, "SD exposed to USB; app storage is detached until reboot");
    return true;
}

void stop() { teardown(); }

bool running() { return s_started; }

bool hostEjected() { return s_hostEjected.load(); }

}  // namespace usb_msc
