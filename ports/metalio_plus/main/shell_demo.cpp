// CPYOS activity-shell demo for the Metalio E-INK4 Plus.
//
// This is the real CPYOS UI: ActivityManager + the CPY theme + the shared
// activities (Home/Recent, Library, Settings, Apps, Reader). It exists to prove
// the shell links and runs on the Plus. The full web server/Calibre pages are
// guarded off (CROSSPOINT_PORT_NO_NETWORK_SHELL) and replaced by a minimal HTTP
// file transfer (`web <ssid> [pass]`); USB MSC and BLE HID are real ports.
//
// UNTESTED ON HARDWARE.

#include "FontCacheManager.h"
#include "FontDecompressor.h"
#include "GfxRenderer.h"
#include <JpegToBmpConverter.h>
#include "bmp_export.h"
#include "HalClock.h"
#include "HalDisplay.h"
#include "HalGPIO.h"
#include "HalFrontlight.h"
#include "HalStorage.h"
#include "esp_private/esp_clk.h"
#include "HalTiltSensor.h"
#include "SdCardFontSystem.h"
#include "SystemFontPack.h"
#include "Typography.h"
#include "DeviceSecret.h"
#include "PortContentCrypto.h"
#include "usb_msc.h"
#if defined(CROSSPOINT_PDF_XIP)
#include "pdf/PdfAssets.h"
#include "pdf/PdfEngineUpdate.h"
#include "pdf/PdfModuleRuntime.h"
#endif
#include "web_transfer.h"
#include "ws_upload.h"
#include "plus_transfer_hooks.h"
#include "sleep_wake.h"
#include "SilentRestart.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "ReadingStatsStore.h"
#include "AchievementsStore.h"
#include "OpdsServerStore.h"
#include "WifiCredentialStore.h"
#include <WiFiClient.h>
#include "network/FirmwareFlasher.h"
#include "cpyos_sd.h"

// HalDisplay samples the EPD PMIC between poweron and poweroff of each push.
extern "C" bool plus_last_push_tps_ok();
#define STEP_MEM(fmt, ...)                                                                      \
    ESP_LOGE("PLUS", "shell step: " fmt " | int_free=%u int_largest=%u dma_largest=%u",          \
             ##__VA_ARGS__, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),              \
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),                    \
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA))
#include <nvs.h>
#include <esp_system.h>
#include <Dictionary.h>
#include "plus_log_mirror.h"
#include "FontExtras.h"
#include "activities/settings/FontExtrasActivity.h"
#include "epd_board_metalio_eink4_plus.h"
#include "plus_battery.h"
#include "plus_audio.h"
#include "BackgroundWork.h"
#include "plus_ai_key.h"
#include "plus_ai_key_routing.h"
#include "plus_voice_activity.h"
#include "plus_voice_client.h"
#include "PlusCellular.h"
#include "plugin_demo.h"
#include <SoundFeedback.h>


#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstdlib>

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "ff.h"
#include <sys/select.h>
#include <unistd.h>

#include <driver/gpio.h>
#include <driver/uart.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>


#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <psa/crypto.h>
#include <sdmmc_cmd.h>
#include <SecureClient.h>
#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)
#include "booksource/BookSource.h"
namespace booksource {
ISource* privateSource();
}
#endif
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_cpu.h>
#include <esp_memory_utils.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <BleKeyboardHost.h>
#include <WiFi.h>
#include "BleInput.h"
#include "activities/RenderLock.h"
#include <plus_battery.h>
#include <plus_charger.h>
#include <plus_rtc.h>
#include <PinyinDb.h>
#include <Epub/hyphenation/HyphenationStore.h>
#include <metalio_touch.h>
#include "metalio_i2c1.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
extern "C" {
#include "epd_board_metalio_eink4_plus.h"
#include "pca9555.h"
}
// Embedded EPUB fonts are skipped on this port by default (CFF scale mismatch);
// `embfont 1` lifts the skip for an A/B run.
static bool s_embeddedFontEnabled = false;
extern "C" bool plus_embedded_font_enabled(void) {
    // A marker file keeps the A/B choice across the `open` reboot the bench uses.
    return s_embeddedFontEnabled || Storage.exists("/.crosspoint/embfont");
}

// Transient-UI waveform knob (HalDisplay.cpp): keeps typing off the 48-phase
// GL16 path. Declared here because the console dispatch needs both directions.
extern "C" void plus_epd_set_transient_wave(int value);
extern "C" int plus_epd_transient_wave(void);
extern "C" void plus_boot_hang_request(void);
#include "I18n.h"
#include "I18nPack.h"
#include "KOReaderCredentialStore.h"
#include "NetworkStartup.h"
#include "HalPowerManager.h"
#include "plus_bt_audio_activity.h"
#include "MappedInputManager.h"
#include "UiCjkFallbacks.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_18_bold.h>
#include <builtinFonts/notosans_18_regular.h>
#include <builtinFonts/notosans_8_regular.h>
#if !defined(CROSSPOINT_FONT_PACK_EMBEDDED)
#include <builtinFonts/notosans_cjk_8.h>
#endif

#include "CrossPointSettings.h"
#include "EmojiFont.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "FontHealth.h"
#include <HalSystem.h>
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
#include "Memory.h"
#include "activities/apps/plugins/PluginActivity.h"
#include "plugins/PluginManager.h"
#endif
#include "activities/home/MyActivity.h"
#include "activities/apps/standby/StandbyActivity.h"
#include "plus_ota_activity.h"
#include "plus_transfer_activity.h"
#include "plus_voice_memo_activity.h"
#include "plus_resources_activity.h"
#include "power_off_activity.h"
#include "cpyos_sd.h"

// UI faces come from UITheme.cpp (built-in fallback when no system pack is
// present); the CJK faces are registered here so the UI has real Han glyphs.
extern EpdFont ui10RegularFont;
extern EpdFont ui10BoldFont;
extern EpdFont ui12RegularFont;
extern EpdFont ui12BoldFont;
extern EpdFont ui18RegularFont;
extern EpdFont ui18BoldFont;
extern EpdFontFamily control18FontFamily;

namespace {

EpdFontFamily ui10Family(&ui10RegularFont, &ui10BoldFont);
EpdFontFamily ui12Family(&ui12RegularFont, &ui12BoldFont);
EpdFontFamily ui18Family(&ui18RegularFont, &ui18BoldFont);
EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFamily(&smallFont);

// Pack build (same policy as Read Pico): only the 8pt built-in subset stays
// in the image as the emergency face. The 10/12/14/16pt CJK bitmaps would add
// ~1.76 MB and the vector system pack already provides every size at 4-bit.
// The placeholders keep the EpdFont pointers valid until typography::apply()
// binds the real faces.
static EpdFontData emptyCjk8Data;
static EpdFontData emptyCjk10Data;
static EpdFontData emptyCjk12Data;
static EpdFontData emptyCjk14Data;
static EpdFontData emptyCjk16Data;
// The vector pack registers 8pt too (Compact density); the bitmap is not
// linked on the embedded profile.
#if defined(CROSSPOINT_FONT_PACK_EMBEDDED)
EpdFont cjk8Font(&emptyCjk8Data);
#else
EpdFont cjk8Font(&notosans_cjk_8);
#endif
EpdFont cjk10Font(&emptyCjk10Data);
EpdFont cjk12Font(&emptyCjk12Data);
EpdFont cjk14Font(&emptyCjk14Data);
EpdFont cjk16Font(&emptyCjk16Data);
EpdFontFamily cjk8Family(&cjk8Font);
EpdFontFamily cjk10Family(&cjk10Font);
EpdFontFamily cjk12Family(&cjk12Font);
EpdFontFamily cjk14Family(&cjk14Font);
EpdFontFamily cjk16Family(&cjk16Font);

// 4G presence: NT26K on UART2. Pins mirror the vendor board config.h
// (NT26_TX_PIN 58 / NT26_RX_PIN 59 / NT26_MRDY_PIN 60 / NT26_SRDY_PIN 3); that
// header is private to the BSP component, so the values are repeated here.
constexpr gpio_num_t kAudioBclk = GPIO_NUM_42;  // vendor config AUDIO_I2S_BCLK
constexpr gpio_num_t kAudioWs = GPIO_NUM_43;    // AUDIO_I2S_WS
constexpr gpio_num_t kAudioDout = GPIO_NUM_44;  // AUDIO_I2S_DOUT
constexpr gpio_num_t kCellularTx = GPIO_NUM_58;
constexpr gpio_num_t kCellularRx = GPIO_NUM_59;
constexpr gpio_num_t kCellularMrdy = GPIO_NUM_60;
constexpr gpio_num_t kCellularSrdy = GPIO_NUM_3;

// 4G presence: NT26K on UART2 (TX58/RX59, MRDY60/SRDY3). The vendor firmware
// talks a framed UART-Ethernet protocol at 2 Mbaud, so the old 115200 raw-AT
// probe could never see a present module. Detection here power-cycles the shared
// P14 rail (the module reboots) and listens at the real rate: a powered module
// emits boot URCs even before the frame handshake is implemented.
bool probeCellularOnce() {
    constexpr uart_port_t kUart = UART_NUM_2;
    // MRDY (runtime/dtr) idle high; SRDY (RI) is an input.
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << kCellularMrdy;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(kCellularMrdy, 1);
    io.pin_bit_mask = 1ULL << kCellularSrdy;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    // Vendor sequence: P14 low ~1 s (with the motor discharge pulse), then high
    // and a 1 s settle before the module answers.
    const esp_err_t reset = epd_board_metalio_eink4_plus_bt_power_reset();
    if (reset != ESP_OK) {
        ESP_LOGW("PLUS", "4G probe: rail reset failed (%s); probing anyway", esp_err_to_name(reset));
    }
    vTaskDelay(pdMS_TO_TICKS(1200));

    uart_config_t cfg = {};
    cfg.baud_rate = 2000000;  // vendor Nt26Board: UART_NUM_2 @ 2 Mbaud
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(kUart, 4096, 0, 0, nullptr, 0) != ESP_OK) return false;
    if (uart_param_config(kUart, &cfg) != ESP_OK ||
        uart_set_pin(kUart, kCellularTx, kCellularRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete(kUart);
        return false;
    }
    uart_flush_input(kUart);
    uint8_t buf[128];
    int n = 0;
    const unsigned long deadline = millis() + 2500;
    while (millis() < deadline) {
        const int r = uart_read_bytes(kUart, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (r > 0) n += r;
    }
    uart_driver_delete(kUart);
    ESP_LOGI("PLUS", "4G probe (2 Mbaud): %d byte(s), srdy=%d, module %s", n,
             gpio_get_level(kCellularSrdy), n > 0 ? "present" : "absent");
    return n > 0;
}

// Frontlight presence has no electrical readback on this board (YX6016N). The
// only probe is the pack-current delta, and at the vendor's 10 % PWM ceiling a
// real pair draws only a few mA — well inside the BQ27220's noise. The probe
// therefore reads AverageCurrent (its ~1 Hz rolling register) at 100 % UI
// brightness, takes the median of three samples on each side, and treats a
// small delta as inconclusive instead of persisting "absent": hiding a light
// that exists is far worse than showing a control on a lightless unit (the
// web-visible frontlightPresent flag still lets an owner force it off).
// Budget: ~5 s of boot time, once, on the first battery boot.
int medianOfFive(int v[5]) {
    for (int i = 1; i < 5; ++i) {
        const int key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = key;
    }
    return v[2];
}

// BQ27220 instantaneous current (0x0C, the only current register the vendor
// firmware uses). The register has a 1 mA LSB but the gauge applies its own
// averaging, so a median of five samples 300 ms apart is stable enough for a
// few-mA LED load.
bool sampleCurrentMa(int& outMa) {
    int samples[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 5; ++i) {
        plus_battery_sample_t s{};
        if (!PlusBattery_Read(&s) || !s.valid) return false;
        samples[i] = s.milliamps;
        if (i < 4) vTaskDelay(pdMS_TO_TICKS(300));
    }
    outMa = medianOfFive(samples);
    return true;
}

void probeFrontlightOnce() {
    if (SETTINGS.frontlightProbed) return;
    if (gpio.isUsbConnected()) return;  // retry on a later battery boot

    const uint8_t savedBrightness = SETTINGS.frontlightBrightness;
    const bool savedLit = Frontlight.isOn();

    Frontlight.setOn(false);
    vTaskDelay(pdMS_TO_TICKS(600));
    int offMa = 0;
    if (!sampleCurrentMa(offMa)) return;
    if (offMa == 0) {
        // USB-powered (or the gauge is not counting): a zero baseline can never
        // prove a light exists. Leave the one-shot probe armed for a battery
        // boot instead of burning another 4 s on every USB boot.
        ESP_LOGI("PLUS", "frontlight probe skipped (0 mA baseline, running on USB)");
        return;
    }

    Frontlight.setBrightness(100);
    Frontlight.setOn(true);
    vTaskDelay(pdMS_TO_TICKS(900));
    int onMa = 0;
    const bool gotOn = sampleCurrentMa(onMa);
    Frontlight.setOn(false);
    Frontlight.setBrightness(savedBrightness);
    if (savedLit) Frontlight.setOn(true);
    if (!gotOn) return;

    // Discharge current is negative; lighting the pair makes it more negative.
    const int delta = offMa - onMa;
    if (delta < 3) {
        ESP_LOGW("PLUS",
                 "frontlight probe inconclusive (%d -> %d mA, delta %d); keeping present, will retry",
                 offMa, onMa, delta);
        return;
    }
    SETTINGS.frontlightPresent = 1;
    SETTINGS.frontlightProbed = 1;
    SETTINGS.frontlightProbeRev = 2;
    SETTINGS.saveToFile();
    ESP_LOGI("PLUS", "frontlight probe: %d -> %d mA (delta %d) -> present", offMa, onMa, delta);
    Frontlight.setPresentOverride(true);
}

void registerFonts(GfxRenderer& renderer) {
    renderer.insertFont(NOTOSANS_18_FONT_ID, ui18Family);
    renderer.insertFont(CONTROL_18_FONT_ID, control18FontFamily);
    renderer.insertFont(UI_10_FONT_ID, ui10Family);
    renderer.insertFont(UI_12_FONT_ID, ui12Family);
    renderer.insertFont(SMALL_FONT_ID, smallFamily);
    // The shared chrome asks for these ids directly; without them the theme
    // logs "Font -858375107 not found" and the clock falls back to a face with
    // different metrics.
    renderer.insertFont(BaseTheme::STATUS_NUMERIC_FONT_ID, smallFamily);
    renderer.insertFont(READER_STATUS_FONT_ID, smallFamily);
    renderer.insertFont(READER_ESTIMATE_FONT_ID, ui10Family);
    renderer.insertFont(CJK_UI_8_FONT_ID, cjk8Family);
    renderer.insertFont(CJK_UI_10_FONT_ID, cjk10Family);
    renderer.insertFont(CJK_UI_12_FONT_ID, cjk12Family);
    // Full UI typography chain: UI_10 falls back to 14pt, UI_12/18 to 16pt,
    // and both drop to the wide GB2312 12pt face for characters outside the
    // i18n vocabulary. The single 14.375 MiB app slot leaves room for both.
    renderer.insertFont(CJK_UI_14_FONT_ID, cjk14Family);
    renderer.insertFont(CJK_UI_16_FONT_ID, cjk16Family);
}

}  // namespace

// The shared activity layer reaches these globals directly, so they exist at
// namespace scope exactly as in main.cpp.
EpdFont offlineReaderFont(&emptyCjk12Data);
SdCardFontSystem sdFontSystem;
GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);

// Silent-restart hooks live in main.cpp on the reference build. Here they are
// real reboots; the destination is carried across the reset in a one-shot marker
// file because silencing the reboot is what reclaims the radio's heap for the
// reader (see JjwxcActivity::handOffToReader).
void silentRestart() {
    LOG_INF("PLUS", "restarting after a settings change");
    esp_restart();
}
namespace {
// One-shot boot destination written just before a silent restart and consumed
// (deleted) by the next boot. Line 1 is the destination, line 2 the EPUB path.
constexpr const char *kSilentTargetPath = "/.crosspoint/silent_target";

void writeSilentTarget(const char *kind, const char *path = "") {
    Storage.ensureDirectoryExists("/.crosspoint");
    String payload(kind);
    if (path != nullptr && *path != '\0') {
        payload += "\n";
        payload += path;
    }
    if (!Storage.writeFile(kSilentTargetPath, payload)) {
        LOG_ERR("PLUS", "could not record silent restart target");
    }
}

String takeSilentTarget() {
    if (!Storage.exists(kSilentTargetPath)) {
        return String();
    }
    String value = Storage.readFile(kSilentTargetPath);
    Storage.remove(kSilentTargetPath);
    return value;
}

// Bench-only persistent reader target: the one-shot marker is consumed by the
// very next boot, and every console open resets the SoC, so an automated
// format check would lose it before it could be observed. This file holds the
// path until it is explicitly cleared and makes every boot land in the reader.
constexpr const char* kPersistOpenPath = "/.crosspoint/open_persist";

// Format: "<boots-left> <path>". The counter keeps a bad book from parking the
// unit in a boot loop: a book that crashes the reader burns one boot per reset
// and the target disappears after a handful, so the device falls back to Home
// on its own even without a console.
constexpr int kPersistOpenBoots = 4;

String takePersistOpenTarget() {
    if (!Storage.exists(kPersistOpenPath)) return String();
    String value = Storage.readFile(kPersistOpenPath);
    value.trim();
    const int space = value.indexOf(' ');
    if (space <= 0) {
        Storage.remove(kPersistOpenPath);
        return String();
    }
    const int left = value.substring(0, space).toInt();
    const String path = value.substring(space + 1);
    if (left <= 1) {
        Storage.remove(kPersistOpenPath);
    } else {
        Storage.writeFile(kPersistOpenPath, String(left - 1) + " " + path);
    }
    return path;
}

bool transferStartHook() { return web_transfer::start(); }
void transferStopHook() { web_transfer::stop(); }
bool usbStartHook() { return usb_msc::start(); }
plus_transfer::Status transferStatusHook() {
    const auto status = ws_upload::status();
    return plus_transfer::Status{web_transfer::running(), status.received, status.total, status.completed};
}
}  // namespace

void silentRestartToSettings() { silentRestart(); }
void silentRestartToReader(bool) {
    writeSilentTarget("reader", APP_STATE.openEpubPath.c_str());
    esp_restart();
}
void silentRestartToReaderAndPreloadChineseFont(uint8_t) {
    writeSilentTarget("reader", APP_STATE.openEpubPath.c_str());
    esp_restart();
}
void silentRestartToJoinNetwork() { silentRestart(); }

void plus_console_start();
// Disarms the boot-time watchdog armed in main.c. Every path that reaches a
// usable UI (including the storage-error page) must call it, or the unit would
// reboot every minute instead of showing the page.
extern "C" void plus_boot_done();

// Persistent power evidence for USB-less sessions. USB opens reset the SoC,
// so a wake/battery histogram cannot live only on the console; one line per
// wake and one per sample goes to /.crosspoint/power.log and is read back after
// the cable is plugged in again. All writes happen on the UI task, after
// HalStorage has restored its parked handles.
static uint32_t s_battLogUntil = 0;
static uint32_t s_battLogNext = 0;
static uint32_t s_powerWakeSeq = 0;
// A reboot during a cable-out run used to drop the in-RAM deadline, leaving the
// 48-minute battery session without samples. Persist the remaining minutes and
// resume on the next boot once the card is mounted.
static bool s_battLogLoaded = false;
constexpr const char* kBattLogStatePath = "/sdcard/.crosspoint/battlog";
constexpr uint32_t kBattLogMaxMinutes = 1440;

void battLogSave(const uint32_t minutes) {
    FILE* f = fopen(kBattLogStatePath, "w");
    if (f == nullptr) return;
    std::fprintf(f, "%u\n", static_cast<unsigned>(minutes));
    fclose(f);
}

void battLogClear() { std::remove(kBattLogStatePath); }

void battLogResumeOnce() {
    if (s_battLogLoaded) return;
    s_battLogLoaded = true;
    FILE* f = fopen(kBattLogStatePath, "r");
    if (f == nullptr) return;
    unsigned minutes = 0;
    if (std::fscanf(f, "%u", &minutes) != 1) minutes = 0;
    fclose(f);
    if (minutes == 0) return;
    if (minutes > kBattLogMaxMinutes) minutes = kBattLogMaxMinutes;
    s_battLogUntil = millis() + static_cast<uint32_t>(minutes) * 60000u;
    s_battLogNext = millis();
    printf("battlog: resumed %u min\n", minutes);
}

