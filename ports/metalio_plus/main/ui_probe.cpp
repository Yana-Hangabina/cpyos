// CPYOS Plus bring-up screen.
//
// This is the first CPYOS-rendered UI on the Metalio E-INK4 Plus: the same
// GfxRenderer + EpdFont stack the reader uses, plus the FT6336U/cover-key input
// backend. It exists so a unit can be validated without a serial console:
//
//   header            build identity and "UNTESTED ON HARDWARE"
//   CPYOS 脆皮鸭       CJK font rendering through the real renderer
//   touch line        last contact, in portrait logical coordinates
//   event line        tap / long-press / swipe / cover key, with a counter
//   SD line           whether the 4-bit SDMMC mount succeeded
//
// Every tap redraws and pushes a full-sheet GC16, which also exercises the
// TPS65185 rail sequencing and the epdiy waveform table end to end.
//
// UNTESTED ON HARDWARE.
#include "SystemCoreFont.h"

#include <Arduino.h>
#include <esp_log.h>

#include <EpdFont.h>
#include <I18n.h>
#include <EpdFontFamily.h>
#include <builtinFonts/notosans_cjk_12.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "SystemFontPack.h"
#include "Typography.h"
#include "fontIds.h"
#include "components/UITheme.h"
#include "WiFi.h"
#include <LibraryBuilder.h>
#include "GfxRenderer.h"
#include "HalDisplay.h"
#include "HalClock.h"
#include "HalStorage.h"
#include "HalGPIO.h"
#include "HalPowerManager.h"
#include "cpyos_sd.h"
#include "metalio_i2c1.h"
#include "plus_battery.h"

// The vendor display headers pull C EpdFont typedefs that collide with the
// CPYOS C++ EpdFont classes; declare just the three status functions.
extern "C" bool MetalioEpd_Ready(void);
extern "C" bool epd_board_metalio_eink4_plus_ioexp_ok(void);
extern "C" void epd_board_metalio_eink4_plus_motor_set(bool on);

