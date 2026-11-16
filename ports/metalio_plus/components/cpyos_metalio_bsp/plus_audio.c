#include "plus_audio.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "config.h"
#include "epd_board_metalio_eink4_plus.h"

/* Ported from the vendor firmware (MIT): metalio_audio.cc / bt_audio_codec.cc.
 * The command sequence, retry count, settle times and I2S slot format are
 * identical; only the plumbing (no SimpleUart observer, no AudioCodec base
 * class) is CPYOS-specific. */

#define TAG "PLUS-AUD"

#define AUDIO_UART UART_NUM_1
#define AUDIO_UART_BAUD 115200

#define AUDIO_I2S_PORT I2S_NUM_0
#define AUDIO_DMA_DESC_NUM 4
#define AUDIO_DMA_FRAME_NUM 240

static const int kSampleRate = 16000;
static const int kRetries = 3;
static const int kAckTimeoutMs = 1000;
static const int kBootSettleMs = 700;
/* The vendor waits this long between the "what to expect" command (AT+RX / AT+TX)
 * and the mode command itself; the module needs it to settle its audio path. */
static const int kModeGapMs = 700;

static atomic_int s_state = PLUS_AUDIO_NOT_READY;
static atomic_bool s_worker_started = false;
static atomic_bool s_worker_done = false;
static atomic_int s_volume = 80;

/* Bluetooth AT channel state. s_bt_mode is 0 until the handshake accepted mode 1;
 * every mode switch updates it only after the module acknowledged. */
static atomic_int s_bt_mode = 0;
static atomic_bool s_scan_active = false;
/* Non-zero while a PCM write/read is inside I2S, so a mode switch can wait for
 * the cue to finish instead of cutting the DMA mid-frame. */
static atomic_int s_stream_ops = 0;

static bool s_uart_ok = false;
static SemaphoreHandle_t s_at_mutex = NULL;

static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
static bool s_i2s_ok = false;

/* ---------------------------------------------------------------- UART ---- */

static bool uart_open(void) {
    if (s_uart_ok) return true;
    uart_config_t cfg = {
        .baud_rate = AUDIO_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* Already installed by an earlier caller: only (re)apply the parameters. */
    if (uart_is_driver_installed(AUDIO_UART) ||
        uart_driver_install(AUDIO_UART, 2048, 0, 0, NULL, 0) == ESP_OK) {
        if (uart_param_config(AUDIO_UART, &cfg) != ESP_OK) return false;
        if (uart_set_pin(AUDIO_UART, BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN, UART_PIN_NO_CHANGE,
                         UART_PIN_NO_CHANGE) != ESP_OK) {
            return false;
        }
        s_uart_ok = true;
    }
    return s_uart_ok;
}

static void uart_drain(void) {
    uint8_t scratch[64];
    while (uart_read_bytes(AUDIO_UART, scratch, sizeof(scratch), 0) > 0) {
    }
}

static bool uart_send(const char* text) {
    const int n = uart_write_bytes(AUDIO_UART, text, strlen(text));
    return n == (int)strlen(text);
}

/* Collects reply lines for `window_ms` and reports whether any line contains
 * `want`; the vendor accepts "SET MODE 1" followed by end-of-line or a space
 * because the module appends an echo of the command. */
static bool uart_expect(const char* want, int window_ms, bool* saw_error) {
    char line[128];
    size_t len = 0;
    bool overflow = false;
    bool found = false;
    if (saw_error) *saw_error = false;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(window_ms);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t c = 0;
        const int n = uart_read_bytes(AUDIO_UART, &c, 1, pdMS_TO_TICKS(20));
        if (n != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[len] = '\0';
            if (!overflow) {
                if (want != NULL && strstr(line, want) != NULL) found = true;
                if (saw_error && (strstr(line, "ERROR") != NULL || strstr(line, "FAIL") != NULL)) {
                    *saw_error = true;
                }
            }
            len = 0;
            overflow = false;
        } else if (c >= 32 && c < 127) {
            if (len + 1 < sizeof(line)) {
                line[len++] = (char)c;
            } else {
                overflow = true;
            }
        }
    }
    return found;
}