void plusPowerLogLine(const char* fmt, ...) {
    FILE* f = fopen("/sdcard/.crosspoint/power.log", "a");
    if (f == nullptr) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void consoleBattLog(const char* args) {
    const int minutes = args != nullptr ? atoi(args) : 0;
    if (minutes <= 0) {
        s_battLogUntil = 0;
        s_battLogLoaded = true;
        battLogClear();
        printf("battlog: off\n");
        return;
    }
    const uint32_t stored = minutes > static_cast<int>(kBattLogMaxMinutes) ? kBattLogMaxMinutes
                                                                          : static_cast<uint32_t>(minutes);
    s_battLogUntil = millis() + stored * 60000u;
    s_battLogNext = millis();
    s_battLogLoaded = true;
    battLogSave(stored);
    printf("battlog: %u min -> /.crosspoint/power.log\n", static_cast<unsigned>(stored));
}

void plusPowerTick() {
    const uint32_t now = millis();
    static int lastUsb = -1;
    const int usb = gpio.isUsbConnected() ? 1 : 0;
    if (usb != lastUsb) {
        lastUsb = usb;
        // Records the unplug itself, so a panic reset that follows can be told
        // apart from a panic inside the sleep park/restore.
        plusPowerLogLine("usb=%d ms=%lu", usb, static_cast<unsigned long>(now));
    }
    const uint32_t seq = powerManager.wakeEventSequence();
    const bool woke = seq != s_powerWakeSeq;
    const bool due = s_battLogUntil != 0 && static_cast<int32_t>(now - s_battLogNext) >= 0;
    if (!s_battLogLoaded && cpyos_sd_is_mounted()) battLogResumeOnce();
    if (!woke && !due) return;
    if (s_battLogUntil != 0 && static_cast<int32_t>(now - s_battLogUntil) >= 0) {
        s_battLogUntil = 0;
        battLogClear();
        printf("battlog: done\n");
        return;
    }

    plus_battery_sample_t sample{};
    const bool have = PlusBattery_Read(&sample);
    const int wifi = WiFi.status() == WL_CONNECTED ? 1 : 0;
    if (woke) {
        // HalPowerManager appends the wake line itself, right after the restore
        // and before control returns here; only advance the counter so the
        // battery sample below is not duplicated.
        s_powerWakeSeq = seq;
    }
    if (due) {
        s_battLogNext = now + 60000u;
        plusPowerLogLine("sample ms=%lu mv=%d ma=%d soc=%d chg=%d usb=%d wifi=%d",
                         static_cast<unsigned long>(now), have ? sample.millivolts : 0,
                         have ? sample.milliamps : 0, have ? sample.soc_percent : 0,
                         PlusBattery_IsCharging() ? 1 : 0, gpio.isUsbConnected() ? 1 : 0, wifi);
        const int32_t left = static_cast<int32_t>(s_battLogUntil - now);
        const uint32_t remaining =
            left > 0 ? (static_cast<uint32_t>(left) + 59999u) / 60000u : 0u;
        battLogSave(remaining);
    }
}

extern "C" void cpyos_shell_demo(void) {
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    // The USB-Serial/JTAG VFS defaults to ESP_LINE_ENDINGS_CR on receive, which
    // rewrites EVERY 0x0D byte to 0x0A. That silently corrupts binary uploads
    // (`putfile`): the size stays right while the content changes, so uploaded
    // images hashed differently on the card and JPEG decode failed. LF mode is
    // pass-through; the console task accepts both \n and \r, so commands are
    // unaffected.
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
#endif
    // Mirror ESP_LOG to the card before anything else: the USB console has
    // died mid-session before, and then there is no other trace.
    plusLogMirrorStart();
    // Non-Activity workers (the JJWXC cache queue service) enable the radio
    // through NetworkStartup::setMode(), which needs the renderer to reclaim
    // render memory. Registering it once here keeps those workers on the same
    // rules as every screen instead of a second, weaker bring-up.
    NetworkStartup::registerRenderer(&renderer);
    // First line of every boot: the hardware session needs to tell a cold boot
    // from a watchdog/panic/brownout reset and from a wake-up.
    printf("plus boot: reset=%d wake=%d\n", static_cast<int>(esp_reset_reason()),
           static_cast<int>(plusFirstWakeupCause()));
    // The Wi-Fi driver opens its own NVS namespace during esp_wifi_init and
    // fails with ESP_ERR_NVS_NOT_INITIALIZED if nobody initialised NVS first;
    // the shell itself has no other NVS user on the boot path (Preferences is
    // lazy), so this has to happen before any radio activity.
#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)
    // Link anchor: the public registry lives in cpyos_ui, which the final link
    // scans after cpyos_app. Without a reference from a main-component object
    // the linker never pulls JjwxcSource.o and booksource::privateSource()
    // stays undefined, so the JJWXC tile silently opens nothing.
    (void)booksource::privateSource();
#endif
    esp_err_t nvsErr = nvs_flash_init();
    if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvsErr = nvs_flash_init();
    }
    if (nvsErr != ESP_OK) {
        printf("plus boot: nvs init failed: %s\n", esp_err_to_name(nvsErr));
    }
    // The transfer page lives in the activity component; these hooks are the
    // only link between it and the HTTP/WebSocket servers in this component.
    plus_transfer::setHooks({transferStartHook, transferStopHook, transferStatusHook, usbStartHook});
    STEP_MEM("before display.begin");
    display.begin();
    STEP_MEM("display ok; gpio.begin");
    gpio.begin();
    // GPIO61 is the vendor/kiikoread AI/BOOT key. It is not part of the input
    // manager's mask, so the UI loop samples it on the same 25 ms debounce the
    // vendor uses.
    plus_ai_key::begin();
    // Boot feedback, deliberately BEFORE plus_audio_init(): the BH1098 reset
    // drives the shared motor line for its rail discharge, and any haptic pulse
    // issued during s_bt_reset_active is masked by that sequence.
    gpio.pulseHaptic(120);
    STEP_MEM("gpio ok");

    // The compat BoardConfig carries the Read Pico profile (no frontlight). The
    // Plus differs only in its cool/warm PWM pair and sensor pins; override the
    // frontlight entry so FrontlightManager can drive GPIO6/GPIO7.
    //
    // FREEINK_CAP_FRONTLIGHT is intentionally 0 for this port, so this profile
    // is inert and the LEDs stay dark. Before enabling the capability, clamp the
    // 100 % UI brightness to 10 % PWM full scale (vendor FL_OUTPUT_MAX_PERCENT);
    // the shared manager otherwise maps 100 % to full duty and can over-drive
    // the LEDs/driver. See docs/engineering/metalio-eink4-plus-gaps.md §2.
    // The Metalio Plus profile now carries these four facts (name, epdiy
    // controller, GPIO6/7 frontlight, no LEDC buzzer on GPIO2), so nothing is
    // mutated at runtime; assert the compiled profile instead.
    static_assert(BoardConfig::METALIO_PLUS.audio.buzzer == BoardConfig::PIN_UNASSIGNED,
                  "Plus must not route the LEDC buzzer onto the TCA INT line");
    halTiltSensor.begin();

    const bool mounted = cpyos_sd_is_mounted();
    ESP_LOGE("PLUS", "shell step: sd=%d; settings", mounted ? 1 : 0);
    if (mounted) {
        Storage.begin();
        // First line of this boot in the persistent power log: a cable replug
        // resets the SoC, so this is how the test session is delimited.
        uint32_t wakeStage = 0;
        uint8_t wakeReason = 0;
        if (HalPowerManager::pendingWake(&wakeStage, &wakeReason)) {
            // Written from RTC memory: this line survives a panic in whatever
            // activity ran after the wake, which is exactly the case a direct
            // SD write at wake time cannot cover.
            plusPowerLogLine("previous-wake reason=%u stage=%u", static_cast<unsigned>(wakeReason),
                             static_cast<unsigned>(wakeStage));
        }
        const uint32_t sleepStage = HalPowerManager::sleepStage();
        plusPowerLogLine("boot reset=%d cause=%d stage=%u ms=%lu", static_cast<int>(esp_reset_reason()),
                         static_cast<int>(plusFirstWakeupCause()), static_cast<unsigned>(sleepStage),
                         static_cast<unsigned long>(millis()));
        if (esp_reset_reason() == ESP_RST_PANIC && sleepStage >= 1 && sleepStage <= 14) {
            // Park/sleep steps: 1 SD, 2 TPS, 3 bus hold, 4 EPD off, 5 expander,
            // 6 wake pin, 7 INT high, 8 wake config, 9 timer, 10 rails,
            // 11 sleeping, 12 woke, 13 restored, 14 returned.
            plusPowerLogLine("panic stage=%u", static_cast<unsigned>(sleepStage));
        }
        HalPowerManager::clearSleepStage();
        const bool hadSettings = SETTINGS.loadFromFile();
#if defined(CPYOS_DEV_SLEEP_DEFAULT_NEVER)
        // Development default only: the sleep timeout starts at "never" so
        // unattended automation does not drop into the standby clock.
        //
        // This is a one-time migration rather than a permanent override: the
        // marker records that this image already applied its default, so a
        // timeout picked in Settings survives the next boot (sleeping still
        // works and must stay debuggable). A card that already has a settings
        // file is migrated too - otherwise an existing 10-minute default keeps
        // coming back and the setting never reads "never".
        // Release images do not define this, so a new install keeps the
        // 10-minute default.
        // "Never" is the top of the value range (31), not 0: fromJson() clamps
        // 0 up to MIN (1 minute), which would have slept every minute.
        constexpr uint8_t kSleepNever = CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES;
        static constexpr const char* kDevSleepMarker = "/.crosspoint/devsleep-v2";
        if (!Storage.exists(kDevSleepMarker)) {
            bool applied = true;
            if (SETTINGS.sleepTimeoutMinutes != kSleepNever) {
                SETTINGS.sleepTimeoutMinutes = kSleepNever;
                applied = SETTINGS.saveToFile();
            }
            if (applied) {
                Storage.ensureDirectoryExists("/.crosspoint");
                Storage.writeFile(kDevSleepMarker, "1\n");
            }
            ESP_LOGW("PLUS", "dev image: sleep timeout set to never (one-time; had=%d saved=%d)",
                     hadSettings ? 1 : 0, applied ? 1 : 0);
        }
#endif
    }
    STEP_MEM("settings ok; i18n");
    // The product build loads every store in main.cpp; the shell only loaded
    // settings and the Wi-Fi list, so features that gate on a store looked
    // unconfigured on the bench. In particular RecentBooksStore is the source
    // for Home's "继续阅读"/recent shelf: without this load the in-memory list
    // starts empty on every boot even though recent.json is present, and the
    // next reader open overwrites it with a one-entry list.
    KOREADER_STORE.loadFromFile();
    APP_STATE.loadFromFile();
    RECENT_BOOKS.loadFromFile();
    READING_STATS.loadFromFile();
    ACHIEVEMENTS.loadFromFile();
    OPDS_STORE.loadFromFile();
    // Apply the saved UI language before the strings are resolved. Read Pico
    // does the same; without it the shell always draws the default language
    // even though Settings shows the user's choice.
    I18N.setLanguage(static_cast<Language>(SETTINGS.language));
    I18N.init();
    STEP_MEM("i18n ok");

    renderer.begin();
    // The panel is bistable and this is a fresh boot: whatever was on it when
    // the machine stopped (the shutdown card, a crashed frame) must be cleared
    // by the first push, not left to a differential update.
    renderer.requestNextRefresh(HalDisplay::FULL_REFRESH);
    STEP_MEM("renderer.begin ok; fonts");
    registerFonts(renderer);
    STEP_MEM("builtin fonts ok; system pack");
    // Vector system face first: the UI roles and the reader sizes are rebound
    // to the pack's 4-bit faces, which is what makes the 16-level text path
    // actually anti-aliased.
    STEP_MEM("system pack begin");
    if (!system_font_pack::begin(renderer)) {
        LOG_ERR("PLUS", "system font pack unavailable; Chinese UI will be incomplete");
    } else {
        typography::apply(renderer, SETTINGS.uiFontScale);
        // Classic headers (reader menu, chapter selection) and card titles ask
        // for the serif title id directly. Read Pico embeds a ~190 KB Song
        // bitmap for it; this image has no slot headroom, so reuse the system
        // pack's title face instead of leaving the id unregistered (which drew
        // no title at all).
        if (renderer.getFontMap().find(CJK_UI_12_FONT_ID) != renderer.getFontMap().end()) {
            renderer.setPreferredFont(NOTOSERIF_TITLE_FONT_ID, CJK_UI_12_FONT_ID);
            renderer.setFallbackFont(NOTOSERIF_TITLE_FONT_ID, CJK_UI_8_FONT_ID);
        }
    }
    STEP_MEM("font pack ok; sd fonts");
    // One line at boot: proves the PSA backend protected EPUBs depend on is
    // usable on this silicon (AES-128-CBC round-trip + SHA-256 vector).
    freeink::content::portContentCryptoSelfTest();

    static FontDecompressor fontDecompressor;
    if (fontDecompressor.init()) {
        static FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts(),
                                                 renderer.getTtfFonts());
        fontCacheManager.setFontDecompressor(&fontDecompressor);
        renderer.setFontCacheManager(&fontCacheManager);
    } else {
        LOG_ERR("PLUS", "font decompressor init failed");
    }
    sdFontSystem.begin(renderer);
#if defined(CROSSPOINT_PDF_XIP_SD)
    // Read Pico installs the MuPDF font/CMap bundle from its font setup; the
    // Plus has no app1 carrier, so unpack the SD bundle once here (marker
    // gated).  Without it non-embedded CJK fonts and CMaps are unavailable to
    // the PDF engine until the bench-only `pdfassets` command runs.
    if (ensurePdfAssets()) {
        ESP_LOGI("PLUS", "PDF font/CMap assets ready");
    } else {
        ESP_LOGW("PLUS", "PDF font/CMap assets unavailable (stage /pdf_assets.bin)");
    }
#endif
    STEP_MEM("sd fonts ok; emoji");
    // Bundled emoji face from the spiffs partition. A missing pack only leaves
    // emoji as tofu; it never aborts. The reader re-applies its point size.
    emoji_font::begin(renderer, SETTINGS.fontPointSize);
#if !defined(CROSSPOINT_EMBED_HYPHENATION)
    // Read Pico warms every hyphenation trie on the main task right after
    // HalSystem::begin(); the Plus port never did, so a section layout that
    // needed hyphenation read the data partition from the PSRAM render stack
    // and the loader refused it ("partition index read refused: PSRAM stack").
    auto& hyph = hyphenation::HyphenationStore::shared();
    hyph.preloadAll();
    ESP_LOGI("PLUS", "HYPH preload done; loaded language: %s",
             hyph.loadedLanguage() ? hyph.loadedLanguage() : "-");
#endif
    STEP_MEM("emoji ok; clock/frontlight");
    halClock.begin();
    halClock.setUseChinaServers(SETTINGS.contentProfile == CrossPointSettings::ContentProfile::China);

    // No-frontlight units: the row "本机带前光" (settings) states presence; the
    // LED pair cannot be detected electrically. Apply before any UI asks
    // Frontlight.present(), and never light an absent pair.
    Frontlight.setPresentOverride(SETTINGS.frontlightPresent != 0);
    Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth,
                     SETTINGS.frontlightOn && SETTINGS.frontlightPresent);
    // Hardware-variant probes run once and persist; no user-facing switches.
    if (!SETTINGS.cellularProbed) {
        SETTINGS.cellularPresent = probeCellularOnce() ? 1 : 0;
        SETTINGS.cellularProbed = 1;
        SETTINGS.saveToFile();
    }
    probeFrontlightOnce();
    // Battery policy: start the vendor settle window and take the boot sample
    // (the log line is the proof the gauge is actually being read). The Plus
    // app never called this before, so the chrome's battery capsule could only
    // ever show the "no reading" placeholder.
    powerManager.begin();
    // The speaker path is deliberately NOT warmed here: the BH1098 reset writes
    // the shared TCA9555 ports, and it was racing display.begin()'s TPS power-on
    // (log: "TPS probe failed" x3, rails off) so the first boot frame scanned
    // with the panel unpowered and the previous screen stayed visible. It is
    // started from the loop once the boot UI has been presented (see the
    // kAudioWarmupDelayMs gate there).
    STEP_MEM("frontlight ok; activityManager.begin");

    UITheme::getInstance().setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme));

    {
        // The device secret lives in NVS (internal flash). A task whose stack is
        // in PSRAM cannot read it, and the render task's stack IS in PSRAM here,
        // so the library/thumbnail scan of a protected EPUB used to log
        // "[DSEC] refusing NVS secret read from a PSRAM-stack task" and every
        // protected book reported "no content key". Warm the cache on the main
        // task before that task exists (same call Read Pico makes in setup()).
        uint8_t warmSecret[32];
        if (!deviceSecret(warmSecret)) {
            LOG_ERR("DSEC", "device secret preload failed");
        }
    }

    HalSystem::installCppFailureHandlers();
    activityManager.begin();
    // Crash visibility parity with Read Pico: a panic/brown-out/watchdog boot
    // lands on the shared crash screen (it prints the fault ring), and the C++
    // failure hooks record a bad_alloc/terminate instead of dying silently.
    if (HalSystem::isRebootFromPanic()) {
        activityManager.goToCrashReport();
    }
    // Now that the UI exists, let the expander's INT line drive touch/keys.
    if (epd_board_metalio_eink4_plus_ioexp_irq_start() != ESP_OK) {
        LOG_ERR("PLUS", "expander INT handler not installed");
    }
    plus_console_start();
    STEP_MEM("activityManager.begin ok; routing");

    // Parity with Read Pico: a card that failed to mount is a visible, retryable
    // state, not a silently empty library with defaults for every setting.
    // The retry button goes through Storage.begin(), which now really remounts
    // the card here (see HalStorage::begin) and restarts into a full cold boot.
    if (!cpyos_sd_is_mounted()) {
        // A card that was mid-transaction when the previous boot was reset can
        // need a moment before it answers again. Retry for ~30 s before showing
        // the error page, so a transient failure heals without a power press.
        for (int attempt = 0; attempt < 10 && !cpyos_sd_is_mounted(); ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            Storage.begin();  // remounts (see HalStorage::begin)
            if (cpyos_sd_is_mounted()) {
                ESP_LOGW("PLUS", "SD card answered on retry %d after boot", attempt + 1);
                SETTINGS.loadFromFile();
                I18N.setLanguage(static_cast<Language>(SETTINGS.language));
                I18N.init();
                break;
            }
        }
    }
    if (!cpyos_sd_is_mounted()) {
        ESP_LOGE("PLUS", "no SD card at boot; showing the storage error page");
        plus_boot_done();
        activityManager.goToFullScreenMessage(tr(STR_SD_CARD_ERROR), EpdFontFamily::BOLD,
                                             tr(STR_SD_CARD_ERROR_DETAIL), true);
        activityManager.requestUpdateAndWait();
        return;
    }

    // The JJWXC app hands a finished book to the reader by rebooting (that is
    // what reclaims the Wi-Fi heap). The reference build carries the target in
    // an RTC flag; here the target rides in a one-shot file written just before
    // the reset, so a normal cold boot still lands on Home.
    String silentTarget = takeSilentTarget();
    if (!silentTarget.startsWith("reader\n")) {
        const String persist = takePersistOpenTarget();
        if (persist.length() > 0) silentTarget = String("reader\n") + persist;
    }
    if (silentTarget.startsWith("reader\n")) {
        String readerPath = silentTarget.substring(7);
        readerPath.trim();
        if (readerPath.length() > 0 && Storage.exists(readerPath.c_str())) {
            ESP_LOGE("PLUS", "silent restart -> reader %s", readerPath.c_str());
            activityManager.goToReader(readerPath.c_str());
            if (!activityManager.currentActivityPtr()) {
                ESP_LOGE("PLUS", "reader resume failed to start; going home");
                activityManager.goHome(HomeMenuItem::RECENTS);
            }
        } else {
            ESP_LOGE("PLUS", "silent reader target '%s' is missing; going home", readerPath.c_str());
            activityManager.goHome(HomeMenuItem::RECENTS);
        }
    } else {
        activityManager.goHome(HomeMenuItem::RECENTS);
    }
    plus_boot_done();
    STEP_MEM("route returned");
}


// ---------------------------------------------------------------------------
// Serial bring-up console (USB Serial/JTAG stdin). It exists so the first
// hardware session can answer "is the panel/SD/sensor/light/RTC alive?" without
// a rebuild. Commands are deliberately read-only except `light`.
// ---------------------------------------------------------------------------
namespace {

char g_consoleLine[96];
size_t g_consoleLen = 0;
bool g_consoleReadable = true;

void consoleHelp() {
    printf("commands: help info dmaheap cryptotest tracetext batt rtc sd imu touch keys slpinfo i2c sdwrite haptic [ms] panel [fast] window screenshot screenshotgray browser <url> tlsprobe <host> putfile <path> <bytes> flash <sd path> lang <zh|en> tap <lx> <ly> key <name> swipe <x0> <y0> <x1> <y1> crypto pinyin hyphen wifi light <0-100> [warm 0-100]\n"
           "          ap <ssid> [pass] | apoff | radiooff | web [ssid] [pass] | weboff | usbmsc on|off\n"
           "          wificonnect <ssid> <password> | wificonnect_saved <ssid> | selftest | touchraw | rtcset <epoch> | bt on|off|scan [seconds]\n"
           "          settings [tab] | my [tab] | uiscale <100..125> | camera | transfer | voicememo | aivoice | otaui | slptest [s] | stats | stab <tab> | saction <n> | slist\n"
           "          wipe <dir> | ota [check|url <u>|channel 0|1] | resources | pageturn 0|1 | pwroff\n");
    // Bench/debug hooks (kept on one line so `help` stays scannable).
    printf("          act aikey areatest audio badge btmap cleanrect vcom battlog boothang btaudio cat cellprobe cellular cellularforget clean "
           " confirm readerpanel cpu dict dns embfont fastturn fbclear fbink flpresent hash httpprobe imgtest jjwxc logtest ls mkdir open orient "
           " part pdfassets pdfengineupdate pdfengineurl <https://url> plugindemo [path] pluginloop <n> [path] plugins pluginlaunch <id> putfilefast <path> <bytes> pdfengineinstall <path> "
           " pwrdemo pwrhold reboot resolve retrysd ring rm sdformat sdhealth sdhealthfail sdremount sdpark sdrename "
           " sdstress sleepbadge slpdiag slpusb standby sync tail textaa touchdown touchup voiceprobe volume vtext wave wifisaved\n");
}

// Raw expander inputs: P0 is the control port (rails/motor), P1 carries the
// power (P1.7) and volume keys (P1.0/P1.1). Use this to confirm active level
// and key order on a real unit before trusting the mapping.
void consoleKeys() {
    uint8_t p0 = 0;
    uint8_t p1 = 0;
    // pca9555_read_input_ex() locks I2C1 internally; a caller-side lock is a
    // self-deadlock (the mutex is not recursive).
    const bool ok = pca9555_read_input_ex(I2C_NUM_1, 0, &p0) == ESP_OK &&
                    pca9555_read_input_ex(I2C_NUM_1, 1, &p1) == ESP_OK;
    if (!ok) {
        printf("keys: expander read failed\n");
        return;
    }
    printf("keys: P0=0x%02X P1=0x%02X power=%d volPrev(P1.0)=%d volNext(P1.1)=%d ai(GPIO61)=%d\n", p0, p1,
           (p1 & 0x80) == 0, (p1 & 0x01) == 0, (p1 & 0x02) == 0, gpio_get_level(GPIO_NUM_61));
}

// Data-pack probes: the pinyin and hyphenation containers now ship in spiffs,
// but their loaders are shared Read Pico code, so the first hardware session has
// to prove they actually load on this board.
void consolePinyin() {
    auto& db = pinyin::PinyinDb::shared();
    const bool ok = db.ensureLoaded();
    printf("pinyin: loaded=%d syllables=%u\n", ok ? 1 : 0,
           ok ? static_cast<unsigned>(db.tables().syllableCount()) : 0u);
}

void consoleHyphen() {
    SerializedHyphenationPatterns patterns{};
    const char lang[2] = {'e', 'n'};
    auto& store = hyphenation::HyphenationStore::shared();
    const bool ok = store.patterns(lang, patterns);
    printf("hyphen: en=%d bytes=%u loaded=%s\n", ok ? 1 : 0,
           ok ? static_cast<unsigned>(patterns.size) : 0u,
           store.loadedLanguage() != nullptr ? store.loadedLanguage() : "-");
}

// Sleep/wake diagnostics. The port deliberately does not sleep yet (the vendor
// application-level sequence is not in the copied BSP subset), but a real unit
// must record the wake cause and the expander/INT levels before that sequence
// can be written. GPIO2 is IO_EXPANDER_INT_GPIO (vendor board/config.h); P1.7
// is the power key. See docs/engineering/metalio-eink4-plus.md §2.7.
void consoleSlpInfo() {
    const esp_sleep_wakeup_cause_t cause = plusFirstWakeupCause();
    const char* name = "undefined";
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0: name = "ext0"; break;
        case ESP_SLEEP_WAKEUP_EXT1: name = "ext1"; break;
        case ESP_SLEEP_WAKEUP_TIMER: name = "timer"; break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD: name = "touchpad"; break;
        case ESP_SLEEP_WAKEUP_ULP: name = "ulp"; break;
        case ESP_SLEEP_WAKEUP_GPIO: name = "gpio"; break;
        case ESP_SLEEP_WAKEUP_UART: name = "uart"; break;
        default: break;
    }
    uint8_t p1 = 0;
    // Same self-deadlock rule as consoleKeys(): the read helper owns the lock.
    const bool ok = pca9555_read_input_ex(I2C_NUM_1, 1, &p1) == ESP_OK;
    printf("slpinfo: wake=%s(%d) int(GPIO2)=%d powerHeld=%d usb=%d\n", name, static_cast<int>(cause),
           gpio_get_level(GPIO_NUM_2), ok ? ((p1 & 0x80) == 0) : -1, gpio.isUsbConnected() ? 1 : 0);
    printf("slpinfo: sleep not implemented on this port; vendor primitives and the evidence needed "
           "before wiring them are listed in docs/engineering/metalio-eink4-plus.md §2.7\n");
}