namespace {

EpdFont cjkFont(&notosans_cjk_12);
EpdFontFamily cjkFamily(&cjkFont);

GfxRenderer* s_renderer = nullptr;
// Font id the probe draws with: the vector UI face once the system pack is
// live, otherwise the built-in 12pt bitmap (registered as id 1).
int s_bodyFont = 1;
int s_lastLx = -1;
int s_lastLy = -1;
char s_lastEvent[48] = "none yet";
char s_library[64] = "library: not scanned";
bool s_libraryScanned = false;
bool s_wifiPage = false;
int s_wifiCount = 0;
char s_wifiLines[10][48] = {};
unsigned s_events = 0;
bool s_sdMounted = false;
char s_bootStatus[192] = "boot status pending";

uint32_t frame_ink_permille() {
    if (s_renderer == nullptr) return 0;
    const uint8_t* fb = s_renderer->getFrameBuffer();
    const size_t n = s_renderer->getBufferSize();
    if (fb == nullptr || n == 0) return 0;
    uint32_t ink = 0;
    for (size_t i = 0; i < n; ++i) ink += 8u - (uint32_t)__builtin_popcount(fb[i]);
    return (uint32_t)((uint64_t)ink * 1000u / ((uint64_t)n * 8u));
}

bool i2c_present(uint8_t addr) {
    if (!metalio_i2c1_lock(100)) return false;
    const bool ok = metalio_i2c1_probe_locked(I2C_NUM_1, addr, 20) == ESP_OK;
    metalio_i2c1_unlock();
    return ok;
}

void snapshot_boot_status() {
    snprintf(s_bootStatus, sizeof(s_bootStatus),
             "epd=%d ioexp=%d acks tca20=%d tps68=%d touch38=%d gauge55=%d rtc51=%d chg6b=%d imu19=%d",
             MetalioEpd_Ready() ? 1 : 0,
             epd_board_metalio_eink4_plus_ioexp_ok() ? 1 : 0,
             i2c_present(0x20) ? 1 : 0, i2c_present(0x68) ? 1 : 0,
             i2c_present(0x38) ? 1 : 0, i2c_present(0x55) ? 1 : 0,
             i2c_present(0x51) ? 1 : 0, i2c_present(0x6B) ? 1 : 0,
             i2c_present(0x19) ? 1 : 0);
}

const char* buttonName(uint8_t index) {
    switch (index) {
        case HalGPIO::BTN_BACK: return "home/back";
        case HalGPIO::BTN_UP: return "prev";
        case HalGPIO::BTN_DOWN: return "next";
        case HalGPIO::BTN_POWER: return "power";
        default: return "?";
    }
}

void redraw() {
    GfxRenderer& r = *s_renderer;
    const int w = r.getScreenWidth();
    const int h = r.getScreenHeight();

    r.clearScreen(0xFF);
    // The built-in CJK faces carry 4-bit coverage; pushing the 1-bit buffer
    // threw it away, which is why the first on-device text looked un-antialiased.
    // Draw the same page into the native 16-level canvas and commit that.
    const bool gray = (r.getGrayscaleLevels() == 16) && r.beginGrayscale16();
    // Pull the real CPY theme into the image: theme metrics, card frame and
    // text centering all run through UITheme exactly like the reader does.
    UITheme::getInstance().setTheme(CrossPointSettings::UI_THEME::CPY);
    GUI.drawCardFrame(r, Rect{8, 8, w - 16, 176}, false);

    // Flush with the active area: the panel is exactly 1216x684 and the
    // logical portrait is 684x1216, so there is no bezel to inset for.
    r.drawRect(0, 0, w, h, 3, true);
    r.drawLine(16, 92, w - 16, 92, 2, true);

    r.drawText(s_bodyFont, 24, 30, "CPYOS 脆皮鸭", true);
    r.drawText(s_bodyFont, 24, 62, "Metalio E-INK4 Plus bring-up", true);
    r.drawText(s_bodyFont, 360, 62, tr(STR_SETTINGS_TITLE), true);

    char line[96];
    snprintf(line, sizeof(line), "panel %dx%d  events %u", w, h, s_events);
    r.drawText(s_bodyFont, 24, 120, line, true);

    if (s_lastLx >= 0) {
        snprintf(line, sizeof(line), "touch %d, %d", s_lastLx, s_lastLy);
    } else {
        snprintf(line, sizeof(line), "touch --");
    }
    r.drawText(s_bodyFont, 24, 200, line, true);

    r.drawText(s_bodyFont, 24, 260, s_lastEvent, true);

    r.drawText(s_bodyFont, 24, 340, s_sdMounted ? "SD 4-bit: mounted" : "SD 4-bit: FAILED", true);

    plus_battery_sample_t battery{};
    if (PlusBattery_Read(&battery)) {
        snprintf(line, sizeof(line), "battery %d mV  %d mA  soc %d%%", battery.millivolts,
                 battery.milliamps, battery.soc_percent);
    } else {
        snprintf(line, sizeof(line), "battery: gauge did not answer");
    }
    r.drawText(s_bodyFont, 24, 385, line, true);

    r.drawText(s_bodyFont, 24, 705, s_library, true);

    snprintf(line, sizeof(line), "wifi: %s (home key scans)", WiFi.status() == WL_CONNECTED ? "connected"
                                                                                        : "idle");
    r.drawText(s_bodyFont, 24, 745, line, true);

    snprintf(line, sizeof(line), "settings %upt lang=%u density=%u",
             static_cast<unsigned>(SETTINGS.fontPointSize),
             static_cast<unsigned>(SETTINGS.language),
             static_cast<unsigned>(SETTINGS.uiDensity));
    r.drawText(s_bodyFont, 24, 665, line, true);

    uint16_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, minute = 0;
    if (halClock.getDateTime(year, month, day, hour, minute)) {
        snprintf(line, sizeof(line), "clock %04u-%02u-%02u %02u:%02u UTC", year, month, day, hour,
                 minute);
    } else {
        snprintf(line, sizeof(line), "clock: not set (PCF8563 empty)");
    }
    r.drawText(s_bodyFont, 24, 425, line, true);

    r.drawText(s_bodyFont, 24, 480, "Tap anywhere.", true);
    r.drawText(s_bodyFont, 24, 540, "Cover keys: home back / prev / next.", true);
    r.drawText(s_bodyFont, 24, 600, "Build-only. UNTESTED ON HARDWARE.", true);

    if (s_wifiPage) {
        r.fillRect(0, 0, w, h, false);
        r.drawText(s_bodyFont, 24, 24, "Wi-Fi scan (home key returns)", false);
        snprintf(line, sizeof(line), "%d networks", s_wifiCount);
        r.drawText(s_bodyFont, 24, 70, line, false);
        for (int i = 0; i < s_wifiCount && i < 10; ++i) {
            r.drawText(s_bodyFont, 24, 120 + i * 46, s_wifiLines[i], false);
        }
        if (gray) {
            r.commitGrayscale16(false);
        } else {
            r.displayBuffer(HalDisplay::FULL_REFRESH);
        }
        return;
    }

    // 16-level ramp: isolates "is the panel really showing grey" from
    // "is the glyph face anti-aliased". Left bar is black, right bar white.
    if (gray) {
        const int barW = w / 16;
        for (int i = 0; i < 16; ++i) {
            const uint8_t level = static_cast<uint8_t>(i * 17);
            for (int yy = 800; yy < 900; ++yy) {
                for (int xx = i * barW; xx < (i + 1) * barW; ++xx) {
                    r.drawGrayscale16Pixel(xx, yy, level);
                }
            }
        }
    }

    // Themed status bar: clock + battery pill + book-source badge, the same
    // code path every main-tab page uses on a real CPYOS device.
    GUI.drawMainTabStatusBar(r, Rect{0, h - 72, w, 72});

    // Crosshair at the last contact (portrait logical coordinates).
    if (s_lastLx >= 0) {
        const int x = s_lastLx;
        const int y = s_lastLy;
        r.drawLine(x - 14, y, x + 14, y, 3, true);
        r.drawLine(x, y - 14, x, y + 14, 3, true);
        r.drawRect(x - 6, y - 6, 12, 12, 2, true);
    }

    if (gray) {
        r.commitGrayscale16(false);
    } else {
        r.displayBuffer(HalDisplay::FULL_REFRESH);
    }
}

}  // namespace