static bool at_mutex_ensure(void);

static bool apply_mode(void) {
    /* Share the AT mutex with the public API: a page could otherwise open the
     * channel while the boot handshake is mid-sequence. */
    const bool locked = at_mutex_ensure() && xSemaphoreTake(s_at_mutex, pdMS_TO_TICKS(3000)) == pdTRUE;
    uart_drain();
    bool ok = false;
    do {
        if (!uart_send("AT+RX=2\r\n")) break;
        vTaskDelay(pdMS_TO_TICKS(700));
        bool error = false;
        (void)uart_expect(NULL, 50, &error);
        if (error) {
            ESP_LOGW(TAG, "AT+RX=2 answered ERROR");
            break;
        }
        uart_drain();
        if (!uart_send("AT+MODE=1\r\n")) break;
        ok = uart_expect("SET MODE 1", kAckTimeoutMs, NULL);
    } while (false);
    if (locked) xSemaphoreGive(s_at_mutex);
    return ok;
}

/* ------------------------------------------------------------ AT channel -- */

/* One command per line; the caller's string may or may not carry the CRLF. */
static bool uart_send_line(const char* cmd) {
    const size_t len = strlen(cmd);
    if (uart_write_bytes(AUDIO_UART, cmd, len) != (int)len) return false;
    const bool has_eol = len > 0 && (cmd[len - 1] == '\n');
    if (has_eol) return true;
    return uart_write_bytes(AUDIO_UART, "\r\n", 2) == 2;
}

/* Reply lines are accumulated into `out` separated by '\n'. Longer-than-buffer
 * lines are truncated but still counted, which is enough for the vendor's
 * "AT+BT:<12 hex><name>" records (name <= 64 bytes). */
typedef struct {
    char* out;
    size_t cap;
    size_t used;
    int lines;
    bool error;
} at_sink_t;

static void at_sink_line(at_sink_t* sink, char* line, size_t len) {
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) --len;
    size_t start = 0;
    while (start < len && line[start] == ' ') ++start;
    if (start >= len) return; /* blank keep-alive line */
    ++sink->lines;
    if (strstr(line + start, "ERROR") != NULL || strstr(line + start, "FAIL") != NULL) {
        sink->error = true;
    }
    if (sink->out == NULL || sink->cap < 2) return;
    /* Always keep room for the separator and the terminator, so a tiny buffer
     * degrades to "one short line" instead of writing past the end. */
    if (sink->used + 2 > sink->cap) return;
    const size_t room = sink->cap - sink->used - 2;
    size_t n = len - start;
    if (n > room) n = room;
    if (n > 0) {
        memcpy(sink->out + sink->used, line + start, n);
        sink->used += n;
    }
    sink->out[sink->used++] = '\n';
    sink->out[sink->used] = '\0';
}

/* A polling drain can land in the middle of a line; without carrying the tail the
 * next sweep would parse a fragment ("me>") as if it were a whole reply. Only the
 * drain path uses this: a command exchange always starts from a clean buffer. */
static char s_at_partial[160];
static size_t s_at_partial_len = 0;