// Panel / window diagnostics: draw a fixed pattern over the current frame and
// push it. `panel` uses a full GC16, `window` only the drawn rectangle, so the
// first hardware session can compare ghosting and area mapping without the UI.
void consolePanel(const char* args) {
    const bool full = args == nullptr || strcmp(args, "fast") != 0;
    const int w = renderer.getScreenWidth();
    const int h = renderer.getScreenHeight();
    renderer.clearScreen();
    renderer.drawRect(8, 8, w - 16, h - 16, 3, true);
    for (int i = 0; i < 8; i++) {
        renderer.fillRect(32 + i * 48, 40, 32, 160 - i * 16, true);
    }
    for (int y = 240; y < h - 24; y += 16) {
        renderer.drawLine(24, y, w - 24, y, true);
    }
    renderer.displayBuffer(full ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    printf("panel: %dx%d pushed (%s)\n", w, h, full ? "GC16" : "fast");
}

// Write/read/delete a small file on the SD card: the quickest check that the
// FAT write path (not just mounting) works. The file is dot-prefixed and only
// lives for the duration of the command.
// Interleave FAT writes with full-panel scans. This is the pattern a chapter
// cache produces (write a chapter, redraw progress, repeat); it is also the
// pattern that has wedged the card with `sdmmc_write_sectors_dma` timeouts
// (0x107). Run with the radio off so the only variable is display + SD DMA.
// StarDict lookup over the SD dictionary fixture (/dictionaries/test).
// Exercises the slim reader, the .qidx sidecar build and a real definition
// read on the Plus, without needing the reader UI.
void consoleDict(const char* args) {
    Dictionary dict;
    if (!dict.open("test")) {
        printf("dict: open failed\n");
        return;
    }
    if (dict.needsIndex()) {
        Dictionary::IndexResult ir = Dictionary::IndexResult::Ok;
        if (!dict.buildIndex(nullptr, nullptr, &ir)) {
            printf("dict: index failed (%d)\n", static_cast<int>(ir));
            return;
        }
        printf("dict: index built\n");
    }
    std::string def;
    std::string head;
    Dictionary::LookupResult lr = Dictionary::LookupResult::NotFound;
    const bool ok = dict.lookup(args != nullptr ? args : "", def, head, &lr);
    printf("dict: ok=%d result=%d head='%s' bytes=%u\n", ok ? 1 : 0, static_cast<int>(lr), head.c_str(),
           static_cast<unsigned>(def.size()));
    if (ok && !def.empty()) printf("dict def: %.80s\n", def.c_str());
}

extern "C" void plus_sd_health_force_fail(void);

extern "C" void plus_sd_health_enable(int enabled);

extern "C" bool plus_sd_format_requested(void);
extern "C" void plus_sd_format_clear(void);

void consoleSdFormat() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("cpyos", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("sdformat: nvs_open failed: %s\n", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(handle, "sdformat", 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        printf("sdformat: cannot persist the request: %s\n", esp_err_to_name(err));
        return;
    }
    printf("sdformat: formatting on the next boot, then restarting\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void consoleSdHealth(const char* args) {
    int on = 1;
    if (args != nullptr) (void)sscanf(args, "%d", &on);
    plus_sd_health_enable(on);
    printf("sdhealth: %s\n", on ? "on" : "off (no probes, no auto-restart)");
}

void consoleSdHealthFail() {
    plus_sd_health_force_fail();
    printf("sdhealthfail: next probes will fail (watchdog test)\n");
}

// Atomic-replace workload: write <name>.part then rename it over <name>, which
// is what settings, progress and section caches do. The web upload and the
// chapter cache both hit their SD wedge on this pattern, not on plain writes.
void consoleSdRename(const char* args) {
    int rounds = 20;
    if (args != nullptr) (void)sscanf(args, "%d", &rounds);
    if (rounds < 1) rounds = 1;
    if (rounds > 500) rounds = 500;
    static uint8_t data[8192];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i * 13U + 7U);
    int failures = 0;
    for (int r = 0; r < rounds; ++r) {
        const int fd = open("/sdcard/.rename.part", O_RDWR | O_CREAT | O_TRUNC, 0644);
        bool ok = fd >= 0;
        if (ok) {
            ok = write(fd, data, sizeof(data)) == (ssize_t)sizeof(data);
            close(fd);
        }
        if (ok) ok = rename("/sdcard/.rename.part", "/sdcard/.rename.bin") == 0;
        if (!ok) ++failures;
        printf("sdrename: %d %s\n", r + 1, ok ? "ok" : "FAILED");
        fflush(stdout);
    }
    unlink("/sdcard/.rename.bin");
    printf("sdrename: %d/%d failed\n", failures, rounds);
}

void consoleSdRemount() {
    printf("sdremount: unmounting and remounting the card\n");
    fflush(stdout);
    const bool ok = cpyos_sd_remount();
    printf("sdremount: %s\n", ok ? "ok" : "FAILED");
}

// Self-test for the light-sleep park path without actually sleeping: open a
// HalFile, read part of it, park every handle (flush + close), restore (reopen
// at the saved offset), and compare the next bytes with a fresh handle. This is
// what lets standby over a book sleep; run it before trusting a battery test.
void consoleSdPark() {
    const char* physical = "/sdcard/.plus_park_test.bin";
    const char* logical = "/.plus_park_test.bin";
    uint8_t pattern[160];
    for (size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = static_cast<uint8_t>((i * 11U + 5U) & 0xFFU);
    const int fd = open(physical, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("sdpark: cannot create %s\n", physical);
        return;
    }
    const bool wrote = write(fd, pattern, sizeof(pattern)) == static_cast<ssize_t>(sizeof(pattern));
    close(fd);
    if (!wrote) {
        printf("sdpark: short write\n");
        return;
    }

    HalFile f = Storage.open(logical, O_RDONLY);
    if (!f) {
        printf("sdpark: cannot open its own fixture\n");
        unlink(physical);
        return;
    }
    uint8_t before[40] = {0};
    const size_t read1 = static_cast<size_t>(f.read(before, sizeof(before)));
    const size_t pos = f.position();
    const bool parked = Storage.quiesceForLightSleep();
    const bool restored = Storage.restoreAfterLightSleep();
    uint8_t after[40] = {0};
    const int n2 = f.read(after, sizeof(after));
    const size_t read2 = n2 > 0 ? static_cast<size_t>(n2) : 0;

    bool match = false;
    {
        HalFile fresh = Storage.open(logical, O_RDONLY);
        if (fresh && fresh.seek(pos)) {
            uint8_t ref[40] = {0};
            const int n = fresh.read(ref, sizeof(ref));
            const size_t refLen = n > 0 ? static_cast<size_t>(n) : 0;
            match = refLen == read2 && (read2 == 0 || memcmp(ref, after, read2) == 0);
        }
    }
    bool contiguous = read1 == sizeof(before) && read2 == sizeof(after) &&
                      (memcmp(before, pattern, read1) == 0) &&
                      (memcmp(after, pattern + pos, read2) == 0);
    printf("sdpark: read1=%u pos=%u park=%d restore=%d read2=%u match=%d contiguous=%d\n",
           static_cast<unsigned>(read1), static_cast<unsigned>(pos), parked ? 1 : 0, restored ? 1 : 0,
           static_cast<unsigned>(read2), match ? 1 : 0, contiguous ? 1 : 0);
    unlink(physical);
}

void consoleSdStress(const char* args) {
    int rounds = 10;
    if (args != nullptr) (void)sscanf(args, "%d", &rounds);
    if (rounds < 1) rounds = 1;
    if (rounds > 200) rounds = 200;
    // 64 KiB in PSRAM, not .bss: a static array here cost 64 KiB of internal RAM
    // for the whole session, which starved esp_wifi_init and the FATFS windows.
    constexpr size_t kPatternBytes = 64 * 1024;
    uint8_t* pattern = static_cast<uint8_t*>(heap_caps_malloc(kPatternBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pattern == nullptr) {
        printf("sdstress: no PSRAM for the pattern\n");
        return;
    }
    for (size_t i = 0; i < kPatternBytes; ++i) pattern[i] = static_cast<uint8_t>((i * 7U + 3U) & 0xFFU);
    const char* path = "/sdcard/.plus_stress.bin";
    printf("sdstress: %d rounds of 64 KiB write + full scan\n", rounds);
    int failures = 0;
    for (int r = 0; r < rounds; ++r) {
        const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        bool ok = fd >= 0;
        if (ok) {
            ok = write(fd, pattern, kPatternBytes) == static_cast<ssize_t>(kPatternBytes);
            close(fd);
        }
        if (!ok) ++failures;
        fflush(stdout);
        display.refreshDisplay(HalDisplay::FULL_REFRESH, false);
        vTaskDelay(pdMS_TO_TICKS(200));
        printf("sdstress: round %d %s\n", r + 1, ok ? "ok" : "WRITE-FAILED");
        fflush(stdout);
    }
    unlink(path);
    heap_caps_free(pattern);
    printf("sdstress: %d/%d failed\n", failures, rounds);
}

void consoleSdWrite() {
    const char* path = "/sdcard/.plus_sd_test";
    uint8_t pattern[4096];
    for (size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = static_cast<uint8_t>((i * 31U) & 0xFFU);
    const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("sdwrite: cannot create %s\n", path);
        return;
    }
    bool ok = write(fd, pattern, sizeof(pattern)) == static_cast<ssize_t>(sizeof(pattern));
    if (ok) ok = lseek(fd, 0, SEEK_SET) == 0;
    uint8_t readback[sizeof(pattern)] = {};
    if (ok) ok = read(fd, readback, sizeof(readback)) == static_cast<ssize_t>(sizeof(readback));
    close(fd);
    ok = ok && memcmp(pattern, readback, sizeof(pattern)) == 0;
    const bool removed = unlink(path) == 0;
    printf("sdwrite: %s (%u bytes, removed=%d)\n", ok && removed ? "PASS" : "FAIL",
           static_cast<unsigned>(sizeof(pattern)), removed ? 1 : 0);
}

// Scan the shared I2C1 bus and label the parts the board expects. This is the
// quickest first-power-on check for "which device is not answering".
void consoleI2C() {
    if (!metalio_i2c1_lock(200)) {
        printf("i2c: bus busy\n");
        return;
    }
    printf("i2c1 scan:");
    for (int addr = 0x08; addr < 0x78; ++addr) {
        if (metalio_i2c1_probe_locked(I2C_NUM_1, static_cast<uint8_t>(addr), 20) == ESP_OK) {
            printf(" 0x%02X", addr);
        }
    }
    printf("\n");
    metalio_i2c1_unlock();
    printf("expected: 0x20 TCA9555, 0x38 FT6336U, 0x51 PCF8563, 0x55 BQ27220, "
           "0x68 TPS65185, 0x6B CX25601N, 0x19 SC7A20H\n");
}

// Bring-up: count dark pixels in a logical-y band of the live framebuffer, and
// wipe the framebuffer, so a stale-vs-fresh-ink question can be answered
// without guessing.
void consoleFbInk(const char* args) {
    int y0 = 0, y1 = 0;
    if (args == nullptr || sscanf(args, "%d %d", &y0, &y1) != 2) {
        printf("fbink: usage fbink <y0> <y1>\n");
        return;
    }
    const uint8_t* fb = HalDisplay::frameBufferSnapshot();
    if (fb == nullptr) {
        printf("fbink: no framebuffer\n");
        return;
    }
    const int h = HalDisplay::DISPLAY_HEIGHT;
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > h ? h : y1;
    uint32_t ink = 0;
    for (int y = y0; y < y1; ++y) {
        const uint8_t* row = fb + static_cast<size_t>(y) * HalDisplay::DISPLAY_WIDTH_BYTES;
        for (int i = 0; i < HalDisplay::DISPLAY_WIDTH_BYTES; ++i) ink += 8 - __builtin_popcount(row[i]);
    }
    printf("fbink %d..%d: %u ink pixels\n", y0, y1, static_cast<unsigned>(ink));
}

void consoleFbClear() {
    GfxRenderer& r = renderer;
    r.clearScreen();
    r.displayBuffer();
    printf("fbclear: framebuffer wiped and pushed\n");
}

// Dump the live 1-bit framebuffer as a BMP over the serial console. Works
// without Wi-Fi, which matters for the very first power-on session.
void consoleScreenshot() {    const uint8_t* frame = HalDisplay::frameBufferSnapshot();
    if (frame == nullptr) {
        printf("screenshot: no framebuffer yet\n");
        return;
    }
    const int w = HalDisplay::DISPLAY_WIDTH;
    const int h = HalDisplay::DISPLAY_HEIGHT;
    const size_t size = bmp_export::fileSize(w, h);
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) buffer = static_cast<uint8_t*>(malloc(size));
    if (buffer == nullptr) {
        printf("screenshot: out of memory (%u bytes)\n", (unsigned)size);
        return;
    }
    if (!bmp_export::writeMonoBmpTo(buffer, size, frame, HalDisplay::DISPLAY_WIDTH_BYTES, w, h,
                                    HalDisplay::frameBufferInverted())) {
        free(buffer);
        printf("screenshot: encode failed\n");
        return;
    }
    printf("SCREENSHOT_START:%u\n", (unsigned)size);
    fflush(stdout);
    fwrite(buffer, 1, size, stdout);
    fflush(stdout);
    printf("\nSCREENSHOT_END\n");
    fflush(stdout);
    free(buffer);
}

// 8-bit BMP of the retained 4-bit canvas. A 1-bit SCREENSHOT cannot show a
// grayscale page faithfully: its proxy frame only holds the pixels the AA path
// wrote, so untouched margins keep whatever the previous page left there.
void consoleScreenshotGray() {
    const uint8_t* gray = display.getGrayscale16Buffer();
    if (gray == nullptr) {
        printf("screenshotgray: no grayscale canvas (page was never 16-level)\n");
        return;
    }
    const int w = HalDisplay::DISPLAY_WIDTH;
    const int h = HalDisplay::DISPLAY_HEIGHT;
    const uint32_t rowBytes = static_cast<uint32_t>(w);
    const uint32_t dataSize = rowBytes * static_cast<uint32_t>(h);
    const uint32_t paletteBytes = 256 * 4;
    const uint32_t offset = 14 + 40 + paletteBytes;
    const uint32_t size = offset + dataSize;
    auto* out = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (out == nullptr) out = static_cast<uint8_t*>(malloc(size));
    if (out == nullptr) {
        printf("screenshotgray: out of memory (%u bytes)\n", (unsigned)size);
        return;
    }
    auto u16 = [](uint8_t* p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; };
    auto u32 = [](uint8_t* p, uint32_t v) {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
    };
    uint8_t* p = out;
    p[0] = 'B'; p[1] = 'M';
    u32(p + 2, size); u32(p + 6, 0); u32(p + 10, offset);
    u32(p + 14, 40); u32(p + 18, static_cast<uint32_t>(w)); u32(p + 22, static_cast<uint32_t>(h));
    u16(p + 26, 1); u16(p + 28, 8); u32(p + 30, 0); u32(p + 34, dataSize);
    u32(p + 38, 2835); u32(p + 42, 2835); u32(p + 46, 256); u32(p + 50, 0);
    for (int i = 0; i < 256; ++i) {
        out[54 + i * 4 + 0] = static_cast<uint8_t>(i);
        out[54 + i * 4 + 1] = static_cast<uint8_t>(i);
        out[54 + i * 4 + 2] = static_cast<uint8_t>(i);
        out[54 + i * 4 + 3] = 0;
    }
    uint8_t* pixels = out + offset;
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* src = gray + static_cast<size_t>(y) * (w / 2);
        uint8_t* dst = pixels + static_cast<size_t>(h - 1 - y) * rowBytes;
        for (int x = 0; x < w; ++x) {
            const uint8_t nibble = (src[x >> 1] >> ((x & 1) * 4)) & 0x0F;
            const uint8_t value = display.frameBufferInverted() ? 15 - nibble : nibble;
            dst[x] = static_cast<uint8_t>(value * 17);
        }
    }
    printf("SCREENSHOT_START:%u\n", (unsigned)size);
    fflush(stdout);
    fwrite(out, 1, size, stdout);
    fflush(stdout);
    printf("\nSCREENSHOT_END\n");
    fflush(stdout);
    free(out);
}

// Write a small file from the console, 64 bytes per chunk with an ACK per
// chunk. Bigger writes over the USB CDC lose bytes mid-stream (observed on the
// reference board), so keep the chunk small and wait for each ACK.
void consolePutFile(const char* args) {
    char path[192] = {};
    unsigned size = 0;
    if (args == nullptr || sscanf(args, "%191s %u", path, &size) != 2 || path[0] == '\0' || size == 0) {
        printf("usage: putfile </path/on/card> <bytes>\n");
        return;
    }
    char full[224];
    if (strncmp(path, "/sdcard", 7) == 0) {
        snprintf(full, sizeof(full), "%s", path);
    } else {
        snprintf(full, sizeof(full), "/sdcard%s", path);
    }
    const int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("putfile: cannot open %s\n", full);
        return;
    }
    printf("PF_READY\n");
    fflush(stdout);
    uint8_t buf[64];
    unsigned got = 0;
    uint32_t lastDataMs = millis();
    while (got < size) {
        const size_t want = std::min<size_t>(sizeof(buf), size - got);
        const size_t n = fread(buf, 1, want, stdin);
        if (n == 0) {
            if (millis() - lastDataMs > 15000) {
                printf("putfile: timeout at %u/%u\n", got, size);
                close(fd);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (write(fd, buf, n) != static_cast<ssize_t>(n)) {
            printf("putfile: write failed at %u\n", got);
            close(fd);
            return;
        }
        got += static_cast<unsigned>(n);
        lastDataMs = millis();
        printf("PF_ACK %u\n", got);
        fflush(stdout);
    }
    close(fd);
    printf("putfile: ok %u bytes\n", got);
}

// Flow-controlled bulk upload for images/assets that are too large for the
// 64-byte putfile loop (and for when USB MSC/Wi-Fi is unavailable).  The host
// sends at most one block, then waits for PFB_ACK before sending more, so the
// USB-Serial/JTAG RX FIFO cannot be overrun; the stream is CRC32-checked and
// the host compares it before trusting the file.
void consolePutFileFast(const char* args) {
    char path[192] = {};
    unsigned size = 0;
    if (args == nullptr || sscanf(args, "%191s %u", path, &size) != 2 || path[0] == '\0' || size == 0) {
        printf("usage: putfilefast </path/on/card> <bytes>\n");
        return;
    }
    char full[224];
    if (strncmp(path, "/sdcard", 7) == 0) {
        snprintf(full, sizeof(full), "%s", path);
    } else {
        snprintf(full, sizeof(full), "/sdcard%s", path);
    }
    const int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("putfilefast: cannot open %s\n", full);
        return;
    }
    constexpr size_t kBlock = 2048;
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(kBlock, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        printf("putfilefast: no PSRAM\n");
        close(fd);
        return;
    }
    printf("PFB_READY\n");
    fflush(stdout);
    unsigned got = 0;
    uint32_t crc = 0xffffffffu;
    uint32_t lastDataMs = millis();
    while (got < size) {
        const size_t want = std::min<size_t>(kBlock, size - got);
        // Drain a complete block before writing/ACKing.  The USB CDC delivers
        // in small packets (64 B); ACKing each packet turned one 1 KiB block
        // into sixteen host round trips and made the transfer far slower than
        // the link allows.
        size_t filled = 0;
        while (filled < want) {
            const size_t n = fread(buf + filled, 1, want - filled, stdin);
            if (n == 0) {
                if (millis() - lastDataMs > 20000) {
                    printf("putfilefast: timeout at %u/%u\n", got, size);
                    heap_caps_free(buf);
                    close(fd);
                    return;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            filled += n;
            lastDataMs = millis();
        }
        if (write(fd, buf, want) != static_cast<ssize_t>(want)) {
            printf("putfilefast: write failed at %u\n", got);
            heap_caps_free(buf);
            close(fd);
            return;
        }
        for (size_t i = 0; i < want; ++i) {
            crc ^= buf[i];
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        got += static_cast<unsigned>(want);
        printf("PFB_ACK %u\n", got);
        fflush(stdout);
    }
    heap_caps_free(buf);
    close(fd);
    printf("PFB_DONE %u %08x\n", got, static_cast<unsigned>(~crc));
}


// Direct SecureClient probe: separates TCP/TLS connect from the read path.
void consoleTlsProbe(const char* args) {
    char host[128] = {};
    if (args == nullptr || sscanf(args, "%127s", host) != 1 || host[0] == '\0') {
        printf("usage: tlsprobe <host> [path]\n");
        return;
    }
    char path[128] = "/";
    char extra[128] = {};
    if (sscanf(args, "%*s %127s", extra) == 1 && extra[0] == '/') {
        strncpy(path, extra, sizeof(path) - 1);
    }
    freeink::SecureClient client;
    client.setInsecure();
    client.setTimeout(15000);
    const uint32_t t0 = millis();
    const int ok = client.connect(host, 443);
    printf("tlsprobe: connect=%d in %ums\n", ok, static_cast<unsigned>(millis() - t0));
    if (ok != 1) return;
    char req[256];
    const int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    const size_t sent = client.write(reinterpret_cast<const uint8_t*>(req), static_cast<size_t>(n));
    printf("tlsprobe: wrote %u\n", static_cast<unsigned>(sent));
    char buf[256];
    const uint32_t t1 = millis();
    int total = 0;
    while (millis() - t1 < 12000) {
        const int got = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (got <= 0) {
            if (!client.connected()) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (total == 0) printf("tlsprobe: first bytes after %ums\n", static_cast<unsigned>(millis() - t1));
        total += got;
        if (total >= 1024) break;
    }
    printf("tlsprobe: read %d bytes in %ums\n", total, static_cast<unsigned>(millis() - t1));
    client.stop();
}

// Plain-HTTP probe: drive the compat Arduino Client exactly the way the SDK's
// SecureHttpClient does (available() gating for the header lines, read() for
// the body) and print what each primitive returns. A 200 in the server log plus
// zero bytes here isolates the defect to the Arduino Client shim rather than to
// DNS, routing or the peer.
void consoleHttpProbe(const char* args) {
    char host[64] = {};
    char path[64] = "/";
    int port = 80;
    const int parsed = args != nullptr ? sscanf(args, "%63s %d %63s", host, &port, path) : 0;
    if (parsed < 1 || host[0] == '\0') {
        printf("usage: httpprobe <host> [port] [path]\n");
        return;
    }
    WiFiClient client;
    client.setTimeout(5000);
    const uint32_t t0 = millis();
    const int ok = client.connect(host, static_cast<uint16_t>(port));
    printf("httpprobe: connect=%d in %ums\n", ok, static_cast<unsigned>(millis() - t0));
    if (ok != 1) return;
    char req[256];
    const int n = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: cpyos-probe\r\nConnection: close\r\n\r\n", path,
                           host);
    const size_t sent = client.write(reinterpret_cast<const uint8_t*>(req), static_cast<size_t>(n));
    printf("httpprobe: wrote %u of %d\n", static_cast<unsigned>(sent), n);
    int total = 0;
    int loops = 0;
    int availSeen = 0;
    int connectedSeen = 0;
    const uint32_t t1 = millis();
    while (millis() - t1 < 8000) {
        const int av = client.available();
        const int co = client.connected();
        if (av > 0) ++availSeen;
        if (co) ++connectedSeen;
        char buf[128];
        const int got = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (got > 0) {
            total += got;
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ++loops;
        if (total > 0 && got <= 0) break;
    }
    printf("httpprobe: read %d bytes in %ums (loops=%d avail>0=%d connected=%d)\n", total,
           static_cast<unsigned>(millis() - t1), loops, availSeen, connectedSeen);
    client.stop();
}

// Jump straight into the browser with a URL (same entry the UI's address bar
// uses). The activity is created through ActivityManager, which queues the
// switch for the UI task, so poll until it is current before navigating.
void consoleBrowser(const char* args) {
    if (args == nullptr || *args == '\0') {
        printf("usage: browser <url>\n");
        return;
    }
    activityManager.goToWebBrowser();
    for (int i = 0; i < 60; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
        Activity* activity = activityManager.currentActivityPtr();
        if (activity != nullptr && strcmp(activity->debugName(), "WebBrowser") == 0) {
            activity->debugBrowserOpen(args);
            printf("browser: navigating to %s\n", args);
            return;
        }
    }
    printf("browser: WebBrowser activity did not start\n");
}

void consoleWindow() {
    const int x = 40;
    const int y = 40;
    const int w = 220;
    const int h = 140;
    renderer.fillRect(x, y, w, h, true);
    renderer.displayWindow(x, y, w, h);
    printf("window: %d,%d %dx%d pushed; pixels outside must keep their optical state\n", x, y, w, h);
}

// Drive the motor directly (TCA P0.3) to check polarity/feel without a key edge.
void consoleHaptic(const char* args) {
    int ms = args ? atoi(args) : 35;
    if (ms <= 0 || ms > 500) ms = 35;
    printf("haptic: level=%u pulse=%dms\n", (unsigned)SETTINGS.hapticFeedbackLevel, ms);
    epd_board_metalio_eink4_plus_motor_set(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    epd_board_metalio_eink4_plus_motor_set(false);
}

void consoleInfo() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t* app = esp_app_get_description();
    printf("settings: language=%u fontScale=%u density=%u theme=%u inverted=%u\n",
           (unsigned)SETTINGS.language, (unsigned)SETTINGS.uiFontScale,
           (unsigned)SETTINGS.uiDensity, (unsigned)SETTINGS.uiTheme,
           (unsigned)SETTINGS.screenInverted);
    // 0 = never. Bench images migrate this to 0 once; a value picked in
    // Settings must survive, so this line is the regression check.
    printf("sleepTimeoutMinutes=%u\n", (unsigned)SETTINGS.sleepTimeoutMinutes);
    printf("chip=%s rev=%d cores=%d version=%s built=%s %s\n", CONFIG_IDF_TARGET, chip.revision, chip.cores,
           app ? app->version : "?", app ? app->date : "?", app ? app->time : "?");
    printf("internal free=%u min=%u largest=%u | psram free=%u largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    printf("uptime=%llds usb=%d\n", (long long)(esp_timer_get_time() / 1000000),
           gpio.isUsbConnected() ? 1 : 0);
}

void consoleBattery() {
    plus_battery_sample_t sample{};
    if (!PlusBattery_Read(&sample)) {
        printf("battery: no answer from BQ27220\n");
        return;
    }
    plus_charger_sample_t charger{};
    const bool haveCharger = PlusCharger_Read(&charger) && charger.valid;
    printf("battery: %d mV %d mA %d%% valid=%d charging=%d\n", sample.millivolts, sample.milliamps,
           sample.soc_percent, sample.valid ? 1 : 0, PlusBattery_IsCharging() ? 1 : 0);
    printf("charger: %s vbus=%s chg=%s\n", haveCharger ? "ok" : "no answer",
           haveCharger ? PlusCharger_VbusName(charger.vbus) : "?",
           haveCharger ? PlusCharger_ChargeName(charger.chg_stat) : "?");
}

extern "C" void epd_set_vcom(uint16_t vcom);

// Bench-only VCOM compare: 1200 is the vendor default, kiikoread measured a
// darker optimum on its unit. The value lives in the TPS65185 and is re-written
// on every power-on, so a reboot restores the firmware default.
void consoleVcom(const char* args) {
    const int mv = args != nullptr && *args != '\0' ? atoi(args) : -1;
    if (mv < 900 || mv > 4000) {
        printf("vcom: usage vcom <900..4000> (current default 1200)\n");
        return;
    }
    epd_set_vcom(static_cast<uint16_t>(mv));
    activityManager.requestUpdate(true);
    printf("vcom: %d mV (applied on the next panel power-on)\n", mv);
}

void consoleRtc() {
    struct tm utc{};
    char text[32] = "?";
    if (PlusRtc_Read(&utc)) {
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &utc);
    }
    printf("pcF8563: %s (%s) | system utc=%lld\n", PlusRtc_Ok() ? text : "unset", PlusRtc_Ok() ? "ok" : "invalid",
           (long long)halClock.nowUtc());
}

void consoleSd() {
    printf("sd: %s\n", cpyos_sd_is_mounted() ? "mounted" : "NOT mounted");
    uint64_t total = 0;
    uint64_t freeBytes = 0;
    if (Storage.getSpace(total, freeBytes)) {
        printf("  space: %.1f / %.1f MB free\n", static_cast<double>(freeBytes) / (1024.0 * 1024.0),
               static_cast<double>(total) / (1024.0 * 1024.0));
    } else {
        printf("  space: unknown (getSpace failed)\n");
    }
    DIR* dir = opendir("/sdcard");
    if (dir == nullptr) {
        printf("  opendir(/sdcard) failed\n");
        return;
    }
    int shown = 0;
    while (struct dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        printf("  %s%s\n", entry->d_name, entry->d_type == DT_DIR ? "/" : "");
        if (++shown >= 12) {
            printf("  ...\n");
            break;
        }
    }
    closedir(dir);
}

// Read the last N bytes of a card file. `cat` starts at offset 0, so the
// mirrored log was impossible to inspect once it grew past the 8 KiB window.
void consoleTail(const char* args) {
    char path[128] = {};
    unsigned long maxBytes = 4096;
    if (args == nullptr || *args == '\0' || sscanf(args, "%127s %lu", path, &maxBytes) < 1) {
        printf("usage: tail </path> [maxBytes]\n");
        return;
    }
    HalFile f = Storage.open(path, O_RDONLY);
    if (!f) {
        printf("tail: cannot open %s\n", path);
        return;
    }
    const unsigned long size = static_cast<unsigned long>(f.size());
    const unsigned long toSend = size < maxBytes ? size : maxBytes;
    if (!f.seek(size - toSend)) {
        printf("tail: seek failed\n");
        return;
    }
    printf("TAIL_START:%lu of %lu\n", toSend, size);
    fflush(stdout);
    uint8_t buf[512];
    unsigned long left = toSend;
    while (left > 0) {
        const size_t want = left < sizeof(buf) ? left : sizeof(buf);
        const int n = f.read(buf, want);
        if (n <= 0) break;
        fwrite(buf, 1, static_cast<size_t>(n), stdout);
        left -= static_cast<unsigned long>(n);
    }
    fflush(stdout);
    printf("\nTAIL_END\n");
    fflush(stdout);
}

void consoleCat(const char* args) {
    // Dump up to N bytes of a card file to the console. Bench-only: it exists so
    // the SD log mirror can be read back even when the console drops lines
    // during a fast render (that lossiness is why a ghost draw went unnoticed).
    if (args == nullptr || *args == '\0') {
        printf("usage: cat </path> [maxBytes]\n");
        return;
    }
    char path[128] = {};
    unsigned long maxBytes = 8192;
    if (sscanf(args, "%127s %lu", path, &maxBytes) < 1) return;
    HalFile f = Storage.open(path, O_RDONLY);
    if (!f) {
        printf("cat: cannot open %s\n", path);
        return;
    }
    const unsigned long size = static_cast<unsigned long>(f.size());
    unsigned long toSend = size < maxBytes ? size : maxBytes;
    printf("CAT_START:%lu of %lu\n", toSend, size);
    fflush(stdout);
    uint8_t buf[512];
    while (toSend > 0) {
        const size_t want = toSend < sizeof(buf) ? toSend : sizeof(buf);
        const int n = f.read(buf, want);
        if (n <= 0) break;
        fwrite(buf, 1, static_cast<size_t>(n), stdout);
        toSend -= static_cast<unsigned long>(n);
    }
    fflush(stdout);
    printf("\nCAT_END\n");
    fflush(stdout);
    f.close();
}

void consoleList(const char* args) {
    // One level of names + sizes; the bench needs the real JJWXC/CBZ paths for
    // `open`, and there is no file browser over serial.
    const char* path = (args != nullptr && *args != '\0') ? args : "/";
    std::vector<String> names = Storage.listFiles(path, 60);
    if (names.empty()) {
        printf("ls: %s: empty or unreadable\n", path);
        return;
    }
    printf("ls %s: %u entries\n", path, (unsigned)names.size());
    for (const String& name : names) {
        const std::string full = std::string(path) + (path[strlen(path) - 1] == '/' ? "" : "/") + name.c_str();
        HalFile f = Storage.open(full.c_str(), O_RDONLY);
        const unsigned long size = f ? static_cast<unsigned long>(f.size()) : 0UL;
        const bool dir = f && f.isDirectory();
        f.close();
        printf("  %s%s %lu\n", name.c_str(), dir ? "/" : "", size);
    }
}

void consoleTouchRaw() {
    printf("touchraw: 10 samples (raw portrait frame), 100 ms apart\n");
    for (int i = 0; i < 10; ++i) {
        metalio_touch_point_t point{};
        const bool contact = metalio_touch_read(&point);
        printf("  [%2d] contact=%d raw=(%d,%d)\n", i, contact ? 1 : 0, point.raw_x, point.raw_y);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void consoleRtcSet(const char* args) {
    long long epoch = 0;
    if (sscanf(args, "%lld", &epoch) != 1 || epoch <= 0) {
        printf("rtcset: usage rtcset <unix-seconds-utc>\n");
        return;
    }
    const time_t value = static_cast<time_t>(epoch);
    struct tm utc{};
    gmtime_r(&value, &utc);
    const bool ok = PlusRtc_Write(&utc);
    printf("rtcset: %s %04d-%02d-%02d %02d:%02d:%02d UTC\n", ok ? "ok" : "FAILED", utc.tm_year + 1900,
           utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
}

void consoleSelfTest() {
    consoleInfo();
    consoleBattery();
    consoleRtc();
    consoleSd();
    printf("imu: %s\n", halTiltSensor.isAvailable() ? "available" : "not detected");
    printf("touch: present=%d\n", gpio.hasTouch() ? 1 : 0);
    printf("crypto: %s\n", freeink::content::portContentCryptoSelfTest() ? "ok" : "FAILED");
    printf("frontlight: present=%d warm=%d\n", Frontlight.present() ? 1 : 0,
           Frontlight.hasColorTemperature() ? 1 : 0);
}

// Diagnostic for the TLS handshake failure "mbedtls_ssl_handshake returned
// -0x7F80" (MBEDTLS_ERR_SSL_HW_ACCEL_FAILED): the error is a collapsed PSA
// status, so probe each primitive the TLS 1.2 handshake needs and print the
// real status per step.
void consoleCryptoTest() {
    psa_status_t st = psa_crypto_init();
    printf("psa init: %d\n", static_cast<int>(st));
    const uint8_t key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                             0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&a, 128);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&a, PSA_ALG_GCM);
        mbedtls_svc_key_id_t k = MBEDTLS_SVC_KEY_ID_INIT;
        st = psa_import_key(&a, key, sizeof(key), &k);
        printf("gcm import: %d\n", static_cast<int>(st));
        if (st == PSA_SUCCESS) {
            uint8_t iv[12] = {0};
            uint8_t pt[16] = {0};
            uint8_t out[64];
            size_t olen = 0;
            st = psa_aead_encrypt(k, PSA_ALG_GCM, iv, sizeof(iv), nullptr, 0, pt, sizeof(pt), out,
                                  sizeof(out), &olen);
            printf("gcm encrypt: %d len=%u\n", static_cast<int>(st), static_cast<unsigned>(olen));
            st = psa_aead_decrypt(k, PSA_ALG_GCM, iv, sizeof(iv), nullptr, 0, out, olen, out,
                                  sizeof(out), &olen);
            printf("gcm decrypt: %d len=%u\n", static_cast<int>(st), static_cast<unsigned>(olen));
            psa_destroy_key(k);
        }
    }
    {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
        psa_set_key_bits(&a, 128);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
        psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
        mbedtls_svc_key_id_t k = MBEDTLS_SVC_KEY_ID_INIT;
        st = psa_import_key(&a, key, sizeof(key), &k);
        printf("hmac import: %d\n", static_cast<int>(st));
        if (st == PSA_SUCCESS) {
            uint8_t mac[32];
            size_t mlen = 0;
            st = psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), reinterpret_cast<const uint8_t*>("abc"), 3,
                                 mac, sizeof(mac), &mlen);
            printf("hmac compute: %d len=%u\n", static_cast<int>(st), static_cast<unsigned>(mlen));
            psa_destroy_key(k);
        }
    }
    {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_DERIVE);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
        psa_set_key_algorithm(&a, PSA_ALG_TLS12_PRF(PSA_ALG_SHA_256));
        mbedtls_svc_key_id_t k = MBEDTLS_SVC_KEY_ID_INIT;
        st = psa_import_key(&a, key, sizeof(key), &k);
        printf("tls12prf import: %d\n", static_cast<int>(st));
        if (st == PSA_SUCCESS) {
            static const uint8_t seed[4] = {1, 2, 3, 4};
            static const uint8_t label[4] = {'a', 'b', 'c', 'd'};
            psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
            st = psa_key_derivation_setup(&op, PSA_ALG_TLS12_PRF(PSA_ALG_SHA_256));
            printf("tls12prf setup: %d\n", static_cast<int>(st));
            if (st == PSA_SUCCESS) {
                st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SEED, seed, sizeof(seed));
                printf("tls12prf seed: %d\n", static_cast<int>(st));
            }
            if (st == PSA_SUCCESS) {
                st = psa_key_derivation_input_key(&op, PSA_KEY_DERIVATION_INPUT_SECRET, k);
                printf("tls12prf secret: %d\n", static_cast<int>(st));
            }
            if (st == PSA_SUCCESS) {
                st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_LABEL, label, sizeof(label));
                printf("tls12prf label: %d\n", static_cast<int>(st));
            }
            uint8_t out[32];
            size_t olen = 0;
            if (st == PSA_SUCCESS) {
                st = psa_key_derivation_output_bytes(&op, out, sizeof(out));
                printf("tls12prf output: %d len=%u\n", static_cast<int>(st), static_cast<unsigned>(olen));
            }
            psa_key_derivation_abort(&op);
            psa_destroy_key(k);
        }
    }
    {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&a, 256);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
        psa_set_key_algorithm(&a, PSA_ALG_ECDH);
        mbedtls_svc_key_id_t k = MBEDTLS_SVC_KEY_ID_INIT;
        st = psa_generate_key(&a, &k);
        printf("ecdh gen: %d\n", static_cast<int>(st));
        if (st == PSA_SUCCESS) psa_destroy_key(k);
    }
}