extern "C" void cpyos_ui_probe(void) {
    display.begin();

    static GfxRenderer renderer(display);
    s_renderer = &renderer;
    renderer.begin();
    renderer.insertFont(1, cjkFamily);

    // Orientation was verified on hardware with a held test frame: the logical
    // 684x1216 portrait rotates to the panel through GfxRenderer::Portrait,
    // exactly like the vendor logical_to_physical and kiikoread's
    // EPD_ROT_INVERTED_PORTRAIT. HalDisplay therefore copies 1:1.

    s_sdMounted = cpyos_sd_is_mounted();
    if (cpyos_sd_is_mounted()) {
        Storage.begin();
        // Bench image: exercise the same first-boot release the shell performs,
        // so the compressed core can be verified on the glass without the shell.
        system_core_font::releaseToSd();
        ESP_LOGI("UI-PROBE", "core font: carrier=%d sd=%d", system_core_font::carrierPresent(),
                 system_core_font::ready());
        SETTINGS.loadFromFile();

        // Exercise the real book engine: one SD walk writes the CLX1 index and
        // (with readMetadata) opens each EPUB far enough to read title/author.
        library::BuildStats stats{};
        library::buildLibraryIndex("/", stats, true);
        snprintf(s_library, sizeof(s_library), "library: %u books, %u dirs, %u parsed",
                 static_cast<unsigned>(stats.books), static_cast<unsigned>(stats.folders),
                 static_cast<unsigned>(stats.parsed));
        s_libraryScanned = true;
    }
    // Vector system face: 4-bit coverage at every registered size. The
    // built-in bitmap families are 2-bit, which is why the first 1-bit page
    // looked jagged even after the 16-level commit.
    if (system_font_pack::begin(renderer)) {
        typography::apply(renderer, static_cast<uint8_t>(SETTINGS.uiDensity));
        s_bodyFont = UI_12_FONT_ID;
        ESP_LOGI("UI-PROBE", "system font pack ready; body font UI_12 (4-bit=%d)",
                 system_font_pack::faceIsFourBit() ? 1 : 0);
    } else {
        ESP_LOGW("UI-PROBE", "system font pack unavailable; built-in 12pt bitmap only");
    }
    I18N.init();
    I18N.setLanguage(Language::ZH_CN);
    powerManager.begin();
    halClock.begin();
    gpio.begin();
    snapshot_boot_status();
    // One startup pulse proves TCA P0.3 and the motor without a console.
    epd_board_metalio_eink4_plus_motor_set(true);
    delay(50);
    epd_board_metalio_eink4_plus_motor_set(false);
    ESP_LOGI("UI-PROBE", "haptic startup pulse 50ms");
    redraw();
    ESP_LOGI("UI-PROBE", "ready panel=%dx%d sd=%d library=%s",
             renderer.getScreenWidth(), renderer.getScreenHeight(), s_sdMounted ? 1 : 0, s_library);
    ESP_LOGI("UI-PROBE", "boot status: %s", s_bootStatus);
}

