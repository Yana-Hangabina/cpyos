#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GC2145 DVP still camera for the Metalio E-INK4 Plus.
 *
 * Ported from the vendor firmware (main/boards/common/camera/dvp_camera.cc +
 * gc2145_vga_settings.h, Apache-2.0, (c) 2025 Espressif Systems (Shanghai) CO
 * LTD for the sensor register table): same 640x480 RGB565 table, same eight
 * warm-up frames, same RGB565X software byte swap, same hardware JPEG encoder
 * at quality 80.
 *
 * ---- LCD_CAM arbitration (read before calling) ----
 * On this SoC the DVP camera and the parallel EPD share one LCD_CAM block:
 * esp_hal_cam/esp32s31/cam_ll.h resets *both* halves through
 * `HP_SYS_CLKRST.lcdcam_ctrl0.reg_lcdcam_rst_en`, and
 * `esp_cam_ctlr_dvp_deinit()` turns the shared bus clock off. The two can
 * therefore not be initialised at the same time. Every entry point below does
 * the hand-off itself, around the whole camera session:
 *
 *   epd_board_metalio_eink4_plus_bus_hold_low()   // epd_lcd_deinit() + EPD pins low
 *   ... camera owns LCD_CAM ...
 *   epd_board_metalio_eink4_plus_bus_restore()    // epd_lcd_init() again
 *
 * That pair is the port's own sleep/wake path, so it is known to bring the
 * panel back. What it cannot do is repaint: the panel had its bus pins driven
 * low while the camera ran.
 *
 * CALLER CONTRACT
 *   1. Hold the render lock (RenderLock) so no panel push can start.
 *   2. Wait for any in-flight scan (HalDisplay::waitForScanIdle) first.
 *   3. After the call returns -- success or failure -- re-push the page with
 *      HalDisplay::FULL_REFRESH (a differential push is not trustworthy while
 *      the bus was detached).
 * Calling without 1..2 is a data race on LCD_CAM; calling without 3 leaves the
 * glass on a frame the firmware no longer knows about.
 */

/* Read the sensor id over SCCB (0x3C on I2C1, page 0). Creates and destroys
 * the DVP controller only to have MCLK, exactly like the vendor's Detect().
 * ESP_ERR_NOT_FOUND: no ACK, or the PID is not GC2145 (0x2145). */
esp_err_t plus_camera_probe(void);

/* PID observed by the last successful probe (0 before the first probe). */
uint16_t plus_camera_last_pid(void);

/* Capture one still and encode it as JPEG. `*out` is a heap buffer the caller
 * frees with heap_caps_free(); it lives in PSRAM (MALLOC_CAP_SPIRAM). */
esp_err_t plus_camera_capture_jpeg(uint8_t** out, size_t* len);

/* Drop any half-open session (controller, frame, shared-reset lease) and make
 * sure the panel has its LCD_CAM back. Idempotent; safe to call on exit. */
void plus_camera_release(void);

#ifdef __cplusplus
}
#endif