void consoleLight(const char* args) {
    int brightness = 0;
    int warmth = -1;
    const int parsed = sscanf(args, "%d %d", &brightness, &warmth);
    if (parsed < 1) {
        printf("light: usage light <0-100> [warm 0-100] (present=%d warm=%d)\n", Frontlight.present() ? 1 : 0,
               Frontlight.hasColorTemperature() ? 1 : 0);
        return;
    }
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;
    Frontlight.setBrightness((uint8_t)brightness);
    if (warmth >= 0) Frontlight.setWarmth((uint8_t)(warmth > 100 ? 100 : warmth));
    Frontlight.setOn(brightness > 0);
    uint32_t coolDuty = 0;
    uint32_t warmDuty = 0;
    Frontlight.debugDuties(coolDuty, warmDuty);
    printf("light: on=%d brightness=%u warmth=%u present=%d duty_cool=%lu duty_warm=%lu (10-bit, 100%%=102 of 1023)\n",
           Frontlight.isOn() ? 1 : 0, (unsigned)Frontlight.brightness(), (unsigned)Frontlight.warmth(),
           Frontlight.present() ? 1 : 0, (unsigned long)coolDuty, (unsigned long)warmDuty);
}

void consoleAp(const char* args) {
    (void)args;
    // The S31 SoftAP path in the precompiled lib needs a beacon event buffer
    // that this image cannot provide and faults instead of failing cleanly.
    printf("ap: SoftAP unavailable on this image; use web <ssid> <pass> on a known network\n");
}

void consoleApOff() {
    web_transfer::stop();
    NetworkStartup::releaseRadio();
    printf("ap: off (mode=%d)\n", (int)WiFi.getMode());
}

// Drop any STA/AP session and deinitialize the radio (same path network
// activities use on exit). BLE is stopped by NetworkStartup::setMode only, so
// this is for bench work.
void consoleRadioOff() {
    web_transfer::stop();
    NetworkStartup::releaseRadio();
    printf("radio: off (mode=%d)\n", (int)WiFi.getMode());
}

