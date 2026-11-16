#pragma once

#include <stdbool.h>
#include <sdmmc_cmd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the Plus microSD card (SDMMC slot 0, 4-bit) at /sdcard. */
bool cpyos_sd_mount(void);

/* True once cpyos_sd_mount() succeeded. */
bool cpyos_sd_is_mounted(void);

// Unmounts and re-runs the mount ladder after a confirmed SD write wedge.
bool cpyos_sd_remount(void);

/* Initialized card descriptor (nullptr when not mounted). */
sdmmc_card_t *cpyos_sd_card(void);

/* Detach the app's FAT/VFS from the card WITHOUT deinitializing the SDMMC host
 * or the card, so a USB MSC LUN can own the raw sectors. The app must stop
 * using /sdcard afterwards; rebooting restores normal storage. */
bool cpyos_sd_release_for_usb(void);

#ifdef __cplusplus
}
#endif
