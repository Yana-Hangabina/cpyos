#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Speaker / microphone path for the Metalio E-INK4 Plus.
 *
 * The board has no codec the ESP drives directly: a BH1098G Bluetooth audio SoC
 * owns the I2S clocks and the analog path. Audio only works after the ESP puts
 * it in "mode 1" over UART1, which is exactly what the vendor firmware does
 * (metalio-vendor: main/boards/metalio-e-ink4-plus/periph/metalio_audio.cc and
 * main/boards/common/audio/bt_audio_codec.cc, MIT, (c) 2025 Shenzhen Xinzhi
 * Future Technology Co., Ltd. / Project Contributors). The command sequence,
 * the 700 ms power settle, the 32-bit stereo slave I2S format and the >>12
 * output scaling below are ported from those files unchanged.
 *
 * Pins (vendor config.h): UART1 TX=GPIO39 RX=GPIO38 115200; I2S BCLK=42
 * WS=43 DOUT=44 DIN=45; PA enable = TCA9555 P0.4 (NS4150B), active high.
 */

typedef enum {
    PLUS_AUDIO_NOT_READY = 0, /* worker not started or BT rail unpowered */
    PLUS_AUDIO_INITIALIZING,
    PLUS_AUDIO_READY,
    PLUS_AUDIO_FAILED, /* this power cycle gave up after the retries */
} plus_audio_state_t;

/* Start the background mode handshake (BT power reset, AT+RX=2, AT+MODE=1).
 * Returns immediately; poll plus_audio_wait_ready() before streaming. */
esp_err_t plus_audio_init(void);

plus_audio_state_t plus_audio_state(void);
bool plus_audio_mode_ready(void);

/* Restart a handshake that finished in PLUS_AUDIO_FAILED. The I2S DMA pool can
 * be exhausted by a concurrent Wi-Fi scan (i2s_alloc_dma_desc: NO_MEM), which
 * used to disable audio for the rest of the power cycle. Returns ESP_OK when a
 * retry was started (or audio is already up), ESP_ERR_INVALID_STATE otherwise. */
esp_err_t plus_audio_retry(void);

/* Wait until the mode is confirmed. Starts the handshake on first use.
 * Returns false on timeout or when this power cycle already failed. */
bool plus_audio_wait_ready(int timeout_ms);

/* Amplifier enable (TCA P0.4). Callers that stream PCM get it toggled for them
 * by plus_audio_write_pcm(). */
esp_err_t plus_audio_pa(bool on);

/* Speaker: mono int16 PCM. The sample rate must be plus_audio_sample_rate()
 * (the BH1098 provides a fixed 16 kHz clock in mode 1). Returns frames
 * written, or -1 when the path is not ready. */
int plus_audio_write_pcm(const int16_t* mono, int samples);

/* Microphone: fills interleaved int16 stereo frames (mic + reference), which is
 * what the vendor codec returns; returns frames read, 0 on timeout, -1 when
 * not ready. */
int plus_audio_read_mic(int16_t* interleaved, int frames);

int plus_audio_sample_rate(void);

/* Output volume 0..100 (vendor mapping: gain = (v/100)^2 * 65536). */
void plus_audio_set_volume(int percent);

/* Drop the mode claim after the BT rail was cut (sleep / power off). */
void plus_audio_shutdown(void);

/* Free the I2S channels' DMA memory before a network session (esp_wifi_init
 * needs contiguous internal/DMA blocks); the next PCM write re-creates them. */
void plus_audio_release_i2s(void);

/* ------------------------------------------------------- Bluetooth AT ----
 *
 * The BH1098G is also the board's Bluetooth audio SoC, and it is controlled
 * entirely over the same UART1 the mode handshake uses. The vendor firmware
 * drives it from a settings page (metalio-vendor: bluetooth_uart.cc /
 * bluetooth_screen.cc, MIT); the command set below is that page's, unchanged:
 *
 *   mode 1   AT+RX=2 (700 ms) AT+MODE=1   receive: I2S audio sink (boot default)
 *   mode 2   AT+TX=1 (700 ms) AT+MODE=2   transmit/pair: scan + connect a headset
 *   mode 3   AT+RX=1 (700 ms) AT+MODE=3   music sink (the device is the speaker)
 *   inquiry  AT+INQUIRING                 answers "INQUIRING START", then one
 *                                         "AT+BT:<12 hex><name>" per device and
 *                                         "INQ COMPLETE" when it ends
 *   connect  AT+CONNECT=<12 hex>          "CONNECTING" / "CONNECT SUCCESS" /
 *                                         "CONNECT TIMEOUT"
 *   profile  AT+BTSCO=1 call, AT+BTSCO=0 music, AT+PP=1 play/pause (AVRCP)
 *
 * Every call below takes one mutex, so AT traffic is serialised even when a
 * sound cue is streaming. None of them may run while the mode worker is
 * handshaking: they return ESP_ERR_INVALID_STATE in that window instead of
 * interleaving two command sequences.
 */

/* Write one line ("AT+..." + CRLF) and collect the reply lines for
 * timeout_ms. `out` (optional) receives the lines joined by '\n', always
 * NUL-terminated and truncated to `cap`. Pass cmd == NULL to only drain what
 * arrived (that is how scan results that show up after AT+INQUIRING are
 * collected). Returns ESP_OK when lines came back and none said ERROR/FAIL,
 * ESP_ERR_TIMEOUT when nothing came back within the window, ESP_FAIL on a
 * write error or an ERROR/FAIL reply, ESP_ERR_INVALID_STATE during a handshake.
 * Lines may include an echo of the command, as on the vendor UART. */
esp_err_t plus_audio_at(const char* cmd, char* out, size_t cap, int timeout_ms);

/* Vendor inquiry. Start sends AT+INQUIRING and is refused unless the module is
 * in mode 2, exactly like the vendor page ("need mode 2").
 * AMBIGUITY: the vendor has no inquiry-abort command - the module ends the scan
 * itself with "INQ COMPLETE" - so stop only clears the local flag and drains
 * the channel; no AT line is invented here. */
esp_err_t plus_audio_bt_scan_start(void);
esp_err_t plus_audio_bt_scan_stop(void);
bool plus_audio_bt_scan_active(void);

/* AT+CONNECT=<addr> with the vendor's 12 hex digits and no separators. */
esp_err_t plus_audio_bt_connect(const char* addr);

/* Headset profile split (vendor call/music buttons): true = AT+BTSCO=1,
 * false = AT+BTSCO=0. */
esp_err_t plus_audio_bt_sco(bool call);
/* AT+PP=1: AVRCP play/pause toggle. */
esp_err_t plus_audio_bt_play_pause(void);

/* Switch the module to `mode` (1..3). Stops the I2S stream and the amplifier
 * first - so a sound cue can never straddle a mode change - sends the vendor
 * command pair, and re-enables I2S for mode 1. Blocks ~0.8-1.8 s; call it from
 * a worker or at a page boundary, not from the render path. Restoring mode 1
 * is what leaving the Bluetooth audio page does. */
esp_err_t plus_audio_bt_set_mode(int mode);
/* Last mode the module accepted, 0 when unknown (handshake not finished). */
int plus_audio_bt_mode(void);

#ifdef __cplusplus
}
#endif