void consoleWeb(const char* args) {
    char ssid[33] = {};
    char pass[65] = {};
    const int parsed = sscanf(args, "%32s %64s", ssid, pass);
    IPAddress ip;
    // See consoleAp: release render memory before the Wi-Fi driver allocates.
    NetworkStartup::prepare(renderer);
    if (parsed >= 1) {
        printf("web: SoftAP unavailable on this image; join a network first (wificonnect) and run web\n");
        return;
    }
    ip = WiFi.localIP();
    if (static_cast<uint32_t>(ip) == 0) {
        // Join the last saved network without handling its password here: the
        // store already has it (the UI Wi-Fi sheet does the same).
        WIFI_STORE.loadFromFile();
        const std::string last = WIFI_STORE.getLastConnectedSsid();
        const auto saved = last.empty() ? std::nullopt : WIFI_STORE.findCredential(last);
        if (saved) {
            printf("web: connecting saved network '%s'\n", saved->ssid.c_str());
            (void)WiFi.begin(saved->ssid.c_str(), saved->password.c_str());
            const unsigned long deadline = millis() + 20000;
            while (millis() < deadline && WiFi.status() != WL_CONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            ip = WiFi.localIP();
        }
        if (static_cast<uint32_t>(ip) == 0) {
            printf("web: no Wi-Fi. Connect a network in Settings, or wificonnect <ssid> <password>\n");
            return;
        }
    }
    if (!web_transfer::start()) {
        printf("web: server failed to start\n");
        return;
    }
    printf("web: http://%u.%u.%u.%u/  (SD root, no authentication)\n", (unsigned)ip[0], (unsigned)ip[1],
           (unsigned)ip[2], (unsigned)ip[3]);
}

void consoleWebOff() {
    web_transfer::stop();
    printf("web: stopped (ap still on; use apoff to drop it)\n");
}

void consoleBt(const char* args) {
    if (strncmp(args, "off", 3) == 0) {
        BleHid.end();
        printf("bt: stopped\n");
        return;
    }
    if (!BleHid.begin("CPYOS-Plus")) {
        printf("bt: could not start the BLE host (see serial log)\n");
        return;
    }
    if (strncmp(args, "scan", 4) != 0) {
        printf("bt: ready (use bt scan [seconds] to list HID peripherals)\n");
        return;
    }
    int seconds = 8;
    (void)sscanf(args + 4, "%d", &seconds);
    if (seconds < 2) seconds = 2;
    if (seconds > 30) seconds = 30;
    BleHid.startScan(static_cast<uint32_t>(seconds) * 1000);
    const uint32_t deadline = millis() + static_cast<uint32_t>(seconds) * 1000 + 500;
    while (millis() < deadline) {
        BleHid.poll();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    BleHid.stopScan();
    printf("bt: %u device(s)\n", (unsigned)BleHid.deviceCount());
    for (uint8_t i = 0; i < BleHid.deviceCount() && i < 10; ++i) {
        const auto& device = BleHid.device(i);
        printf("  %2d dBm %s %s%s\n", device.rssi, device.addr, device.name, device.hid ? " [HID]" : "");
    }
}

void consoleUsbMsc(const char* args) {
    if (strncmp(args, "off", 3) == 0) {
        usb_msc::stop();
        printf("usbmsc: stopped; rebooting to restore the app filesystem\n");
        fflush(stdout);
        esp_restart();
        return;
    }
    if (!usb_msc::start()) {
        printf("usbmsc: failed (is an SD card mounted?)\n");
        return;
    }
    printf("usbmsc: SD exposed. The app stops using the card; eject and power-cycle\n"
           "        to return to normal operation (console may disappear).\n");
}

void consoleWifi() {
    NetworkStartup::prepare(renderer);
    const int count = WiFi.scanNetworks();
    printf("wifi: %d network(s)\n", count);
    for (int i = 0; i < count && i < 10; ++i) {
        printf("  %2d dBm  auth=%u  %s\n", (int)WiFi.RSSI(i), (unsigned)WiFi.encryptionType(i),
               WiFi.SSID(i).c_str());
    }
}

// Enter a network from the console and store it exactly as the Wi-Fi sheet
// would, so later silent reconnects (JJWXC, font packs, OTA) find it.
void consoleWifiSaved() {
    WIFI_STORE.loadFromFile();
    const auto summaries = WIFI_STORE.getCredentialSummaries();
    printf("wifisaved: %u network(s), last='%s'\n", static_cast<unsigned>(summaries.size()),
           WIFI_STORE.getLastConnectedSsid().c_str());
    for (const auto& entry : summaries) {
        printf("  %s%s%s\n", entry.ssid.c_str(), entry.hasPassword ? " [saved]" : " [no-pass]",
               entry.isLastConnected ? " [last]" : "");
    }
}

void consoleWifiConnect(const char* args) {
    char ssid[64] = {};
    char pass[96] = {};
    if (args == nullptr || sscanf(args, "%63s %95s", ssid, pass) != 2) {
        printf("usage: wificonnect <ssid> <password>\n");
        return;
    }
    const bool stored = WIFI_STORE.addCredential(ssid, pass);
    WIFI_STORE.setLastConnectedSsid(ssid);
    const bool saved = WIFI_STORE.saveToFile();
    printf("wificonnect: stored=%d saved=%d ssid='%s'", stored, saved, ssid);
    NetworkStartup::prepare(renderer);
    const int result = WiFi.begin(ssid, pass);
    printf(" result=%d status=%d ip=%s\n", result, static_cast<int>(WiFi.status()),
           WiFi.localIP().toString().c_str());
}

// Connect using a credential already in the store (no password on the command
// line). WiFi.begin() writes NVS, so the work runs on a short internal-stack
// task: the console itself prefers a PSRAM stack.
void consoleWifiConnectSaved(const char* ssid) {
    if (ssid == nullptr || *ssid == '\0') {
        printf("usage: wificonnect_saved <ssid>\n");
        return;
    }
    WIFI_STORE.loadFromFile();
    std::string pass;
    bool found = false;
    for (size_t i = 0; i < WIFI_STORE.getCredentialCount(); ++i) {
        const auto credential = WIFI_STORE.getCredentialAt(i);
        if (credential && credential->ssid == ssid) {
            pass = credential->password;
            found = true;
            break;
        }
    }
    if (!found) {
        printf("wificonnect_saved: unknown network '%s'\n", ssid);
        return;
    }
    struct Ctx {
        const char* ssid;
        std::string pass;
        volatile int result;
        volatile int status;
        char ip[16];
    } ctx{ssid, pass, -1, -1, {0}};
    auto runner = [](void* raw) {
        auto* c = static_cast<Ctx*>(raw);
        NetworkStartup::prepare(renderer);
        c->result = WiFi.begin(c->ssid, c->pass.c_str());
        const uint32_t started = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) vTaskDelay(pdMS_TO_TICKS(200));
        c->status = static_cast<int>(WiFi.status());
        std::snprintf(c->ip, sizeof(c->ip), "%s", WiFi.localIP().toString().c_str());
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(runner, "wificon", 12288, &ctx, 4, nullptr) != pdPASS) {
        printf("wificonnect_saved: task create failed\n");
        return;
    }
    while (ctx.result < 0) vTaskDelay(pdMS_TO_TICKS(50));
    printf("wificonnect_saved: ssid='%s' result=%d status=%d ip=%s\n", ssid, ctx.result, ctx.status, ctx.ip);
}

// Flash a firmware image already on the SD card into the inactive OTA slot.
// This is the development update path: the board has no EN reset line and the
// running app owns the USB-Serial/JTAG console, so esptool cannot reflash it
// without a power cycle. The device-side write needs no USB at all.
void consoleFlash(const char* args) {
    if (args == nullptr || *args == '\0') {
        printf("usage: flash <sd path>\n");
        return;
    }
    printf("flash: writing %s to the inactive OTA slot (do not power off)\n", args);
    fflush(stdout);
    const firmware_flash::Result result = firmware_flash::flashFromSdPath(args, nullptr, nullptr);
    printf("flash: %s\n", firmware_flash::resultName(result));
    if (result == firmware_flash::Result::OK) {
        printf("flash: ok; rebooting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

// Switch the UI language immediately and persist it.
void consoleLang(const char* args) {
    if (args == nullptr || *args == '\0') {
        printf("language now: %u\n", (unsigned)SETTINGS.language);
        return;
    }
    const bool zh = strcmp(args, "zh") == 0 || strcmp(args, "cn") == 0;
    // Numeric index lets the bench reach a packed language (9 = de) without
    // walking the settings list; the pack must be flashed at spiffs +0x100000.
    const int index = atoi(args);
    const bool numeric = args[0] >= '0' && args[0] <= '9';
    Language lang = zh ? Language::ZH_CN : Language::EN;
    if (numeric) {
        if (index < 0 || index >= static_cast<int>(Language::_COUNT)) {
            printf("language index %d out of range (0..%d)\n", index, static_cast<int>(Language::_COUNT) - 1);
            return;
        }
        lang = static_cast<Language>(index);
    }
    SETTINGS.language = static_cast<uint8_t>(lang);
    SETTINGS.saveToFile();
    I18N.setLanguage(lang);
    printf("language set to %d (i18n_pack packed=%d)\n", static_cast<int>(lang),
           i18n_pack::isPacked(lang) ? 1 : 0);
    activityManager.requestUpdate();
}

// Synthetic input for automated UI walks: tap in logical portrait coordinates
// (0..683, 0..1215) or press a cover key by name.
void consoleTap(const char* args) {
    int lx = 0;
    int ly = 0;
    if (args == nullptr || sscanf(args, "%d %d", &lx, &ly) != 2) {
        printf("usage: tap <lx> <ly>\n");
        return;
    }
    gpio.debugInjectTap(lx, ly);
    activityManager.requestUpdate();
    printf("tap %d,%d\n", lx, ly);
}

// Drive the focused activity's own debug ring/confirm hooks. Activities that
// implement them (JJWXC, settings lists) then navigate exactly as a hardware
// press would, without guessing hit rectangles.
void consoleAct() {
    Activity* a = activityManager.currentActivityPtr();
    printf("act: %s%s\n", a != nullptr ? a->debugName() : "(none)", activityManager.isSwitchPending() ? " (switch pending)" : "");
}

// These hooks must run on the UI task, not here. An activity's confirm handler
// can measure text, and measuring rasterizes a glyph through FreeType -- several
// KB of stack. The console task has 12 KB (enough for FAT walks) and overflowed
// its canary the first time a JJWXC "job busy" message was measured from here.
// Posting the action keeps the console stack shallow and exercises the same task
// context a real touch/key event uses.
static volatile int s_benchAction = 0;  // 1=ring 2=confirm 3=select 4=sync 5=action 6=tab 7=actions
// Bench-only: pretend the power key is held until this millis() deadline so
// `pwrhold` can exercise the production gesture path without a finger.
static volatile unsigned long s_benchPowerHoldUntil = 0;
                                        // 8=ota check 9=ota install url 10=ota channel
static volatile int s_benchArg = 0;
static char s_benchStr[256];

void runPendingBenchAction() {
    const int action = s_benchAction;
    if (action == 0) return;
    s_benchAction = 0;
    Activity* a = activityManager.currentActivityPtr();
    if (a == nullptr) return;
    if (action == 1) {
        a->debugMoveRing(s_benchArg);
    } else if (action == 2) {
        a->debugConfirm();
    } else if (action == 3) {
        a->debugSelect(s_benchArg);
    } else if (action == 4) {
        a->debugSync();
    } else if (action == 5) {
        a->debugActivateAction(s_benchArg);
    } else if (action == 6) {
        a->debugSetTab(s_benchArg);
    } else if (action == 7) {
        a->debugListActions();
    } else if (action == 8) {
        a->debugCheck();
    } else if (action == 9) {
        a->debugInstallUrl(s_benchStr);
    } else if (action == 10) {
        a->debugSetChannel(s_benchArg);
    } else if (action == 11) {
        a->debugOpenPanel(s_benchArg);
    }
    activityManager.requestUpdate();
}

void consoleRing(const char* args) {
    int delta = 0;
    if (args != nullptr) (void)sscanf(args, "%d", &delta);
    s_benchArg = delta;
    s_benchAction = 1;
    printf("ring %d\n", delta);
}

void consoleConfirm() {
    s_benchAction = 2;
    printf("confirm\n");
}


// Serial-only: open a reader toolbar panel by tool index (0 contents, 1 text,
// 2 bookmarks, 3 more). The toolbar itself switches tools with Left/Right, but
// injected keys race the panel render; calling the hook directly is the
// deterministic path a bench needs.
void consoleReaderPanel(const char* args) {
    const int tool = args != nullptr ? atoi(args) : 0;
    s_benchArg = tool;
    s_benchAction = 11;
    printf("readerpanel %d\n", tool);
}

void consoleSelect(const char* args) {
    int index = 0;
    if (args != nullptr) (void)sscanf(args, "%d", &index);
    s_benchArg = index;
    s_benchAction = 3;
    printf("sel %d\n", index);
}

// Coverage sweep on the UI task: switch settings category, list its action
// rows, and open one. Posted like the ring/confirm hooks (see s_benchAction).
// The category is switched separately (`stab`) once the page is on screen,
// because the activity switch is queued for the next UI loop.
void consoleSettings(const char* args) {
    (void)args;
    activityManager.goToSettings();
    printf("settings: opening\n");
}

void consoleUiScale(const char* args) {
    const int value = args != nullptr ? atoi(args) : 0;
    if (value < 100 || value > 125) {
        printf("usage: uiscale 100..125 (current %u)\n", (unsigned)SETTINGS.uiScalePercent);
        return;
    }
    SETTINGS.uiScalePercent = static_cast<uint8_t>(value);
    {
        RenderLock lock;
        UITheme::getInstance().applyRendererGeometry(renderer);
        typography::apply(renderer, SETTINGS.uiFontScale);
    }
    SETTINGS.saveToFile();
    const auto vp = renderer.uiViewport();
    printf("uiscale: %d%% native=%dx%d design=%dx%d scale=%u\n", value, renderer.getNativeWidth(),
           renderer.getNativeHeight(), vp.width, vp.height, (unsigned)vp.scalePermille);
    activityManager.requestUpdate(true);
}

void consoleMy(const char* args) {
    int tab = 0;
    if (args != nullptr && *args != '\0') (void)sscanf(args, "%d", &tab);
    activityManager.replaceActivityWith<MyActivity>(tab);
    printf("my: opening tab %d\n", tab);
}

void consoleStats() {
    activityManager.goToReadingStatsMenu();
    printf("stats: opening reading-stats menu\n");
}

void consoleTab(const char* args) {
    s_benchArg = args != nullptr ? atoi(args) : 0;
    s_benchAction = 6;
    printf("stab %d\n", s_benchArg);
}

void consoleAction(const char* args) {
    s_benchArg = args != nullptr ? atoi(args) : 0;
    s_benchAction = 5;
    printf("saction %d\n", s_benchArg);
}

void consoleListActions() {
    s_benchAction = 7;
    printf("slist\n");
}

// Page-turn animation A/B: arm the next push with the vendor spatial wipe
// (Read Pico's water-ripple analogue) without touching the setting.
void consoleWipe(const char* args) {
    int dir = 1;
    if (args != nullptr && *args != '\0') (void)sscanf(args, "%d", &dir);
    renderer.setPageTurnDirection(dir);
    activityManager.requestUpdate();
    printf("wipe: next push dir=%d\n", dir);
}

// Vendor OTA: open the page, or drive its bench hooks (check / direct URL /
// channel). Network + flash run on the page's worker, never on this task.
void consoleOta(const char* args) {
    if (args == nullptr || *args == '\0') {
        activityManager.replaceActivityWith<PlusOtaActivity>();
        printf("ota: page opened\n");
        return;
    }
    if (strncmp(args, "check", 5) == 0) {
        s_benchAction = 8;
        printf("ota: check\n");
        return;
    }
    if (strncmp(args, "url ", 4) == 0) {
        snprintf(s_benchStr, sizeof(s_benchStr), "%s", args + 4);
        s_benchAction = 9;
        printf("ota: install %s\n", s_benchStr);
        return;
    }
    if (strncmp(args, "channel ", 8) == 0) {
        s_benchArg = atoi(args + 8);
        s_benchAction = 10;
        printf("ota: channel %d (0 stable, 1 test)\n", s_benchArg);
        return;
    }
    printf("usage: ota | ota check | ota url <http url> | ota channel <0|1>\n");
}

void consoleKey(const char* args) {
    if (args == nullptr || *args == '\0') {
        printf("usage: key back|ok|left|right|up|down|power\n");
        return;
    }
    uint8_t button = HalGPIO::BTN_CONFIRM;
    if (strcmp(args, "back") == 0) button = HalGPIO::BTN_BACK;
    else if (strcmp(args, "ok") == 0) button = HalGPIO::BTN_CONFIRM;
    else if (strcmp(args, "left") == 0) button = HalGPIO::BTN_LEFT;
    else if (strcmp(args, "right") == 0) button = HalGPIO::BTN_RIGHT;
    else if (strcmp(args, "up") == 0) button = HalGPIO::BTN_UP;
    else if (strcmp(args, "down") == 0) button = HalGPIO::BTN_DOWN;
    else if (strcmp(args, "power") == 0) button = HalGPIO::BTN_POWER;
    else {
        printf("unknown key %s\n", args);
        return;
    }
    gpio.debugInjectKey(button);
    activityManager.requestUpdate();
    printf("key %s\n", args);
}

// Held contact: touchdown, wait (the UI loop keeps ticking so the 500 ms
// long-press timer fires), then touchup. Unlocks the long-press flows the tap
// injection cannot reach (library actions, word selection, bookmark delete).
void consoleTouchDown(const char* args) {
    int lx = 0;
    int ly = 0;
    if (args == nullptr || sscanf(args, "%d %d", &lx, &ly) != 2) {
        printf("usage: touchdown <lx> <ly>\n");
        return;
    }
    gpio.debugInjectTouchDown(lx, ly);
    activityManager.requestUpdate();
    printf("touchdown %d,%d\n", lx, ly);
}

void consoleTouchUp() {
    gpio.debugInjectTouchUp();
    activityManager.requestUpdate();
    printf("touchup\n");
}

// Synthetic swipe (logical portrait coordinates), for scrolling touch lists.
void consoleSwipe(const char* args) {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (args == nullptr || sscanf(args, "%d %d %d %d", &x0, &y0, &x1, &y1) != 4) {
        printf("usage: swipe <x0> <y0> <x1> <y1>\n");
        return;
    }
    gpio.debugInjectSwipe(x0, y0, x1, y1);
    activityManager.requestUpdate();
    printf("swipe %d,%d -> %d,%d\n", x0, y0, x1, y1);
}

// Defined with the side-key handler below; the bench command reuses it so the
// console path and the physical keys share one volume implementation.
void setSpeakerVolume(int volume, bool persist = true);

void consoleExec(char* line) {
    while (*line == ' ') ++line;
    char* command = line;
    char* args = command;
    while (*args != '\0' && *args != ' ') ++args;
    if (*args == ' ') *args++ = '\0';
    if (*command == '\0') return;
    // The console prefers a PSRAM stack (internal RAM is too fragmented for a
    // 20 KB one). Writing internal flash from PSRAM is a panic, so the few
    // bench commands that do that refuse here instead of wedging the unit.
    if (esp_ptr_external_ram(esp_cpu_get_sp()) &&
        (strcmp(command, "flash") == 0 || strcmp(command, "ota") == 0 ||
         strcmp(command, "sdformat") == 0)) {
        printf("%s: refused on the PSRAM console stack; use esptool or the UI\n", command);
        return;
    }
    if (strcmp(command, "plugindemo") == 0) {
        consolePlugin(args);
    } else if (strcmp(command, "pluginloop") == 0) {
        consolePluginLoop(args);
    } else if (strcmp(command, "help") == 0) {
        consoleHelp();
    } else if (strcmp(command, "flash") == 0) {
        consoleFlash(args);
    } else if (strcmp(command, "lang") == 0) {
        consoleLang(args);
    } else if (strcmp(command, "tap") == 0) {
        consoleTap(args);
    } else if (strcmp(command, "key") == 0) {
        consoleKey(args);
    } else if (strcmp(command, "act") == 0) {
        consoleAct();
    } else if (strcmp(command, "ring") == 0) {
        consoleRing(args);
    } else if (strcmp(command, "confirm") == 0) {
        consoleConfirm();
    } else if (strcmp(command, "readerpanel") == 0) {
        consoleReaderPanel(args);
    } else if (strcmp(command, "sel") == 0) {
        consoleSelect(args);
    } else if (strcmp(command, "settings") == 0) {
        consoleSettings(args);
    } else if (strcmp(command, "uiscale") == 0) {
        consoleUiScale(args);
    } else if (strcmp(command, "camera") == 0) {
        activityManager.goToCamera();
    } else if (strcmp(command, "transfer") == 0) {
        activityManager.replaceActivityWith<PlusTransferActivity>();
    } else if (strcmp(command, "voicememo") == 0) {
        activityManager.replaceActivityWith<PlusVoiceMemoActivity>();
    } else if (strcmp(command, "aivoice") == 0) {
        activityManager.replaceActivityWith<PlusVoiceActivity>();
    } else if (strcmp(command, "otaui") == 0) {
        activityManager.replaceActivityWith<PlusOtaActivity>();
    } else if (strcmp(command, "btaudio") == 0) {
        activityManager.pushActivity(
            std::make_unique<PlusBtAudioActivity>(renderer, mappedInputManager));
    } else if (strcmp(command, "voiceprobe") == 0) {
        // Memory-only probe: start the real voice worker against a refused
        // local port and stop it. Proves the PSRAM stack + internal guard with
        // Wi-Fi resident, without needing a xiaozhi server.
        // A released radio tears lwIP down, so the worker must never open a
        // socket here; the same guard protects the product path.
        if (WiFi.status() != WL_CONNECTED) {
            printf("voiceprobe: no network; connect Wi-Fi or 4G first (start=0)\n");
            return;
        }
        plus_voice::Config cfg{};
        std::strncpy(cfg.url, "ws://127.0.0.1:9/", sizeof(cfg.url) - 1);
        cfg.version = 1;
        const bool started = plus_voice::start(cfg);
        printf("voiceprobe: start=%d phase=%d err=%d largest=%u\n", started ? 1 : 0,
               static_cast<int>(plus_voice::status().phase), plus_voice::status().error,
               static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        if (started) {
            vTaskDelay(pdMS_TO_TICKS(4000));
            plus_voice::stop();
            printf("voiceprobe: stopped phase=%d\n", static_cast<int>(plus_voice::status().phase));
        }
    } else if (strcmp(command, "aikey") == 0) {
        // Bench: inject one AI-key event through the same sampler the loop
        // polls, so the routing/blocklist/page toggle is exercised unchanged.
        if (args != nullptr && strcmp(args, "long") == 0) {
            plus_ai_key::debugInject(plus_ai_key::Event::Long);
            printf("aikey: long queued\n");
        } else if (args != nullptr && strcmp(args, "short") == 0) {
            plus_ai_key::debugInject(plus_ai_key::Event::Short);
            printf("aikey: short queued\n");
        } else {
            printf("aikey: usage aikey short|long\n");
        }
    } else if (strcmp(command, "dns") == 0) {
        // Bench override for resolver diagnostics: the ISP/router here answers
        // app-cdn.jjwxc.net with a dead CDN edge, which makes a firmware
        // failure indistinguishable from a network outage without this.
        if (args == nullptr || *args == '\0') {
            printf("dns: usage dns <a.b.c.d|dhcp>\n");
        } else if (strcmp(args, "dhcp") == 0) {
            printf("dns: dhcp -> %d\n", (int)WiFi.useDhcpDns());
        } else {
            unsigned a = 0, b = 0, c = 0, d = 0;
            if (sscanf(args, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 &&
                d < 256) {
                const bool ok = WiFi.setDNS(IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d),
                                            IPAddress(8, 8, 8, 8));
                printf("dns: %u.%u.%u.%u -> %d\n", a, b, c, d, (int)ok);
            } else {
                printf("dns: bad address '%s'\n", args);
            }
        }
    } else if (strcmp(command, "resolve") == 0) {
        if (args == nullptr || *args == '\0') {
            printf("resolve: usage resolve <host>\n");
        } else {
            IPAddress ip;
            const uint32_t started = millis();
            const bool ok = WiFi.hostByName(args, ip);
            printf("resolve: %s %s %u.%u.%u.%u in %u ms\n", args, ok ? "ok" : "fail", ip[0], ip[1],
                   ip[2], ip[3], (unsigned)(millis() - started));
        }
    } else if (strcmp(command, "vcom") == 0) {
        consoleVcom(args);
    } else if (strcmp(command, "badge") == 0) {
        // An empty argument is a read, not "set 0": `atoi("")` would silently
        // turn the badge off during a diagnostic.
        const bool readOnly = args == nullptr || *args == '\0';
        const int want = readOnly ? -1 : atoi(args);
        if (want < 0) {
            printf("badge: %u\n", static_cast<unsigned>(SETTINGS.sleepStateBadge));
        } else {
            SETTINGS.sleepStateBadge = want != 0 ? 1 : 0;
            const bool saved = SETTINGS.saveToFile();
            printf("badge: %u saved=%d\n", static_cast<unsigned>(SETTINGS.sleepStateBadge), saved ? 1 : 0);
        }
    } else if (strcmp(command, "slpdiag") == 0) {
        Activity* cur = activityManager.currentActivityPtr();
        printf("slpdiag: activity=%s bg=%d wifiMode=%d usb=%d debugUsb=%d canStandby=%d sdPark=%d "
               "badge=%u lightStyle=%u internal=%u\n",
               cur != nullptr ? cur->debugName() : "(none)", background_work::active() ? 1 : 0,
               static_cast<int>(WiFi.getMode()), gpio.isUsbConnected() ? 1 : 0,
               HalPowerManager::debugUsbSleepAllowed() ? 1 : 0,
               powerManager.canStandbyLightSleep(gpio) ? 1 : 0,
               Storage.canParkForLightSleep() ? 1 : 0, static_cast<unsigned>(SETTINGS.sleepStateBadge),
               static_cast<unsigned>(SETTINGS.lightSleepStyle),
               static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    } else if (strcmp(command, "jjwxc") == 0) {
#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)
        activityManager.goToJjwxc();
        printf("jjwxc: opened\n");
#else
        printf("jjwxc: this build has no private book source\n");
#endif
    } else if (strcmp(command, "standby") == 0) {
        activityManager.goToStandby();
    } else if (strcmp(command, "sleepbadge") == 0) {
        // Render the light-sleep badge on the standby face without parking the
        // CPU, so its text and placement can be screenshotted over USB.
        const bool on = args != nullptr && atoi(args) != 0;
        standby_debug::showSleepBadge(on);
        Activity* current = activityManager.currentActivityPtr();
        if (on && (current == nullptr || strcmp(current->debugName(), "Standby") != 0)) {
            activityManager.goToStandby();
        } else {
            activityManager.requestUpdate();
        }
        printf("sleepbadge: %d setting=%u\n", on ? 1 : 0,
               static_cast<unsigned>(SETTINGS.sleepStateBadge));
    } else if (strcmp(command, "slpusb") == 0) {
        const bool on = args != nullptr && atoi(args) != 0;
        HalPowerManager::setDebugAllowUsbSleep(on);
        printf("slpusb: %d\n", on ? 1 : 0);
    } else if (strcmp(command, "slptest") == 0) {
        int seconds = args != nullptr ? atoi(args) : 5;
        if (seconds < 1) seconds = 1;
        if (seconds > 30) seconds = 30;
#if defined(CROSSPOINT_PDF_XIP_SD)
        if (pdf_module::isLoaded()) {
            printf("slptest: refused; the PDF engine is mapped in PSRAM (Standby also refuses)\n");
            return;
        }
#endif
        printf("slptest: light sleep %d s\n", seconds);
        struct SleepCtx {
            uint32_t seconds;
            volatile int reason;
            volatile unsigned failure;
            volatile int error;
            volatile int done;
        } sctx{static_cast<uint32_t>(seconds), -1, 0, 0, 0};
        auto sleeper = [](void* raw) {
            auto* c = static_cast<SleepCtx*>(raw);
            c->reason = static_cast<int>(powerManager.lightSleepFor(c->seconds));
            c->failure = HalPowerManager::lastLightSleepFailure();
            c->error = HalPowerManager::lastLightSleepError();
            c->done = 1;
            vTaskDelete(nullptr);
        };
        if (esp_ptr_external_ram(esp_cpu_get_sp())) {
            if (xTaskCreate(sleeper, "slptest", 12288, &sctx, 4, nullptr) != pdPASS) {
                printf("slptest: task create failed\n");
                return;
            }
            while (!sctx.done) vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            sleeper(&sctx);
        }
        printf("slptest: wake=%d failure=%u err=%d\n", sctx.reason, sctx.failure, sctx.error);
    } else if (strcmp(command, "my") == 0) {
        consoleMy(args);
    } else if (strcmp(command, "stats") == 0) {
        consoleStats();
    } else if (strcmp(command, "stab") == 0) {
        consoleTab(args);
    } else if (strcmp(command, "saction") == 0) {
        consoleAction(args);
    } else if (strcmp(command, "slist") == 0) {
        consoleListActions();
    } else if (strcmp(command, "wipe") == 0) {
        consoleWipe(args);
    } else if (strcmp(command, "ota") == 0) {
        consoleOta(args);
    } else if (strcmp(command, "resources") == 0) {
        activityManager.replaceActivityWith<PlusResourcesActivity>();
        printf("resources: page opened\n");
    } else if (strcmp(command, "pa") == 0) {
        const bool on = args != nullptr && atoi(args) != 0;
        printf("pa: %d -> %d\n", on ? 1 : 0, (int)epd_board_metalio_eink4_plus_pa_set(on));
    } else if (strcmp(command, "audio") == 0) {
        // Speaker path probe. `audio` prints the BH1098 state, `audio tone
        // [ms]` plays a 440 Hz test tone, `audio mic [frames]` reads the mic
        // and prints its RMS so a silent capture is easy to spot.
        char mode[16] = "state";
        int arg = 0;
        if (args != nullptr) (void)sscanf(args, "%15s %d", mode, &arg);
        if (strcmp(mode, "tone") == 0) {
            const int ms = arg > 0 ? (arg > 3000 ? 3000 : arg) : 500;
            if (!plus_audio_wait_ready(8000)) {
                printf("audio: path not ready\n");
            } else {
                const int rate = plus_audio_sample_rate();
                int16_t buf[256];
                int total = rate * ms / 1000;
                int done = 0;
                constexpr float kPi = 3.14159265f;
                float phase = 0.0f;
                plus_audio_set_volume(80);
                plus_audio_pa(true);
                while (done < total) {
                    int n = total - done;
                    if (n > (int)(sizeof(buf) / sizeof(buf[0]))) n = (int)(sizeof(buf) / sizeof(buf[0]));
                    for (int i = 0; i < n; ++i) {
                        buf[i] = (int16_t)(sinf(phase) * 9000.0f);
                        phase += 2.0f * kPi * 440.0f / (float)rate;
                        if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
                    }
                    if (plus_audio_write_pcm(buf, n) < 0) break;
                    done += n;
                }
                int16_t silence[160] = {0};
                plus_audio_write_pcm(silence, 160);
                plus_audio_pa(false);
                printf("audio: tone %d ms, %d frames written\n", ms, done);
            }
        } else if (strcmp(mode, "mic") == 0) {
            const int frames = arg > 0 ? (arg > 1024 ? 1024 : arg) : 320;
            int16_t* pcm = (int16_t*)malloc((size_t)frames * 2 * sizeof(int16_t));
            if (pcm == nullptr) {
                printf("audio: out of memory\n");
            } else {
                const int got = plus_audio_read_mic(pcm, frames);
                double sum = 0;
                for (int i = 0; i < got * 2; ++i) sum += (double)pcm[i] * pcm[i];
                const double rms = (got > 0) ? sqrt(sum / (got * 2)) : 0.0;
                printf("audio: mic frames=%d rms=%.0f\n", got, rms);
                free(pcm);
            }
        } else {
            printf("audio: state=%d ready=%d rate=%d\n", (int)plus_audio_state(),
                   plus_audio_mode_ready() ? 1 : 0, plus_audio_sample_rate());
        }
    } else if (strcmp(command, "cellprobe") == 0) {
        // The probe installs its own UART2 driver; while the 4G session owns the
        // modem that install fails and the "no bytes" answer says nothing about
        // the hardware, so never let that wipe a working configuration.
        if (plus_cellular_active()) {
            printf("cellprobe: skipped, the 4G session owns UART2 (use cellularforget on the next boot)\n");
            return;
        }
        const bool present = probeCellularOnce();
        // Persist the probe the same way the boot probe does: the settings row
        // and the 4G transport both gate on these two flags, so a bench probe
        // must be able to declare a module without hand-editing settings. The
        // settings store lives in NVS (internal flash); this console task's
        // stack is internal RAM, so the save is legal here.
        SETTINGS.cellularPresent = present ? 1 : 0;
        SETTINGS.cellularProbed = 1;
        SETTINGS.saveToFile();
        printf("cellprobe: present=%d (saved; reboot to switch the network type)\n", present ? 1 : 0);
    } else if (strcmp(command, "cellularforget") == 0) {
        // Drop the "already probed" flag so the next boot probes again (the boot
        // probe only runs while cellularProbed is 0). Presence is left as-is
        // until that boot rewrites it, so the network-type row does not vanish
        // in the current session.
        SETTINGS.cellularProbed = 0;
        SETTINGS.saveToFile();
        printf("cellularforget: probed=0, next boot will probe again\n");
    } else if (strcmp(command, "cellular") == 0) {
        printf("cellular: present=%u probed=%u\n", (unsigned)SETTINGS.cellularPresent,
               (unsigned)SETTINGS.cellularProbed);
        if (SETTINGS.networkType == CrossPointSettings::NETWORK_CELLULAR && plus_cellular_active()) {
            const int csq = plus_cellular_csq();
            if (csq >= 0 && csq <= 31) {
                printf("cellular: csq=%d (%d dBm) registered=%d\n", csq, -113 + 2 * csq,
                       plus_cellular_registered() ? 1 : 0);
            } else {
                printf("cellular: csq=%d (unknown) registered=%d\n", csq, plus_cellular_registered() ? 1 : 0);
            }
        }
    } else if (strcmp(command, "haptic") == 0) {
        const int ms = args != nullptr ? atoi(args) : 120;
        gpio.pulseHaptic((uint16_t)(ms > 0 ? (ms > 2000 ? 2000 : ms) : 120));
        printf("haptic: %d ms\n", ms > 0 ? ms : 120);
    } else if (strcmp(command, "volume") == 0) {
        if (args != nullptr && *args != '\0') {
            setSpeakerVolume(atoi(args));
        }
        printf("volume=%u\n", (unsigned)SETTINGS.audioVolume);
    } else if (strcmp(command, "pwrhold") == 0) {
        // Bench: synthesise a physical power-key hold of N ms (default 3000)
        // through the production gesture path, so the shutdown-card / standby
        // split can be verified without a finger.
        int ms = args != nullptr ? atoi(args) : 3000;
        if (ms < 200) ms = 200;
        if (ms > 8000) ms = 8000;
        s_benchPowerHoldUntil = millis() + (unsigned long)ms;
        printf("pwrhold: simulating a %d ms power-key hold\n", ms);
    } else if (strcmp(command, "pwrdemo") == 0) {
        activityManager.pushActivityWith<PowerOffActivity>(renderer, mappedInputManager);
        printf("pwrdemo: shutdown confirmation pushed\n");
    } else if (strcmp(command, "pwroff") == 0) {
        printf("pwroff: vendor P1.3 pulse train\n");
        fflush(stdout);
        const esp_err_t err = epd_board_metalio_eink4_plus_power_off_pulse();
        printf("pwroff: pulse returned %d (device should lose power)\n", (int)err);
    } else if (strcmp(command, "flpresent") == 0) {
        // Frontlight hardware presence (no electrical readback on this board).
        SETTINGS.frontlightPresent = (args != nullptr && atoi(args) != 0) ? 1 : 0;
        SETTINGS.frontlightProbed = 0;  // allow a fresh automatic probe
        SETTINGS.saveToFile();
        Frontlight.setPresentOverride(SETTINGS.frontlightPresent != 0);
        if (!SETTINGS.frontlightPresent) Frontlight.setOn(false);
        printf("frontlight present=%u (present()=%d)\n", (unsigned)SETTINGS.frontlightPresent,
               Frontlight.present() ? 1 : 0);
    } else if (strcmp(command, "pageturn") == 0) {
        if (args != nullptr && *args != '\0') {
            SETTINGS.pageTurnEffect = static_cast<uint8_t>(atoi(args) != 0 ? 1 : 0);
            SETTINGS.saveToFile();
        }
        printf("pageTurnEffect=%u (0 direct, 1 spatial wipe)\n", (unsigned)SETTINGS.pageTurnEffect);
    } else if (strcmp(command, "sync") == 0) {
        s_benchAction = 4;
        printf("sync: starting cloud progress sync\n");
    } else if (strcmp(command, "swipe") == 0) {
        consoleSwipe(args);
    } else if (strcmp(command, "touchdown") == 0) {
        consoleTouchDown(args);
    } else if (strcmp(command, "touchup") == 0) {
        consoleTouchUp();
    } else if (strcmp(command, "info") == 0) {
        consoleInfo();
    } else if (strcmp(command, "dmaheap") == 0) {
        printf("dma free=%u largest=%u | internal free=%u largest=%u | psram free=%u largest=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        const size_t probes[] = {512, 1024, 2048, 4096, 8192};
        for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
            void *p = heap_caps_malloc(probes[i], MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
            printf("  probe %u -> %s\n", (unsigned)probes[i], p != nullptr ? "ok" : "FAIL");
            if (p != nullptr) heap_caps_free(p);
        }
        sdmmc_card_t *card = cpyos_sd_card();
        if (card != nullptr) {
            printf("  sd sector=%u chunk=%u bounce=%p\n", (unsigned)card->csd.sector_size,
                   (unsigned)card->host.unaligned_multi_block_rw_max_chunk_size,
                   card->host.dma_aligned_buffer);
        }
    } else if (strcmp(command, "batt") == 0) {
        consoleBattery();
    } else if (strcmp(command, "battlog") == 0) {
        consoleBattLog(args);
    } else if (strcmp(command, "rtc") == 0) {
        consoleRtc();
    } else if (strcmp(command, "sd") == 0) {
        consoleSd();
    } else if (strcmp(command, "open") == 0) {
        // `open <path>` parks a persistent reader target and reboots, so the
        // next boot (including the one this console open causes) lands in the
        // reader on that file. `open clear` drops it again.
        // Deterministic format check: park the path in the one-shot silent
        // target and reboot; the boot route opens the reader on it, so a test
        // never depends on tapping the right library tile.
        if (args == nullptr || *args == '\0') {
            printf("usage: open </path/on/card>\n");
        } else if (strcmp(args, "clear") == 0) {
            Storage.remove(kPersistOpenPath);
            printf("open: persistent target cleared\n");
        } else if (!Storage.exists(args)) {
            printf("open: no such file: %s\n", args);
        } else {
            Storage.writeFile(kPersistOpenPath, String(kPersistOpenBoots) + " " + args);
            printf("open: next %d boots open %s ('open clear' stops it)\n", kPersistOpenBoots, args);
            fflush(stdout);
            esp_restart();
        }
    } else if (strcmp(command, "hash") == 0) {
        // FNV-1a over the whole file, so an upload can be compared byte for
        // byte with the host copy: the console putfile path can drop a chunk
        // without changing the reported size.
        if (args == nullptr || *args == '\0' || !Storage.exists(args)) {
            printf("hash: missing %s\n", args ? args : "-");
        } else {
            HalFile f = Storage.open(args, O_RDONLY);
            uint32_t h = 2166136261u;
            unsigned long total = 0;
            uint8_t buf[1024];
            while (f) {
                const int n = f.read(buf, sizeof(buf));
                if (n <= 0) break;
                for (int i = 0; i < n; ++i) {
                    h ^= buf[i];
                    h *= 16777619u;
                }
                total += static_cast<unsigned long>(n);
            }
            f.close();
            printf("hash: %s size=%lu fnv1a=%08lx\n", args, total, static_cast<unsigned long>(h));
        }
    } else if (strcmp(command, "imgtest") == 0) {
        // Isolate the image pipeline: open the source, open the BMP target and
        // run the JPEG converter exactly as ImageViewer/Cbz do, then report each
        // step. Keeps format failures from being blamed on the reader UI.
        if (args == nullptr || *args == '\0') {
            printf("usage: imgtest </path>\n");
        } else {
            // Three configurations, so a failure names the path that broke:
            // scaled Gray2 (ImageViewer), scaled Mono1 (Cbz), and native size
            // Gray2 (no scaling at all).
            struct Case {
                const char* name;
                bool mono;
                bool native;
            };
            const Case cases[] = {{"gray2-scaled", false, false}, {"mono1-scaled", true, false}, {"gray2-native", false, true}};
            for (const auto& c : cases) {
                HalFile in;
                HalFile out;
                const bool okIn = Storage.openFileForRead("IMAGE", args, in);
                const bool okOut = Storage.openFileForWrite("IMAGE", "/.crosspoint/imgprobe.bmp", out);
                bool conv = false;
                if (okIn && okOut) {
                    const int w = c.native ? 0 : renderer.getScreenWidth();
                    const int h = c.native ? 0 : renderer.getScreenHeight();
                    conv = c.mono ? JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(in, out, w, h, false)
                                  : JpegToBmpConverter::jpegFileToBmpStreamWithSize(in, out, w, h, false);
                    out.flush();
                }
                out.close();
                in.close();
                HalFile probe = Storage.open("/.crosspoint/imgprobe.bmp", O_RDONLY);
                const unsigned long size = probe ? static_cast<unsigned long>(probe.size()) : 0UL;
                probe.close();
                printf("imgtest %s %s: read=%d write=%d conv=%d bmp=%lu\n", args, c.name, (int)okIn, (int)okOut,
                       (int)conv, size);
            }
        }
    } else if (strcmp(command, "stat") == 0) {
        if (args == nullptr || *args == '\0') {
            printf("usage: stat </path>\n");
        } else if (!Storage.exists(args)) {
            printf("stat: missing %s\n", args);
        } else {
            HalFile f = Storage.open(args, O_RDONLY);
            printf("stat: %s size=%lu\n", args, f ? static_cast<unsigned long>(f.size()) : 0UL);
        }
    } else if (strcmp(command, "retrysd") == 0) {
        const bool ok = Storage.begin();
        printf("retrysd: %s (mounted=%d)\n", ok ? "ok" : "failed", cpyos_sd_is_mounted() ? 1 : 0);
    } else if (strcmp(command, "imu") == 0) {
        printf("imu: %s (motion page turn %u)\n", halTiltSensor.isAvailable() ? "available" : "not detected",
               (unsigned)SETTINGS.motionPageTurn);
    } else if (strcmp(command, "touch") == 0) {
        printf("touch: present=%d held=%lums power=%d\n", gpio.hasTouch() ? 1 : 0, gpio.lastTouchHeldMs(),
               gpio.isPressed(HalGPIO::BTN_POWER) ? 1 : 0);
    } else if (strcmp(command, "keys") == 0) {
        consoleKeys();
    } else if (strcmp(command, "slpinfo") == 0) {
        consoleSlpInfo();
    } else if (strcmp(command, "pinyin") == 0) {
        if (esp_ptr_external_ram(esp_cpu_get_sp())) {
            // The pinyin tables live in the data partition; that flash read is
            // only legal on an internal stack.
            struct Ctx {
                volatile int ok;
                volatile unsigned syllables;
                volatile int done;
            } ctx{0, 0, 0};
            auto runner = [](void* raw) {
                auto* c = static_cast<Ctx*>(raw);
                auto& db = pinyin::PinyinDb::shared();
                c->ok = db.ensureLoaded() ? 1 : 0;
                c->syllables = c->ok ? static_cast<unsigned>(db.tables().syllableCount()) : 0u;
                c->done = 1;
                vTaskDelete(nullptr);
            };
            if (xTaskCreate(runner, "pinyin", 10240, &ctx, 4, nullptr) != pdPASS) {
                printf("pinyin: task create failed\n");
            } else {
                while (!ctx.done) vTaskDelay(pdMS_TO_TICKS(20));
                printf("pinyin: loaded=%d syllables=%u\n", ctx.ok, ctx.syllables);
            }
        } else {
            consolePinyin();
        }
    } else if (strcmp(command, "hyphen") == 0) {
        consoleHyphen();
    } else if (strcmp(command, "i2c") == 0) {
        consoleI2C();
    } else if (strcmp(command, "sdwrite") == 0) {
        consoleSdWrite();
    } else if (strcmp(command, "sdstress") == 0) {
        consoleSdStress(args);
    } else if (strcmp(command, "sdremount") == 0) {
        consoleSdRemount();
    } else if (strcmp(command, "sdpark") == 0) {
        consoleSdPark();
    } else if (strcmp(command, "sdrename") == 0) {
        consoleSdRename(args);
    } else if (strcmp(command, "sdhealthfail") == 0) {
        consoleSdHealthFail();
    } else if (strcmp(command, "sdhealth") == 0) {
        consoleSdHealth(args);
    } else if (strcmp(command, "sdformat") == 0) {
        consoleSdFormat();
    } else if (strcmp(command, "dict") == 0) {
        consoleDict(args);
    } else if (strcmp(command, "haptic") == 0) {
        consoleHaptic(args);
    } else if (strcmp(command, "panel") == 0) {
        consolePanel(args);
    } else if (strcmp(command, "window") == 0) {
        consoleWindow();
    } else if (strcmp(command, "screenshot") == 0) {
        consoleScreenshot();
    } else if (strcmp(command, "screenshotgray") == 0) {
        consoleScreenshotGray();
    } else if (strcmp(command, "fbink") == 0) {
        consoleFbInk(args);
    } else if (strcmp(command, "fbclear") == 0) {
        consoleFbClear();
    } else if (strcmp(command, "browser") == 0) {
        consoleBrowser(args);
    } else if (strcmp(command, "plugins") == 0) {
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
        activityManager.goToNativePlugins();
        printf("plugins: opened\n");
#else
        printf("plugins: runtime not built\n");
#endif
    } else if (strcmp(command, "pluginlaunch") == 0) {
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
        char id[40] = {};
        if (args == nullptr || sscanf(args, "%39s", id) != 1) {
            printf("usage: pluginlaunch <id>\n");
        } else {
            auto plugins = native_plugins::scan();
            for (const auto& info : plugins) {
                if (info.id == id && info.error.empty()) {
                    if (activityManager.replaceActivityWith<PluginActivity>(info)) {
                        printf("pluginlaunch: %s\n", id);
                    } else {
                        printf("pluginlaunch: OOM\n");
                    }
                    id[0] = '\0';
                    break;
                }
            }
            if (id[0] != '\0') printf("pluginlaunch: not found or invalid: %s\n", id);
        }
#else
        printf("pluginlaunch: runtime not built\n");
#endif
#if defined(CROSSPOINT_PDF_XIP)
    } else if (strcmp(command, "pdfengineurl") == 0) {
        // Bench: point the engine updater at a local HTTPS server so the full
        // manifest -> download -> install path can be exercised without
        // publishing a release.  An empty argument clears the override.
        pdf_engine::setManifestUrlOverride(args != nullptr && *args != '\0' ? args : nullptr);
        printf("pdfengineurl: %s\n", pdf_engine::isConfigured() ? "set" : "cleared");
    } else if (strcmp(command, "pdfengineupdate") == 0) {
        bool started = false;
        if (esp_ptr_external_ram(esp_cpu_get_sp())) {
            struct Ctx {
                volatile int started;
            } ctx{-1};
            auto runner = [](void* raw) {
                auto* c = static_cast<Ctx*>(raw);
                c->started = pdf_engine::startUpdate() ? 1 : 0;
                vTaskDelete(nullptr);
            };
            if (xTaskCreate(runner, "pdfengstart", 10240, &ctx, 4, nullptr) != pdPASS) {
                printf("pdfengineupdate: task create failed\n");
            } else {
                while (ctx.started < 0) vTaskDelay(pdMS_TO_TICKS(20));
                started = ctx.started == 1;
            }
        } else {
            started = pdf_engine::startUpdate();
        }
        if (!started) {
            printf("pdfengineupdate: busy or not configured\n");
        } else if (pdf_engine::updateInProgress()) {
            while (pdf_engine::updateInProgress()) vTaskDelay(pdMS_TO_TICKS(200));
            const auto state = pdf_engine::pollUpdate();
            const auto failure = pdf_engine::result().failure;
            printf("pdfengineupdate: state=%d failure=%d\n", static_cast<int>(state), static_cast<int>(failure));
            if (state == pdf_engine::State::Done) {
                int install = -1;
                if (esp_ptr_external_ram(esp_cpu_get_sp())) {
                    struct InstallCtx {
                        volatile int status;
                    } ictx{-1};
                    auto installRunner = [](void* raw) {
                        auto* c = static_cast<InstallCtx*>(raw);
                        c->status = static_cast<int>(pdf_engine::installFromTemp());
                        vTaskDelete(nullptr);
                    };
                    if (xTaskCreate(installRunner, "pdfenginst", 12288, &ictx, 4, nullptr) == pdPASS) {
                        while (ictx.status < 0) vTaskDelay(pdMS_TO_TICKS(20));
                        install = ictx.status;
                    }
                } else {
                    install = static_cast<int>(pdf_engine::installFromTemp());
                }
                printf("pdfengineupdate: install=%d\n", install);
            }
        }
    } else if (strcmp(command, "pdfassets") == 0) {
        printf("pdfassets: %s\n", ensurePdfAssets() ? "ok" : "fail");
    } else if (strcmp(command, "pdfengineinstall") == 0) {
        if (args == nullptr || *args == '\0') {
            printf("usage: pdfengineinstall </path/to/pdfmod.bin>\n");
        } else if (esp_ptr_external_ram(esp_cpu_get_sp())) {
            // installFromSdFile reads the running app's SHA from internal flash;
            // doing that on the PSRAM console stack trips the cache-disable
            // assert. Run it on a short internal-stack task and wait for it.
            struct Ctx {
                const char* path;
                volatile int status;
            } ctx{args, -1};
            auto runner = [](void* raw) {
                auto* c = static_cast<Ctx*>(raw);
                c->status = static_cast<int>(pdf_module::installFromSdFile(c->path));
                vTaskDelete(nullptr);
            };
            TaskHandle_t task = nullptr;
            if (xTaskCreate(runner, "pdfinstall", 12288, &ctx, 4, &task) != pdPASS) {
                printf("pdfengineinstall: task create failed\n");
            } else {
                while (ctx.status < 0) vTaskDelay(pdMS_TO_TICKS(20));
                printf("pdfengineinstall: %d\n", ctx.status);
            }
        } else {
            printf("pdfengineinstall: %d\n", static_cast<int>(pdf_module::installFromSdFile(args)));
        }
#endif
    } else if (strcmp(command, "httpprobe") == 0) {
        consoleHttpProbe(args);
    } else if (strcmp(command, "tlsprobe") == 0) {
        consoleTlsProbe(args);
    } else if (strcmp(command, "putfile") == 0) {
        consolePutFile(args);
    } else if (strcmp(command, "putfilefast") == 0) {
        consolePutFileFast(args);
    } else if (strcmp(command, "crypto") == 0) {
        printf("crypto: %s\n", freeink::content::portContentCryptoSelfTest() ? "ok" : "FAILED");
    } else if (strcmp(command, "cryptotest") == 0) {
        consoleCryptoTest();
    } else if (strcmp(command, "tracetext") == 0) {
        const bool on = args != nullptr && *args == '1';
        gfxSetTextTrace(on);
        printf("tracetext: %s\n", on ? "on" : "off");
    } else if (strcmp(command, "textaa") == 0) {
        // Bisect CJK label artifacts between the 16-level and B/W text paths.
        const int want = args != nullptr ? atoi(args) : -1;
        if (want < 0) {
            printf("textaa: %u (0 = B/W, 1 = 16-level)\n", (unsigned)SETTINGS.textAntiAliasing);
        } else {
            SETTINGS.textAntiAliasing = want != 0 ? 1 : 0;
            SETTINGS.saveToFile();
            activityManager.requestUpdate(true);
            printf("textaa: %u\n", (unsigned)SETTINGS.textAntiAliasing);
        }
    } else if (strcmp(command, "ls") == 0) {
        consoleList(args);
    } else if (strcmp(command, "cleanrect") == 0) {
        int x = 0, y = 0, w = 0, h = 0;
        if (args == nullptr || sscanf(args, "%d %d %d %d", &x, &y, &w, &h) != 4) {
            printf("usage: cleanrect <x> <y> <w> <h>\n");
        } else {
            const bool ok = display.cleanWindow(static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                                                static_cast<uint16_t>(w), static_cast<uint16_t>(h));
            printf("cleanrect: %d,%d %dx%d ok=%d\n", x, y, w, h, ok ? 1 : 0);
        }
    } else if (strcmp(command, "clean") == 0) {
        // Force one non-differential GC16 sweep of the whole panel. This is the
        // only thing that removes residue left by skipped/partial drives.
        display.refreshDisplay(HalDisplay::FULL_REFRESH, false);
        printf("clean: full GC16 sweep issued\n");
    } else if (strcmp(command, "tail") == 0) {
        consoleTail(args);
    } else if (strcmp(command, "cat") == 0) {
        consoleCat(args);
    } else if (strcmp(command, "rm") == 0) {
        if (args == nullptr || *args == '\0') {
            printf("usage: rm </path>\n");
        } else {
            const bool ok = Storage.remove(args);
            printf("rm: %s %s\n", args, ok ? "ok" : "FAILED");
        }
    } else if (strcmp(command, "mkdir") == 0) {
        if (args == nullptr || *args == '\0') {
            printf("usage: mkdir </path>\n");
        } else {
            errno = 0;
            const bool ok = Storage.mkdir(args);
            const int e1 = errno;
            errno = 0;
            const int raw = ::mkdir("/sdcard/__rawprobe", 0777);
            const int e2 = errno;
            struct stat st{};
            errno = 0;
            const int sr = ::stat("/sdcard", &st);
            const int e4 = errno;
            errno = 0;
            const int rd = ::open("/sdcard/plus-test.txt", O_RDONLY);
            const int e5 = errno;
            if (rd >= 0) ::close(rd);
            errno = 0;
            const int wr = ::open("/sdcard/probe-w.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            const int e6 = errno;
            if (wr >= 0) ::close(wr);
            errno = 0;
            const int mk2 = ::mkdir("/sdcard/probe-m", 0777);
            const int e7 = errno;
            printf("mkdir %s ok=%d errno=%d raw=%d/%d stat=%d/%d read=%d/%d write=%d/%d mk=%d/%d\n", args,
                   (int)ok, e1, raw, e2, sr, e4, rd, e5, wr, e6, mk2, e7);
        }
    } else if (strcmp(command, "cpu") == 0) {
        printf("cpu=%d MHz (PM %s)\n", esp_clk_cpu_freq() / 1000000,
#if defined(CONFIG_PM_ENABLE)
               "on");

#else
               "off");
#endif
    } else if (strcmp(command, "areatest") == 0) {
        int n = args != nullptr ? atoi(args) : 3;
        if (n <= 0 || n > 20) n = 3;
        {
            RenderLock lock;
            renderer.clearScreen();
            for (int i = 0; i < n; ++i) {
                const int lx = 60 + i * 12;
                const int ly = 300;
                renderer.fillRect(lx, ly, 48, 48, true);
                // Portrait logical rect -> physical panel rect (xL,yL)->(yL,683-xL).
                renderer.displayWindow(ly, 683 - (lx + 47), 48, 48);
                renderer.fillRect(lx, ly, 48, 48, false);
                vTaskDelay(pdMS_TO_TICKS(80));
            }
        }
        printf("areatest %d done\n", n);
    } else if (strcmp(command, "logtest") == 0) {
        ESP_LOGI("TEST", "sid=ABCDEF123456 token=xyz loginName=a@b.c loginPassword=hunter2 password=123456 keep=this");
        printf("logtest: line emitted; check /.crosspoint/logs/boot.log after ~4 s\n");
    } else if (strcmp(command, "vtext") == 0) {
        if (args != nullptr && *args != '\0') {
            SETTINGS.verticalText = static_cast<uint8_t>(atoi(args) != 0 ? 1 : 0);
            SETTINGS.saveToFile();
        }
        printf("verticalText=%u\n", (unsigned)SETTINGS.verticalText);
    } else if (strcmp(command, "orient") == 0) {
        if (args != nullptr && *args != '\0') {
            SETTINGS.orientation = static_cast<uint8_t>(std::clamp(atoi(args), 0, 3));
            SETTINGS.saveToFile();
        }
        printf("orientation=%u (0 portrait, 1 landscape CW, 2 inverted, 3 landscape CCW)\n",
               (unsigned)SETTINGS.orientation);
    } else if (strcmp(command, "fastturn") == 0) {
        if (args != nullptr && *args != '\0') {
            SETTINGS.fastPageTurn = atoi(args) != 0 ? 1 : 0;
            SETTINGS.saveToFile();
        }
        printf("fastPageTurn=%u\n", (unsigned)SETTINGS.fastPageTurn);
    } else if (strcmp(command, "embfont") == 0) {
        if (args != nullptr && *args != '\0') {
            const bool on = atoi(args) != 0;
            s_embeddedFontEnabled = on;
            if (on) {
                Storage.writeFile("/.crosspoint/embfont", String("1"));
            } else {
                Storage.remove("/.crosspoint/embfont");
            }
        }
        printf("embfont=%d (0 skip, 1 allow; persisted for bench reboots)\n",
               (s_embeddedFontEnabled || Storage.exists("/.crosspoint/embfont")) ? 1 : 0);
    } else if (strcmp(command, "part") == 0) {
        const esp_partition_t* running = esp_ota_get_running_partition();
        const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
        printf("running=%s@0x%08lx next=%s@0x%08lx\n", running ? running->label : "?",
               running ? (unsigned long)running->address : 0UL, next ? next->label : "?",
               next ? (unsigned long)next->address : 0UL);
    } else if (strcmp(command, "boothang") == 0) {
        plus_boot_hang_request();
        printf("boothang: rebooting into a 60 s boot stall (watchdog should switch slots)\n");
        fflush(stdout);
        esp_restart();
    } else if (strcmp(command, "wave") == 0) {
        if (args != nullptr && *args != '\0') plus_epd_set_transient_wave(atoi(args));
        printf("wave transient=%d (0 GL16, 1 DU20, 2 DU14, 3 GL36)\n", plus_epd_transient_wave());
    } else if (strcmp(command, "wifi") == 0) {
        consoleWifi();
    } else if (strcmp(command, "wificonnect_saved") == 0) {
        consoleWifiConnectSaved(args);
    } else if (strcmp(command, "wificonnect") == 0) {
        consoleWifiConnect(args);
    } else if (strcmp(command, "wifisaved") == 0) {
        consoleWifiSaved();
    } else if (strcmp(command, "light") == 0) {
        consoleLight(args);
    } else if (strcmp(command, "ap") == 0) {
        consoleAp(args);
    } else if (strcmp(command, "apoff") == 0) {
        consoleApOff();
    } else if (strcmp(command, "radiooff") == 0) {
        consoleRadioOff();
    } else if (strcmp(command, "web") == 0) {
        consoleWeb(args);
    } else if (strcmp(command, "weboff") == 0) {
        consoleWebOff();
    } else if (strcmp(command, "usbmsc") == 0) {
        consoleUsbMsc(args);
    } else if (strcmp(command, "touchraw") == 0) {
        consoleTouchRaw();
    } else if (strcmp(command, "rtcset") == 0) {
        consoleRtcSet(args);
    } else if (strcmp(command, "selftest") == 0) {
        consoleSelfTest();
    } else if (strcmp(command, "reboot") == 0) {
        // Unattended bring-up: when a subsystem (typically SDMMC) wedges there
        // is no shell, no reset key, and replugging needs a human. A plain
        // restart re-initialises every driver and keeps the SD card mounted.
        printf("reboot: restarting\n");
        fflush(stdout);
        esp_restart();
    } else if (strcmp(command, "btmap") == 0) {
        bleinput::Action action = bleinput::Action::Count;
        const bool back = bleinput::lookup(SETTINGS.bleKeyMap, 0, 6, action) &&
                          action == bleinput::Action::PageBack;
        const bool forward = bleinput::lookup(SETTINGS.bleKeyMap, 0, 7, action) &&
                             action == bleinput::Action::PageForward;
        printf("btmap: default-left-back=%d default-right-forward=%d\n", back ? 1 : 0,
               forward ? 1 : 0);
    } else if (strcmp(command, "bt") == 0) {
        consoleBt(args);
    } else {
        printf("unknown command: %s\n", command);
        consoleHelp();
    }
}

// Dedicated console reader. The select()-in-the-loop variant never saw input
// on the USB-Serial/JTAG console, so this task uses a plain blocking read on
// stdin (the IDF console VFS blocks until a byte arrives) and dispatches from
// its own stack. That also keeps flashing commands off the render path.
void consoleTask(void* arg) {
    (void)arg;
    for (;;) {
        char c = 0;
        const int n = read(STDIN_FILENO, &c, 1);
        if (n != 1) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n') {
            g_consoleLine[g_consoleLen] = '\0';
            if (g_consoleLen > 0) consoleExec(g_consoleLine);
            g_consoleLen = 0;
            continue;
        }
        if (c == 0x7f || c == 0x08) {
            if (g_consoleLen > 0) --g_consoleLen;
            continue;
        }
        if (g_consoleLen + 1 < sizeof(g_consoleLine)) {
            g_consoleLine[g_consoleLen++] = c;
        }
    }
}

// Keep the BLE HID host alive for pages that support it (the reader), matching
// the reference main loop. The user enables it in Settings -> Bluetooth.
void updateBluetoothLifecycle() {
#if FREEINK_CAP_BLE_HID_HOST
    static unsigned long nextStartAttemptAt = 0;
    const auto wanted = [] {
        return SETTINGS.bluetoothEnabled && activityManager.keepsBluetoothAlive() &&
               !activityManager.requiresExclusiveStorageLoop() &&
               static_cast<int>(WiFi.getMode()) == static_cast<int>(WIFI_MODE_NULL);
    };
    if (!wanted()) {
        bleinput::stop();
        return;
    }
    if (bleinput::isRunning() || activityManager.deferBluetoothStart() ||
        millis() < nextStartAttemptAt) {
        return;
    }
    // Only readers use the automatic reader-memory gate and retry loop.
    if (!activityManager.isReaderActivity()) return;
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return;
    if (!wanted() || activityManager.deferBluetoothStart() || !activityManager.isReaderActivity() ||
        bleinput::isRunning()) {
        return;
    }
    const auto result = bleinput::ensureStarted(renderer, bleinput::StartContext::Reader);
    if (result == bleinput::StartResult::LowMemory || result == bleinput::StartResult::Failed) {
        nextStartAttemptAt = millis() + 2000;
    }
#endif
}

}  // namespace

// Starts the blocking stdin reader; defined out of the anonymous namespace so
// cpyos_shell_demo() can see it.
void plus_console_start() {
    // Bench commands run whole subsystems inline on this task: `stat`/`hash`
    // walk the FAT layer, `imgtest` decodes JPEG and writes a BMP, `open`
    // builds an EPUB index, and `audio tone` enters the I2S driver. 4 KiB
    // overflowed the canary on the very first `stat`, and 12 KiB overflowed
    // again on the first I2S initialisation (panic "Stack protection fault" in
    // plus-console); a panic in the middle of a FAT write is what leaves the
    // card needing a power cycle. Nothing here is product code, so give it a
    // comfortable stack.
    // The merged UI fragments the internal heap enough that a 20 KB stack can
    // fail even with ~78 KB free (largest block), and a failed create used to be
    // silent: heartbeats kept flowing while every command was ignored. Prefer a
    // PSRAM stack (the depth unit is StackType_t words), then an internal stack.
    // The flash-writing bench commands must not run on that stack; they check.
    if (xTaskCreatePinnedToCoreWithCaps(consoleTask, "plus-console",
                                       20480 / sizeof(StackType_t), nullptr, 2, nullptr, 0,
                                       MALLOC_CAP_SPIRAM) != pdPASS) {
      const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (xTaskCreate(consoleTask, "plus-console", 20480, nullptr, 2, nullptr) != pdPASS &&
          xTaskCreate(consoleTask, "plus-console", 12288, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE("PLUS", "console task create failed (internal largest=%u); serial commands are unavailable",
                 static_cast<unsigned>(largest));
        return;
      }
      ESP_LOGW("PLUS", "console on internal stack (PSRAM create failed, largest=%u)",
               static_cast<unsigned>(largest));
    }
}

// ---------------------------------------------------------------------------
// Side volume keys (TCA P1.0 / P1.1).
//
// Vendor behaviour (metalio-vendor: display/lv_adapter_epdiy.cc, OnVolUp/Down):
// inside the reader the keys turn pages; everywhere else a click changes the
// volume by 10 % and a long press jumps to 100 % / 0 %, with a short on-screen
// notice. The reader keeps the old BTN_UP/BTN_DOWN mapping, so the shell flips
// HalGPIO's side-key mode per frame.
// ---------------------------------------------------------------------------
namespace {
constexpr unsigned long kVolumeLongPressMs = 600;
// Long enough to tap/drag the HUD after a volume key summoned it.
constexpr unsigned long kVolumeOverlayMs = 3500;
unsigned long s_volumeToastUntilMs = 0;
bool s_volumeToastVisible = false;
bool s_volumeDragging = false;
int s_volumeShown = -1;
Rect s_volumeHitRect{};
Rect s_volumeBarRect{};

// Right-edge vertical HUD (mobile-style). Drawn from primitives: the generated
// 48 px icon headers trip the IDF build's -Werror=comment.
void paintVolumeOverlay(int volume) {
  RenderLock lock;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  constexpr int kCardW = 112;
  constexpr int kCardH = 520;
  const int x = screenW - 16 - kCardW;
  const int y = (screenH - kCardH) / 2;
  GUI.drawModalCardFrame(renderer, Rect{x, y, kCardW, kCardH});

  const int iconX = x + (kCardW - 40) / 2;
  const int iconY = y + 22;
  renderer.fillRect(iconX, iconY + 12, 9, 16, true);
  {
    const int px[4] = {iconX + 9, iconX + 24, iconX + 24, iconX + 9};
    const int py[4] = {iconY + 20, iconY + 6, iconY + 34, iconY + 20};
    renderer.fillPolygon(px, py, 4, true);
  }
  renderer.drawLine(iconX + 33, iconY + 7, iconX + 38, iconY + 14, 2, true);
  renderer.drawLine(iconX + 38, iconY + 14, iconX + 38, iconY + 26, 2, true);
  renderer.drawLine(iconX + 38, iconY + 26, iconX + 33, iconY + 33, 2, true);

  constexpr int kSegW = 48;
  constexpr int kSegH = 34;
  constexpr int kGap = 8;
  const int barX = x + (kCardW - kSegW) / 2;
  const int barTop = iconY + 40 + 18;
  const int lit = (volume + 9) / 10;
  for (int i = 0; i < 10; ++i) {
    const int sy = barTop + (9 - i) * (kSegH + kGap);
    if (i < lit) {
      renderer.fillRoundedRect(barX, sy, kSegW, kSegH, 5, Color::Black);
    } else {
      renderer.drawRoundedRect(barX, sy, kSegW, kSegH, 2, 5, true);
    }
  }
  s_volumeBarRect = Rect{barX, barTop, kSegW, 10 * kSegH + 9 * kGap};
  s_volumeHitRect = Rect{x - 12, y, kCardW + 24, kCardH};
  s_volumeShown = volume;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
  s_volumeToastUntilMs = millis() + kVolumeOverlayMs;
  s_volumeToastVisible = true;
}

void setSpeakerVolume(int volume, bool persist) {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  if (SETTINGS.audioVolume != volume) {
    SETTINGS.audioVolume = static_cast<uint8_t>(volume);
    plus_audio_set_volume(volume);
    if (persist) SETTINGS.saveToFile();
    ESP_LOGI("PLUS", "speaker volume=%d", volume);
  }
  if (s_volumeToastVisible && s_volumeShown == volume) {
    s_volumeToastUntilMs = millis() + kVolumeOverlayMs;
    return;
  }
  paintVolumeOverlay(volume);
}

// Tap or drag on the HUD. Runs before the activity loop and kills this
// contact's tap/swipe so a drag on the slider cannot also open a book.
void handleVolumeOverlayTouch() {
  if (!s_volumeToastVisible) return;
  int x = 0;
  int y = 0;
  const bool held = mappedInputManager.isScreenTouchHeld(x, y);
  if (!s_volumeDragging) {
    if (!held || x < s_volumeHitRect.x || x >= s_volumeHitRect.x + s_volumeHitRect.width ||
        y < s_volumeHitRect.y || y >= s_volumeHitRect.y + s_volumeHitRect.height) {
      return;
    }
    s_volumeDragging = true;
    gpio.suppressTapForCurrentContact();
  }
  if (held) {
    const int top = s_volumeBarRect.y;
    const int height = s_volumeBarRect.height;
    int v = 100;
    if (height > 0) v = 100 - (std::clamp(y - top, 0, height) * 100) / height;
    v = ((v + 2) / 5) * 5;  // 5 % steps, same granularity as the settings row
    setSpeakerVolume(v, /*persist=*/false);
  }
  if (mappedInputManager.wasScreenTouchReleased()) {
    s_volumeDragging = false;
    SETTINGS.saveToFile();
  }
}

void handleSideVolumeKeys() {
  static bool upHeldPrev = false;
  static bool downHeldPrev = false;
  static unsigned long upSince = 0;
  static unsigned long downSince = 0;
  static bool upLongFired = false;
  static bool downLongFired = false;
  const unsigned long now = millis();

  const bool upHeld = gpio.isVolumeUpHeld();
  const bool downHeld = gpio.isVolumeDownHeld();
  if (upHeld && !upHeldPrev) {
    upSince = now;
    upLongFired = false;
  }
  if (downHeld && !downHeldPrev) {
    downSince = now;
    downLongFired = false;
  }
  if (upHeld && !upLongFired && now - upSince >= kVolumeLongPressMs) {
    upLongFired = true;
    setSpeakerVolume(100, true);
  }
  if (downHeld && !downLongFired && now - downSince >= kVolumeLongPressMs) {
    downLongFired = true;
    setSpeakerVolume(0, true);
  }
  // Clicks are decided on release so a long press does not also add a step.
  if (gpio.wasVolumeUpReleased() && !upLongFired) setSpeakerVolume(SETTINGS.audioVolume + 10, true);
  if (gpio.wasVolumeDownReleased() && !downLongFired) setSpeakerVolume(SETTINGS.audioVolume - 10, true);
  upHeldPrev = upHeld;
  downHeldPrev = downHeld;
}

// Runs even while side keys are blocked, so a popup drawn just before a page
// change still disappears. Never hides under the user's finger.
void hideVolumeOverlayIfDue() {
  if (s_volumeToastVisible && !s_volumeDragging &&
      static_cast<long>(millis() - s_volumeToastUntilMs) >= 0) {
    s_volumeToastVisible = false;
    activityManager.requestUpdate(true);
  }
}
}  // namespace

#if defined(ENABLE_CHINESE_VERSION) && defined(BOARD_HAS_PSRAM)
// Read Pico consumes FontCacheManager's missing codepoints in its main loop and
// offers the downloadable packs once per boot. The Plus shell had no consumer,
// so a missing glyph (a rare book title, a font-extra pack name) stayed tofu
// with no way to fix it. Keep the same stable-page guard: never stack the
// prompt over Wi-Fi selection or a reader page.
static void maybeOfferFontExtrasForMissingUiGlyph() {
    if (activityManager.isReaderActivity() || activityManager.isStandbyActivity()) return;
    if (FontExtrasActivity::wasPromptShownThisBoot()) return;
    auto* current = activityManager.currentActivityPtr();
    if (current == nullptr) return;
    const char* currentName = current->debugName();
    const bool stablePage = std::strcmp(currentName, "InxRecent") == 0 ||
                            std::strcmp(currentName, "LibraryList") == 0 ||
                            std::strcmp(currentName, "My") == 0 ||
                            std::strcmp(currentName, "Settings") == 0 ||
                            std::strcmp(currentName, "FindBooks") == 0;
    if (!stablePage) return;
    auto* fcm = renderer.getFontCacheManager();
    if (fcm == nullptr || !fcm->hasMissingCodepoint()) return;

    uint32_t codepoints[4] = {};
    const uint8_t count = fcm->consumeMissingCodepoints(codepoints, 4);
    if (count == 0) return;

    std::vector<std::string> families;
    font_extras::installedFallbackFamilies(families);
    const bool emojiReady = font_extras::emojiInstalled();
    bool fixable = false;
    for (uint8_t i = 0; i < count; ++i) {
        const uint32_t cp = codepoints[i];
        const bool emojiLike = (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF);
        if ((emojiLike && !emojiReady) || (!emojiLike && families.empty())) {
            fixable = true;
            break;
        }
    }
    if (!fixable) return;

    ESP_LOGI("PLUS", "missing UI glyph U+%04X; offering font extras",
             static_cast<unsigned>(codepoints[0]));
    FontExtrasActivity::suppressPromptThisBoot();
    activityManager.pushActivityWith<FontExtrasActivity>(
        renderer, mappedInputManager, FontExtrasActivity::Purpose::ReaderPrompt);
}
#endif

extern "C" void cpyos_shell_loop(void) {
    // loop() owns input polling, activity dispatch and render requests; the
    // render task created by begin() performs the panel pushes.
    plusPowerTick();
    if (usb_msc::running()) {
        // The SD card belongs to the USB host now; the activity layer must not
        // touch it. When the host releases the volume, reboot back to the app.
        if (usb_msc::hostEjected()) {
            usb_msc::stop();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        return;
    }
        // Bench ring/confirm commands posted by the console task run here, on the
    // same 32 KB UI task that handles real input.
    runPendingBenchAction();
    // Same order as the reference main loop: clear the per-frame input latches,
    // poll the touch/expander backend, then let the activity layer consume the
    // edges. Without both calls the shell would never see a tap or key.
    // The side keys turn pages only inside the reader; everywhere else they
    // drive the speaker volume (vendor rule). Set before update() so the frame's
    // mapping follows the activity that is about to run.
    gpio.setSideKeysNavigate(activityManager.isReaderActivity());
    gpio.beginFrame();
    mappedInputManager.update();
    // Physical AI key: short press opens the AI-voice page, long press opens it
    // and starts the turn as soon as the channel is up. Pages that are already
    // showing AI voice consume the key as a hands-free talk toggle; system
    // pages that must not lose their modal state ignore it.
    {
        const auto aiEvent = plus_ai_key::poll(millis());
        if (aiEvent != plus_ai_key::Event::None) {
            Activity* current = activityManager.currentActivityPtr();
            const char* name = current != nullptr ? current->debugName() : "";
            const bool blocked = activityManager.isSwitchPending() || plus_ai_key::routingBlocked(name);
            if (!blocked) {
                const bool longPress = aiEvent == plus_ai_key::Event::Long;
                if (!current->onAiKey(longPress)) {
                    activityManager.pushActivityWith<PlusVoiceActivity>(
                        renderer, mappedInputManager, longPress, /*fromKey=*/true);
                }
            }
        }
    }
    // A tap/drag on the volume HUD is consumed here, before the activity sees
    // the contact (and with its tap/swipe suppressed).
    handleVolumeOverlayTouch();
    // Power key gestures are decided from ONE hold timer: a tap opens the
    // standby overlay, a 3 s hold opens the CPYOS shutdown card. The release
    // edge used to be read as a tap unconditionally, so releasing a 3 s hold
    // pushed standby on top of the card and the screen looked like it only
    // locked. A hold between the tap window and 3 s does nothing.
    constexpr unsigned long kTapMaxMs = 1200;
    constexpr unsigned long kPowerHoldMs = 3000;
    // The card must be ON SCREEN when the user's 3 s hold ends, not after the
    // release. The panel needs ~0.4 s for the card's scan, so the prompt starts
    // a little early; a genuine release before 3 s still does not open it (the
    // release path below keeps the 3 s rule).
    constexpr unsigned long kPowerPromptMs = 2500;
    {
        static unsigned long powerDownMs = 0;
        static bool powerLongHandled = false;
        static bool powerHeldPrev = false;
        // The key that powered the device on is normally still down (or was
        // released a few hundred ms ago) when the shell loop starts. That press
        // is the boot key, not a "go to standby" tap: swallow it until the key
        // has actually been seen released after boot.
        static bool powerArmed = false;
        const bool benchHeld =
            s_benchPowerHoldUntil != 0 && static_cast<long>(millis() - s_benchPowerHoldUntil) < 0;
        if (!benchHeld) s_benchPowerHoldUntil = 0;
        const bool powerHeld = gpio.isPressed(HalGPIO::BTN_POWER) || benchHeld;
        if (!powerArmed) {
            if (!powerHeld || benchHeld) powerArmed = true;
            powerHeldPrev = powerHeld;
            if (powerHeld) {
                static bool loggedBootHold = false;
                if (!loggedBootHold) {
                    loggedBootHold = true;
                    ESP_LOGI("PLUS", "power key still down at boot; arming after release");
                }
            }
        } else {
            if (powerHeld && !powerHeldPrev) {
                powerDownMs = millis();
                powerLongHandled = false;
                // Mirrored to /.crosspoint/logs/boot.log: distinguishes a real
                // hold from a key the expander never reported.
                ESP_LOGI("PLUS", "power key down (P1=0x%02X)", gpio.debugPort1Stable());
            } else if (!powerHeld && powerHeldPrev) {
                const unsigned long heldMs = powerDownMs != 0 ? millis() - powerDownMs : 0;
                const bool longHandled = powerLongHandled;
                powerDownMs = 0;
                powerLongHandled = false;
                ESP_LOGI("PLUS", "power key up after %lu ms", heldMs);
                Activity* current = activityManager.currentActivityPtr();
                const bool powerOffCard =
                    current != nullptr && std::strcmp(current->debugName(), "PowerOff") == 0;
                // Decide the hold on release as well: a UI task that was blocked
                // by a library scan or a render can miss the 3 s instant while
                // the key is still down. A hold of 3 s must always reach the card.
                if (!longHandled && heldMs >= kPowerHoldMs && !powerOffCard &&
                    !activityManager.isSwitchPending() && !activityManager.isSleepActivity()) {
                    ESP_LOGI("PLUS", "power key released after %lu ms -> shutdown card", heldMs);
                    activityManager.pushActivityWith<PowerOffActivity>(renderer, mappedInputManager);
                } else if (!longHandled && heldMs < kTapMaxMs && !powerOffCard &&
                           SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::STANDBY &&
                           !activityManager.isStandbyActivity() && !activityManager.isSleepActivity() &&
                           // A release that closed the standby overlay (or a PMU
                           // release bounce right after it) must not become a
                           // second entry. Standby registers the deadline; the
                           // same gate covers the bounce.
                           !mappedInputManager.powerGestureBlocked()) {
                    activityManager.goToStandby();
                }
            }
            powerHeldPrev = powerHeld;
            if (powerHeld) {
                static unsigned long lastPowerLogMs = 0;
                if (millis() - lastPowerLogMs >= 1000) {
                    lastPowerLogMs = millis();
                    ESP_LOGI("PLUS", "power held %lu ms (P1=0x%02X)",
                             powerDownMs != 0 ? (unsigned long)(millis() - powerDownMs) : 0UL,
                             gpio.debugPort1Stable());
                }
            }
            // A 3 s hold must still reach the shutdown card from the standby
            // page (there a *tap* is the wake gesture, a hold is a power-off).
            const bool powerBusy = activityManager.isSwitchPending() || activityManager.isSleepActivity();
            if (powerHeld && powerDownMs != 0 && !powerLongHandled && !powerBusy &&
                millis() - powerDownMs >= kPowerPromptMs) {
                Activity* current = activityManager.currentActivityPtr();
                if (current != nullptr && std::strcmp(current->debugName(), "PowerOff") != 0) {
                    powerLongHandled = true;
                    ESP_LOGI("PLUS", "power key held %lu ms -> shutdown card (activity=%s)",
                             (unsigned long)(millis() - powerDownMs),
                             current != nullptr ? current->debugName() : "?");
                    activityManager.pushActivityWith<PowerOffActivity>(renderer, mappedInputManager);
                }
            }
        }
    }
    activityManager.loop();
    // Crash ambience (heap/psram/uptime) for the screen a panic boot shows; the
    // shared main samples this once a second, this port has its own loop.
    {
        static unsigned long lastAmbienceMs = 0;
        if (millis() - lastAmbienceMs >= 1000) {
            lastAmbienceMs = millis();
            HalSystem::snapshotCrashAmbience();
        }
    }
    // Shared font policy (network reclaim revert + role watchdog). src/main.cpp
    // is not part of this build, so without this call the Plus target would
    // keep the reclaim without the restore and the blank-label failure mode.
    font_health::tick(renderer, activityManager);
    // Battery sampling: the SoC policy averages one accepted sample per second,
    // so the gauge must be read from the loop, never from the renderer (that
    // inversion is why this port's capsule sat at the "no reading" placeholder
    // forever). Repaint a tab page when the shown level/validity/charging state
    // changes; the reader redraws on page turns, and a full scan mid-reading
    // would cost more than a slightly stale number.
    {
        static unsigned long lastBatteryCheckMs = 0;
        static uint16_t lastBatteryPercent = 0xFFFF;
        static bool lastBatteryKnown = false;
        static bool lastBatteryCharging = false;
        const unsigned long nowMs = millis();
        if (nowMs - lastBatteryCheckMs >= 1000) {
            lastBatteryCheckMs = nowMs;
            const uint16_t level = powerManager.getBatteryPercentage();
            const bool known = powerManager.batteryPercentageKnown();
            const bool charging = powerManager.isBatteryCharging();
            if (level != lastBatteryPercent || known != lastBatteryKnown ||
                charging != lastBatteryCharging) {
                lastBatteryPercent = level;
                lastBatteryKnown = known;
                lastBatteryCharging = charging;
                const Activity* batteryActivity = activityManager.currentActivityPtr();
                if (batteryActivity != nullptr && batteryActivity->mainTab() != MainTab::None &&
                    !activityManager.isReaderActivity() && !activityManager.isStandbyActivity() &&
                    !activityManager.isSwitchPending()) {
                    activityManager.requestUpdate();
                }
            }
        }
    }
#if defined(CROSSPOINT_PDF_XIP)
    // Read Pico flushes the deferred PDF-engine unmap from its main loop.  The
    // Plus owns the same loader API and must do the same, otherwise the mapped
    // engine (code + rodata + data aliases) stays alive for the whole boot
    // after the reader is left, which is neither parity nor necessary.
    if (pdf_module::flushScheduledUnload(!activityManager.hasActivityNamed("PdfReader"))) {
        ESP_LOGI("PLUS", "PDF engine unmapped after leaving the reader");
    }
#endif
    // Same network idle window as the reference main loop. Without this the
    // Plus port ignored SETTINGS.wifiIdleTimeoutSeconds completely.
    // Skipped while a radio command is executing: pollIdle re-asserts
    // WIFI_PS_NONE once a second, and that driver call is not something to
    // interleave with another task's mode change.
    if (NetworkStartup::radioIdle()) NetworkStartup::pollIdle(millis());
#if defined(ENABLE_CHINESE_VERSION) && defined(BOARD_HAS_PSRAM)
    maybeOfferFontExtrasForMissingUiGlyph();
#endif
    hideVolumeOverlayIfDue();
    // Second chance for the boot frame. On a cold battery start the TPS rails can
    // take longer than display.begin()'s three probes, so the first scan can run
    // with the panel unpowered and the previous screen (the shutdown card) stays
    // visible. Once the rails are certainly up, drive one full GC16 sweep and
    // repaint the current page, so boot always ends on a clean UI.
    {
        static bool bootRepaintDone = false;
        if (!bootRepaintDone && !plus_last_push_tps_ok() && millis() >= 7000) {
            bootRepaintDone = true;
            {
                RenderLock lock;
                display.refreshDisplay(HalDisplay::FULL_REFRESH, false);
            }
            activityManager.requestUpdate(true);
            ESP_LOGI("PLUS", "boot repaint: TPS failed during display.begin; full GC16 re-applied");
        }
    }
    // Vendor rule: the side keys are inert on the shutdown card, standby and
    // sleep pages (metalio-vendor SideKeysBlocked()).
    {
        Activity* volumeCur = activityManager.currentActivityPtr();
        const char* volumeName = volumeCur != nullptr ? volumeCur->debugName() : "";
        const bool sideKeysBlocked = std::strcmp(volumeName, "PowerOff") == 0 ||
                                     std::strcmp(volumeName, "Standby") == 0 ||
                                     std::strcmp(volumeName, "Sleep") == 0;
        if (!activityManager.isReaderActivity() && !sideKeysBlocked) handleSideVolumeKeys();
    }
    // The panel is bistable but the frontlight is not: leaving the LEDs lit on
    // the standby clock page just drains the battery. Read Pico has no
    // frontlight, so the shared standby page cannot do this; restore only what
    // this port turned off.
    {
        static bool standbyLightOff = false;
        const bool standby = activityManager.isStandbyActivity();
        if (standby && !standbyLightOff) {
            if (Frontlight.isOn()) {
                Frontlight.setOn(false);
                standbyLightOff = true;
            }
        } else if (!standby && standbyLightOff) {
            if (SETTINGS.frontlightOn) Frontlight.setOn(true);
            standbyLightOff = false;
        }
    }
    // Battery safety: the vendor cuts the pack at 3.1 V. This port keeps the
    // charger read-only, so the equivalent guard is a forced power-off through
    // the same P1.3 pulse train once the pack is at the cutoff and no adapter is
    // present. Runs every 30 s; a 10 s grace lets the warning repaint.
    {
        static unsigned long nextCheckMs = 0;
        static unsigned long cutAtMs = 0;
        const unsigned long nowMs = millis();
        if (nextCheckMs == 0 || static_cast<long>(nowMs - nextCheckMs) >= 0) {
            nextCheckMs = nowMs + 30000;
            plus_battery_sample_t b{};
            if (PlusBattery_Read(&b) && b.valid && !gpio.isUsbConnected() && b.millivolts > 0 &&
                b.millivolts <= 3100) {
                if (cutAtMs == 0) {
                    cutAtMs = nowMs + 10000;
                    ESP_LOGE("PLUS", "battery critical: %u mV, powering off in 10 s", (unsigned)b.millivolts);
                }
            } else {
                cutAtMs = 0;
            }
        }
        if (cutAtMs != 0 && static_cast<long>(nowMs - cutAtMs) >= 0) {
            ESP_LOGE("PLUS", "battery cutoff: %s", "pulsing P1.3");
            epd_board_metalio_eink4_plus_power_off_pulse();
            esp_restart();  // still alive (e.g. USB attached): never spin at 3.1 V
        }
    }
    // The per-frame press latches are still valid here: one pulse per new edge.
    gpio.updateHapticFeedback(SETTINGS.hapticFeedbackLevel);
#if CROSSPOINT_CAP_SOUND_FEEDBACK
    // Speaker cues (plus_audio.c -> BH1098G). The shared code takes the same
    // physical press mask Read Pico passes; level 0 means Off and shuts the PA.
    // Wait until the boot frame is on the panel before the BH1098 reset starts:
    // its TCA writes used to collide with the TPS power-on and blank the first
    // frame. Cues simply drop until the mode is ready.
    constexpr unsigned long kAudioWarmupDelayMs = 9000;
    constexpr unsigned long kAudioRetryDelayMs = 15000;
    constexpr int kAudioRetryLimit = 3;
    static unsigned long audioNextAttemptMs = kAudioWarmupDelayMs;
    static int audioRetries = 0;
    const unsigned long nowAudioMs = millis();
    if (SETTINGS.soundFeedbackLevel == 0) {
        audioRetries = 0;
        audioNextAttemptMs = kAudioWarmupDelayMs;
    } else if (plus_audio_state() == PLUS_AUDIO_NOT_READY && nowAudioMs >= audioNextAttemptMs &&
               WiFi.getMode() == WIFI_OFF) {
        plus_audio_init();
    } else if (plus_audio_state() == PLUS_AUDIO_FAILED && audioRetries < kAudioRetryLimit &&
               nowAudioMs >= audioNextAttemptMs && WiFi.getMode() == WIFI_OFF) {
        // A Wi-Fi scan can hold the internal DMA pool, which made
        // i2s_alloc_dma_desc fail and killed audio for the whole power cycle.
        // Retry now that the radio is idle, with a bounded backoff.
        audioRetries++;
        audioNextAttemptMs = nowAudioMs + kAudioRetryDelayMs;
        const esp_err_t retry = plus_audio_retry();
        ESP_LOGW("PLUS", "audio retry %d/%d: %s", audioRetries, kAudioRetryLimit, esp_err_to_name(retry));
    }
    // With 4G selected, bring the modem up once the boot UI and the BH1098 rail
    // reset are done (the reset shares P14). The official firmware starts the
    // cellular board at boot as well.
    {
        static bool cellularBootKick = false;
        if (!cellularBootKick && SETTINGS.networkType == CrossPointSettings::NETWORK_CELLULAR &&
            SETTINGS.cellularPresent) {
            const plus_audio_state_t audioState = plus_audio_state();
            const bool audioSettled = audioState == PLUS_AUDIO_READY || audioState == PLUS_AUDIO_FAILED ||
                                      SETTINGS.soundFeedbackLevel == 0;
            if (millis() >= 12000 && audioSettled) {
                cellularBootKick = true;
                xTaskCreate([](void*) {
                    plus_cellular_start(60000);
                    vTaskDelete(nullptr);
                }, "cell-boot", 8192, nullptr, 3, nullptr);
                ESP_LOGI("PLUS", "4G boot: modem worker started");
            }
        }
    }
    SoundFeedback::update(SETTINGS.soundFeedbackLevel, gpio.physicalPressedMask());
#endif
    updateBluetoothLifecycle();
    halClock.update();
    // Auto-standby: the CPYOS "time to sleep" setting now has a visible effect.
    // Light/deep sleep is still not implemented, so this only shows the standby
    // overlay (power key wakes it); it never cuts the radio or storage.
    {
        static unsigned long lastActivityMs = 0;
        const unsigned long nowMs = millis();
        if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity()) {
            lastActivityMs = nowMs;
        }
        const unsigned long timeoutMs =
            static_cast<unsigned long>(SETTINGS.sleepTimeoutMinutes) * 60000UL;
        if (timeoutMs != 0 && !activityManager.isStandbyActivity() && !activityManager.isSleepActivity() &&
            nowMs - lastActivityMs >= timeoutMs) {
            // The old WiFi.getMode()==NULL condition made one network session
            // disable auto-standby for the rest of the boot (releaseRadio keeps
            // the station initialised). The overlay is safe to enter with a
            // resident driver; light sleep decides for itself what may run.
            activityManager.goToStandby();
            lastActivityMs = nowMs;
        }
    }
    halTiltSensor.update(SETTINGS.motionPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());
    // Heartbeat: one line every 10 s proves the loop task is alive even when no
    // command is answered, and distinguishes "console works, app wedged" from
    // "USB peripheral dead" in the next session's log.
    {
        static unsigned long lastBeatMs = 0;
        static unsigned long beats = 0;
        const unsigned long nowMs = millis();
        if (lastBeatMs == 0) lastBeatMs = nowMs;
        if (nowMs - lastBeatMs >= 10000) {
            lastBeatMs = nowMs;
            ESP_LOGI("PLUS", "[hb] %lu s alive=%lu int_free=%u psram=%u", (unsigned long)(nowMs / 1000),
                     (unsigned long)++beats, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
    // Worker tasks (browser fetch, JJWXC jobs/cache service) run at IDLE
    // priority on this core. A busy loop starves them completely: the browser
    // sat at "loading" with no fetch-task log until this yield was added.
    vTaskDelay(pdMS_TO_TICKS(1));
}