extern "C" void cpyos_ui_loop(void) {
    if (s_renderer == nullptr) {
        return;
    }
    gpio.beginFrame();
    gpio.update();

    bool changed = false;
    float nx = 0.0f;
    float ny = 0.0f;
    int lx = 0;
    int ly = 0;

    if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
        if (s_wifiPage) {
            s_wifiPage = false;
            redraw();
            return;
        }
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        s_wifiCount = WiFi.scanNetworks();
        if (s_wifiCount < 0) s_wifiCount = 0;
        for (int i = 0; i < s_wifiCount && i < 10; ++i) {
            snprintf(s_wifiLines[i], sizeof(s_wifiLines[i]), "%2d dBm  %s", (int)WiFi.RSSI(i),
                     WiFi.SSID(i).c_str());
        }
        s_wifiPage = true;
        redraw();
        return;
    }

    const auto note = [&](const char* text, float tx, float ty) {
        s_renderer->tapToLogical(tx, ty, lx, ly);
        s_lastLx = lx;
        s_lastLy = ly;
        snprintf(s_lastEvent, sizeof(s_lastEvent), "%s @ %d,%d", text, lx, ly);
        ++s_events;
        ESP_LOGI("UI-PROBE", "event %s #%u", s_lastEvent, (unsigned)s_events);
        changed = true;
    };

    if (gpio.wasTouchDown(nx, ny)) {
        note("down", nx, ny);
    }
    if (gpio.wasTouchTap(nx, ny)) {
        note("tap", nx, ny);
    }
    if (gpio.wasTouchLongPress(nx, ny)) {
        note("long-press", nx, ny);
    }
    float sx0 = 0.0f;
    float sy0 = 0.0f;
    float sx1 = 0.0f;
    float sy1 = 0.0f;
    if (gpio.wasSwipe(sx0, sy0, sx1, sy1)) {
        s_renderer->tapToLogical(sx1, sy1, lx, ly);
        s_lastLx = lx;
        s_lastLy = ly;
        snprintf(s_lastEvent, sizeof(s_lastEvent), "swipe @ %d,%d", lx, ly);
        ++s_events;
        changed = true;
    }
    for (uint8_t button = 0; button < 7; ++button) {
        if (gpio.wasPressed(button)) {
            snprintf(s_lastEvent, sizeof(s_lastEvent), "key %s", buttonName(button));
            ++s_events;
            ESP_LOGI("UI-PROBE", "event %s #%u", s_lastEvent, (unsigned)s_events);
            changed = true;
        }
    }

    if (changed) {
        redraw();
    }

    // Periodic liveness/status line. The USB-Serial/JTAG console disappears
    // when I2C takes GPIO0/1 (vendor design), so this heartbeat is what the
    // serial capture can still show after the app is up.
    static uint32_t lastHeartbeat = 0;
    const uint32_t now = millis();
    if (lastHeartbeat == 0 || now - lastHeartbeat >= 2000) {
        lastHeartbeat = now;
        ESP_LOGI("UI-PROBE", "alive events=%u touch=%d,%d sd=%d event=%s | ink_permille=%u inv=%d setinv=%u | %s",
                 (unsigned)s_events, s_lastLx, s_lastLy, s_sdMounted ? 1 : 0, s_lastEvent,
                 (unsigned)frame_ink_permille(), display.isInverted() ? 1 : 0,
                 (unsigned)SETTINGS.screenInverted, s_bootStatus);
    }
}