static void at_collect(int window_ms, at_sink_t* sink) {
    char line[160];
    size_t len = 0;
    bool overflow = false;
    const bool drain_only = window_ms <= 0;
    if (!drain_only) s_at_partial_len = 0;
    if (drain_only && s_at_partial_len > 0) {
        memcpy(line, s_at_partial, s_at_partial_len);
        len = s_at_partial_len;
        s_at_partial_len = 0;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(drain_only ? 0 : window_ms);
    for (;;) {
        if (!drain_only && (int32_t)(deadline - xTaskGetTickCount()) <= 0) break;
        uint8_t c = 0;
        /* A zero wait is a single non-blocking sweep of whatever the ring holds,
         * which is what the callers' polling drain wants. */
        const int n = uart_read_bytes(AUDIO_UART, &c, 1, drain_only ? 0 : pdMS_TO_TICKS(20));
        if (n != 1) {
            if (drain_only) break;
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (!overflow) {
                at_sink_line(sink, line, len);
            } else {
                ESP_LOGW(TAG, "AT line longer than %u bytes, dropped", (unsigned)sizeof(line));
            }
            len = 0;
            overflow = false;
        } else if (c >= 32 && c < 127) {
            if (len + 1 < sizeof(line)) {
                line[len++] = (char)c;
            } else {
                overflow = true;
            }
        }
    }
    if (drain_only && !overflow && len > 0 && len < sizeof(s_at_partial)) {
        memcpy(s_at_partial, line, len);
        s_at_partial_len = len;
    }
}

static bool at_mutex_ensure(void) {
    if (s_at_mutex != NULL) return true;
    s_at_mutex = xSemaphoreCreateMutex();
    return s_at_mutex != NULL;
}

/* Caller holds s_at_mutex. */
static esp_err_t at_locked(const char* cmd, char* out, size_t cap, int timeout_ms) {
    /* The boot handshake owns the UART while it runs; interleaving a second
     * command sequence with it is exactly what "SET MODE" retries can break. */
    if (plus_audio_state() == PLUS_AUDIO_INITIALIZING) return ESP_ERR_INVALID_STATE;
    if (!uart_open()) {
        ESP_LOGE(TAG, "AT: UART unavailable");
        return ESP_FAIL;
    }
    if (out != NULL && cap > 0) out[0] = '\0';
    if (cmd != NULL) {
        ESP_LOGI(TAG, "TX: %s", cmd);
        if (!uart_send_line(cmd)) {
            ESP_LOGE(TAG, "AT: write failed");
            return ESP_FAIL;
        }
    }
    at_sink_t sink = {.out = out, .cap = cap, .used = 0, .lines = 0, .error = false};
    at_collect(timeout_ms, &sink);
    if (sink.out != NULL && sink.cap > 0 && sink.used == 0) sink.out[0] = '\0';
    if (sink.lines > 0) ESP_LOGI(TAG, "RX: %s", sink.out != NULL ? sink.out : "(lines)");
    if (sink.error) return ESP_FAIL;
    if (sink.lines == 0) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

esp_err_t plus_audio_at(const char* cmd, char* out, size_t cap, int timeout_ms) {
    if (!at_mutex_ensure()) return ESP_FAIL;
    if (xSemaphoreTake(s_at_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "AT: busy");
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = at_locked(cmd, out, cap, timeout_ms);
    xSemaphoreGive(s_at_mutex);
    return err;
}

/* The three vendor mode sequences, in module order. */
static const char* const kModePre[3] = {"AT+RX=2", "AT+TX=1", "AT+RX=1"};
static const char* const kModeCmd[3] = {"AT+MODE=1", "AT+MODE=2", "AT+MODE=3"};
static const char* const kModeAck[3] = {"SET MODE 1", "SET MODE 2", "SET MODE 3"};

static void i2s_stream_stop(void);
static bool i2s_stream_start(void);

/* Waits (bounded) for an in-flight cue so the DMA never straddles the switch.
 * Sound cues are well under a second; the cap is a safety net, not a budget. */
static void stream_quiesce(int cap_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(cap_ms);
    while (atomic_load(&s_stream_ops) > 0 && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (atomic_load(&s_stream_ops) > 0) ESP_LOGW(TAG, "stream still busy; switching anyway");
}

esp_err_t plus_audio_bt_set_mode(int mode) {
    if (mode < 1 || mode > 3) return ESP_ERR_INVALID_ARG;
    const plus_audio_state_t st = plus_audio_state();
    if (st == PLUS_AUDIO_INITIALIZING) return ESP_ERR_INVALID_STATE;
    if (mode == 1 && st != PLUS_AUDIO_READY) {
        /* Nothing to restore yet: the boot handshake has not run or it failed.
         * Starting it leaves the module in mode 1 as a side effect. */
        atomic_store(&s_bt_mode, 0);
        plus_audio_retry();
        return plus_audio_init();
    }
    if (!at_mutex_ensure()) return ESP_FAIL;
    if (xSemaphoreTake(s_at_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    /* Stop the audio path before touching the mode: the module stops clocking
     * I2S in modes 2/3, and the vendor never switches while streaming. */
    stream_quiesce(1000);
    (void)plus_audio_pa(false);
    i2s_stream_stop();
    atomic_store(&s_bt_mode, 0);

    esp_err_t err = at_locked(kModePre[mode - 1], NULL, 0, 200);
    if (err == ESP_OK || err == ESP_ERR_TIMEOUT) {
        vTaskDelay(pdMS_TO_TICKS(kModeGapMs));
        char reply[128];
        err = at_locked(kModeCmd[mode - 1], reply, sizeof(reply), kAckTimeoutMs);
        if (err == ESP_OK && strstr(reply, kModeAck[mode - 1]) == NULL) {
            ESP_LOGW(TAG, "%s: no \"%s\" ack", kModeCmd[mode - 1], kModeAck[mode - 1]);
            err = ESP_ERR_TIMEOUT;
        }
    }
    if (err == ESP_OK) {
        atomic_store(&s_bt_mode, mode);
        if (mode == 1) {
            /* I2S clocks are back; re-enable the stream so sound feedback works
             * again. A failure here leaves the module claim intact. */
            if (!i2s_stream_start()) ESP_LOGE(TAG, "I2S restart failed after mode 1");
        }
        ESP_LOGI(TAG, "BH1098 mode %d", mode);
    } else {
        ESP_LOGE(TAG, "mode %d switch failed: %s", mode, esp_err_to_name(err));
    }
    xSemaphoreGive(s_at_mutex);
    return err;
}

int plus_audio_bt_mode(void) { return atomic_load(&s_bt_mode); }

esp_err_t plus_audio_bt_scan_start(void) {
    if (atomic_load(&s_bt_mode) != 2) {
        /* Vendor page refuses to scan outside mode 2 ("need mode 2"). */
        ESP_LOGW(TAG, "scan refused: module is in mode %d", atomic_load(&s_bt_mode));
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_scan_active, true);
    const esp_err_t err = plus_audio_at("AT+INQUIRING", NULL, 0, 300);
    if (err == ESP_FAIL) atomic_store(&s_scan_active, false);
    return err;
}

esp_err_t plus_audio_bt_scan_stop(void) {
    if (!atomic_load(&s_scan_active)) return ESP_OK;
    atomic_store(&s_scan_active, false);
    /* No abort command exists in the vendor protocol: the module ends the
     * inquiry on its own ("INQ COMPLETE"). Just drain what is still queued so a
     * fresh scan does not inherit stale "AT+BT:" lines. */
    plus_audio_at(NULL, NULL, 0, 100);
    ESP_LOGI(TAG, "scan stopped locally (module ends the inquiry itself)");
    return ESP_OK;
}

bool plus_audio_bt_scan_active(void) { return atomic_load(&s_scan_active); }

esp_err_t plus_audio_bt_connect(const char* addr) {
    if (addr == NULL) return ESP_ERR_INVALID_ARG;
    size_t n = 0;
    for (const char* p = addr; *p != '\0'; ++p, ++n) {
        const bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F');
        if (!hex) return ESP_ERR_INVALID_ARG;
        if (n >= 12) return ESP_ERR_INVALID_ARG; /* vendor address is exactly 12 digits */
    }
    if (n != 12) return ESP_ERR_INVALID_ARG;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CONNECT=%s", addr);
    return plus_audio_at(cmd, NULL, 0, 400);
}

esp_err_t plus_audio_bt_sco(bool call) {
    return plus_audio_at(call ? "AT+BTSCO=1" : "AT+BTSCO=0", NULL, 0, 400);
}

esp_err_t plus_audio_bt_play_pause(void) { return plus_audio_at("AT+PP=1", NULL, 0, 400); }

static bool i2s_ensure(void);  /* defined below; the worker initialises it */

/* The P14 rail is shared by 4G, BT and the PA, and the BH1098 power reset cuts
 * it for ~1 s. The official board does not refuse audio while 4G is up: it
 * stops the modem before the reset and re-initialises it on the rising edge
 * (Nt26Board::PreparePowerOff / RecoverAfterPowerOnAsync). The cellular
 * component provides both hooks; without it they are no-ops. */
void plus_audio_prepare_rail_cycle(void) __attribute__((weak));
void plus_audio_rail_cycle_done(void) __attribute__((weak));

static void mode_worker(void* arg) {
    (void)arg;
    epd_board_metalio_eink4_plus_pa_set(false);

    if (plus_audio_prepare_rail_cycle != NULL) {
        plus_audio_prepare_rail_cycle();
    }

    /* The rail is already on (BSP default); the reset gives the module a known
     * state and takes ~1.5 s, so it runs once per power cycle, not per retry. */
    bool powered = epd_board_metalio_eink4_plus_bt_power_reset() == ESP_OK;
    if (plus_audio_rail_cycle_done != NULL) {
        /* P14 is high again: let the modem re-attach in the background while
         * this worker finishes the BT handshake. */
        plus_audio_rail_cycle_done();
    }
    if (!powered) {
        ESP_LOGE(TAG, "BT power reset failed");
        atomic_store(&s_state, PLUS_AUDIO_FAILED);
        atomic_store(&s_worker_done, true);
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(kBootSettleMs));

    bool ok = false;
    for (int attempt = 0; attempt <= kRetries; ++attempt) {
        if (apply_mode()) {
            ok = true;
            break;
        }
        ESP_LOGW(TAG, "audio mode init failed (attempt %d/%d)", attempt + 1, kRetries + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (ok) {
        /* Create/enable the I2S channels here, not on the caller: driver init
         * needs ~4 KiB of stack and the bench console task is only 12 KiB. */
        ok = i2s_ensure();
        if (!ok) ESP_LOGE(TAG, "I2S init failed after mode 1");
    }
    /* Mode before state: plus_audio_write_pcm() gates on s_bt_mode, so a cue can
     * never observe READY with an unknown mode in between. */
    if (ok) atomic_store(&s_bt_mode, 1);
    atomic_store(&s_state, ok ? PLUS_AUDIO_READY : PLUS_AUDIO_FAILED);
    atomic_store(&s_worker_done, true);
    if (ok) {
        ESP_LOGI(TAG, "BH1098 mode 1 ready (%d Hz, I2S slave)", kSampleRate);
    } else {
        ESP_LOGE(TAG, "audio unavailable after initialization");
    }
    vTaskDelete(NULL);
}

esp_err_t plus_audio_init(void) {
    // The BH1098 power reset writes the shared TCA9555 ports; running it while
    // display.begin() powers the EPD PMIC made the TPS probe fail ("rails off")
    // and the first boot frame scanned unpowered. Refuse early calls and let the
    // shell start this once the boot frame is on the panel.
    if (esp_timer_get_time() < 9000LL * 1000) {
        return ESP_OK;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_worker_started, &expected, true)) {
        return ESP_OK;
    }
    atomic_store(&s_state, PLUS_AUDIO_INITIALIZING);
    atomic_store(&s_worker_done, false);
    if (!uart_open()) {
        ESP_LOGE(TAG, "audio UART init failed");
        atomic_store(&s_state, PLUS_AUDIO_FAILED);
        atomic_store(&s_worker_done, true);
        return ESP_FAIL;
    }
    /* Internal RAM stack: this worker never touches internal flash, but keeping
     * it out of PSRAM matches the rest of the port's short-lived workers. */
    if (xTaskCreate(mode_worker, "plus-audio", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "audio worker creation failed");
        atomic_store(&s_state, PLUS_AUDIO_FAILED);
        atomic_store(&s_worker_done, true);
        return ESP_FAIL;
    }
    return ESP_OK;
}

plus_audio_state_t plus_audio_state(void) {
    return (plus_audio_state_t)atomic_load(&s_state);
}

esp_err_t plus_audio_retry(void) {
    /* The worker is one-shot: a failed attempt (e.g. I2S DMA refused while a
     * Wi-Fi scan holds internal DMA) would otherwise leave audio dead for the
     * whole power cycle. Only a finished, failed attempt may be restarted; an
     * in-flight handshake must not be duplicated. */
    const plus_audio_state_t st = plus_audio_state();
    if (st == PLUS_AUDIO_INITIALIZING || st == PLUS_AUDIO_READY) return ESP_OK;
    if (st != PLUS_AUDIO_FAILED || !atomic_load(&s_worker_done)) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_worker_done, false);
    atomic_store(&s_state, PLUS_AUDIO_NOT_READY);
    atomic_store(&s_worker_started, false);
    return plus_audio_init();
}

bool plus_audio_mode_ready(void) {
    return plus_audio_state() == PLUS_AUDIO_READY;
}

bool plus_audio_wait_ready(int timeout_ms) {
    if (plus_audio_mode_ready()) return true;
    if (plus_audio_state() == PLUS_AUDIO_FAILED && atomic_load(&s_worker_done)) {
        return false;
    }
    plus_audio_init();
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 0);
    while (!plus_audio_mode_ready()) {
        if (atomic_load(&s_worker_done) && plus_audio_state() == PLUS_AUDIO_FAILED) {
            return false;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

esp_err_t plus_audio_pa(bool on) {
    return epd_board_metalio_eink4_plus_pa_set(on);
}

/* --------------------------------------------------------------- I2S ------ */

static bool i2s_ensure(void) {
    if (s_i2s_ok) return true;
    i2s_chan_config_t chan_cfg = {
        .id = AUDIO_I2S_PORT,
        .role = I2S_ROLE_SLAVE,
        .dma_desc_num = AUDIO_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
        .tx_destination = I2S_DESTINATION_DMA,
        .rx_destination = I2S_DESTINATION_DMA,
    };
    if (i2s_new_channel(&chan_cfg, &s_tx, &s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel creation failed");
        return false;
    }
    i2s_std_config_t std_cfg = {0};
    std_cfg.clk_cfg.sample_rate_hz = kSampleRate;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
#if SOC_I2S_HW_VERSION_2
    std_cfg.clk_cfg.ext_clk_freq_hz = 0;
#endif
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
#if SOC_I2S_HW_VERSION_2
    std_cfg.slot_cfg.left_align = true;
    std_cfg.slot_cfg.big_endian = false;
    std_cfg.slot_cfg.bit_order_lsb = false;
#endif
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = AUDIO_I2S_BCLK;
    std_cfg.gpio_cfg.ws = AUDIO_I2S_WS;
    std_cfg.gpio_cfg.dout = AUDIO_I2S_DOUT;
    std_cfg.gpio_cfg.din = AUDIO_I2S_DIN;

    esp_err_t err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init/enable failed: %s", esp_err_to_name(err));
        if (s_tx) {
            i2s_del_channel(s_tx);
            s_tx = NULL;
        }
        if (s_rx) {
            i2s_del_channel(s_rx);
            s_rx = NULL;
        }
        return false;
    }
    s_i2s_ok = true;
    return true;
}

/* Mode 2/3 stop the module clocking I2S; disable (not delete) the channels so a
 * later mode 1 can re-enable them without paying a new driver allocation. */
static void i2s_stream_stop(void) {
    if (!s_i2s_ok) return;
    if (s_tx) (void)i2s_channel_disable(s_tx);
    if (s_rx) (void)i2s_channel_disable(s_rx);
}

static bool i2s_stream_start(void) {
    if (!i2s_ensure()) return false;
    if (s_tx && i2s_channel_enable(s_tx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S tx enable failed");
        return false;
    }
    if (s_rx && i2s_channel_enable(s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S rx enable failed");
        return false;
    }
    return true;
}

static int32_t volume_factor(void) {
    const int v = atomic_load(&s_volume);
    const int pct = v < 0 ? 0 : (v > 100 ? 100 : v);
    /* Vendor: (volume/100)^2 * 65536, applied in the 32-bit domain. */
    return (int32_t)((int64_t)pct * pct * 65536 / 10000);
}

/* Release the I2S driver's DMA descriptors/buffers before a network session.
 * The full shell leaves only ~27 KB contiguous internal RAM at Home; audio
 * warming at boot (sound feedback) takes a chunk of the DMA pool and
 * esp_wifi_init then fails with ESP_ERR_NO_MEM. The BT module stays in mode 1,
 * so the next PCM write re-creates the channels through i2s_ensure(). */
void plus_audio_release_i2s(void) {
    if (atomic_load(&s_state) == PLUS_AUDIO_INITIALIZING) return;
    if (atomic_load(&s_stream_ops) != 0) return;
    plus_audio_pa(false);
    if (s_tx) {
        (void)i2s_channel_disable(s_tx);
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_rx) {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    s_i2s_ok = false;
}

int plus_audio_write_pcm(const int16_t* mono, int samples) {
    if (mono == NULL || samples <= 0) return -1;
    /* Only mode 1 clocks I2S. In mode 2/3 the module is a Bluetooth endpoint
     * and the local I2S channels are disabled, so a cue must not be written. */
    if (atomic_load(&s_bt_mode) != 1) return -1;
    if (!plus_audio_wait_ready(5000) || !i2s_ensure()) return -1;

    atomic_fetch_add(&s_stream_ops, 1);
    int16_t block[128];
    int32_t out[2 * 128];
    const int32_t gain = volume_factor();
    int written = 0;
    while (written < samples) {
        int n = samples - written;
        if (n > (int)(sizeof(block) / sizeof(block[0]))) n = (int)(sizeof(block) / sizeof(block[0]));
        memcpy(block, mono + written, (size_t)n * sizeof(int16_t));
        for (int i = 0; i < n; ++i) {
            int64_t v = (int64_t)block[i] * gain;
            if (v > INT32_MAX) v = INT32_MAX;
            if (v < INT32_MIN) v = INT32_MIN;
            out[2 * i] = (int32_t)v;
            out[2 * i + 1] = (int32_t)v;
        }
        size_t bytes = 0;
        const esp_err_t err = i2s_channel_write(s_tx, out, (size_t)n * 2 * sizeof(int32_t), &bytes,
                                                pdMS_TO_TICKS(250));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S write: %s", esp_err_to_name(err));
            break;
        }
        written += (int)(bytes / (2 * sizeof(int32_t)));
    }
    atomic_fetch_sub(&s_stream_ops, 1);
    return written;
}

int plus_audio_read_mic(int16_t* interleaved, int frames) {
    if (interleaved == NULL || frames <= 0) return -1;
    /* The microphone rides the same mode-1 I2S link; see plus_audio_write_pcm. */
    if (atomic_load(&s_bt_mode) != 1) return -1;
    if (!plus_audio_wait_ready(5000) || !i2s_ensure()) return -1;
    atomic_fetch_add(&s_stream_ops, 1);
    int32_t slot[2 * 256];
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > 256) n = 256;
        size_t bytes = 0;
        const esp_err_t err =
            i2s_channel_read(s_rx, slot, (size_t)n * 2 * sizeof(int32_t), &bytes, pdMS_TO_TICKS(250));
        if (err != ESP_OK) break;
        const int got = (int)(bytes / (2 * sizeof(int32_t))); /* stereo frames */
        /* Vendor: 32-bit slot -> int16 with >>12. */
        for (int i = 0; i < got * 2; ++i) {
            int32_t v = slot[i] >> 12;
            if (v > INT16_MAX) v = INT16_MAX;
            if (v < -INT16_MAX) v = -INT16_MAX;
            interleaved[(size_t)done * 2 + (size_t)i] = (int16_t)v;
        }
        done += got;
        if (got < n) break;
    }
    atomic_fetch_sub(&s_stream_ops, 1);
    return done;
}

int plus_audio_sample_rate(void) { return kSampleRate; }

void plus_audio_set_volume(int percent) {
    atomic_store(&s_volume, percent < 0 ? 0 : (percent > 100 ? 100 : percent));
}

void plus_audio_shutdown(void) {
    if (s_i2s_ok) {
        if (s_tx) i2s_channel_disable(s_tx);
        if (s_rx) i2s_channel_disable(s_rx);
    }
    plus_audio_pa(false);
    atomic_store(&s_state, PLUS_AUDIO_NOT_READY);
    atomic_store(&s_worker_started, false);
    atomic_store(&s_worker_done, false);
    atomic_store(&s_bt_mode, 0);
    atomic_store(&s_scan_active, false);
}
