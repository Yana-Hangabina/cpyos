#include "HalDisplay.h"

#include <esp_system.h>

#include <cstdlib>
#include <cstring>

#include "BoardConfig.h"
#include "config.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "metalio_epd.h"
#include "epd_board_metalio_eink4_plus.h"
#include "my_waveform.h"
#include "HalPowerManager.h"

// The vendor board header defines DISPLAY_WIDTH/HEIGHT as macros; our
// HalDisplay exposes the same names as class constants.
#undef DISPLAY_WIDTH
#undef DISPLAY_HEIGHT

namespace {

uint8_t* s_frameBuffer = nullptr;
// 4-bit CPYOS grayscale canvas, one nibble per pixel (0 = black, 15 = white).
// Allocated on first use and kept for the session: freeing/reallocating a
// 415 KB PSRAM block per page would fragment the heap for no gain.
uint8_t* s_grayBuffer = nullptr;
bool s_firstPush = true;
// The first frame after boot must not assume a white panel: the machine may
// have been switched off while the "powered off" screen was showing, and the
// from-white first-paint only drives pixels that need ink, leaving that image
// visible under the new page. A full GC16 sweep clears whatever was there.
bool s_bootClean = true;
// Whether the most recent push actually powered the EPD PMIC; the flag inside the
// vendor BSP is cleared by poweroff(), so it has to be sampled between poweron
// and poweroff.
bool s_lastPushTpsOk = true;
bool s_inverted = false;
bool s_railsOn = false;
uint32_t s_lastPanelMs = 0;
uint32_t s_pushFrames = 0;
uint64_t s_pushUs = 0;
uint32_t s_grayScans = 0;

void notePush(int64_t startedUs) {
    ++s_pushFrames;
    const int64_t elapsed = esp_timer_get_time() - startedUs;
    if (elapsed > 0) s_pushUs += static_cast<uint64_t>(elapsed);
}

void ensureFrameBuffer() {
    if (s_frameBuffer != nullptr) {
        return;
    }
    s_frameBuffer = static_cast<uint8_t*>(
        heap_caps_malloc(HalDisplay::BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_frameBuffer == nullptr) {
        s_frameBuffer = static_cast<uint8_t*>(std::malloc(HalDisplay::BUFFER_SIZE));
    }
    if (s_frameBuffer != nullptr) {
        std::memset(s_frameBuffer, 0xFF, HalDisplay::BUFFER_SIZE);
    }
}

void blitBwRect(int x, int y, int w, int h);

// Transient-UI waveform. Every keystroke, overlay step and slider frame used to
// pay a full 48-phase differential GL16 scan (~735 ms measured on the glass),
// which made typing a password feel far worse than on Read Pico. This panel
// family's reference firmware drives monochrome UI with DU, so transient frames
// default to the 20-phase DU table; a GL16 frame every kTransientCleanEvery
// pushes bounds the ghost DU leaves behind.
//
// 0 = GL16 (old behaviour), 1 = DU 20 phases, 2 = 14-phase DU, 3 = trimmed
// 36/37-phase GL. The console `wave` command switches it at run time so the
// ghosting can be judged on the glass without a rebuild.
static int s_transientWave = 1;
static uint8_t s_transientRun = 0;
static constexpr uint8_t kTransientCleanEvery = 8;
// Page-turn animation request (GfxRenderer::setPageTurnDirection). The vendor
// spatial wipe is a GL16 frame with ten phase-staggered bands -- the same idea
// as Read Pico's water ripple -- so it is consumed once by the next non-full,
// non-fast push. 0 = plain differential update.
static int s_pageTurnDir = 0;
// Windowed scans are the one display path with no bounded wait inside the vendor
// stack. Arm this before the scan; a small task restarts the unit if it never
// returns (the board has no reset button and a wedged scan leaves the panel
// stuck). Created lazily so boards that never use a window pay nothing.
static volatile bool s_scanBusy = false;
static volatile unsigned long s_scanDeadlineMs = 0;
static void armScanWatchdog(unsigned long ms);
static void disarmScanWatchdog();

static void scanWatchdogTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (s_scanBusy && static_cast<long>(millis() - s_scanDeadlineMs) > 0) {
            ESP_LOGE("PLUS", "windowed scan did not return in time; restarting");
            esp_restart();
        }
    }
}

static void armScanWatchdog(unsigned long ms) {
    static bool started = false;
    if (!started) {
        started = true;
        xTaskCreate(scanWatchdogTask, "scanwd", 3072, nullptr, 1, nullptr);
    }
    s_scanDeadlineMs = millis() + ms;
    s_scanBusy = true;
}

static void disarmScanWatchdog() { s_scanBusy = false; }

// Bits 13..14 of MODE_SPATIAL_WIPE_GL encode the UI orientation that picks the
// band axis/order; 0 = bands along the panel height, reversed (verified on
// glass as a top-to-bottom reveal).
static constexpr int kSpatialWipeOrient = 0;

extern "C" void plus_epd_set_transient_wave(int value) {
    if (value >= 0 && value <= 3) {
        s_transientWave = value;
        s_transientRun = 0;
    }
}
extern "C" int plus_epd_transient_wave(void) { return s_transientWave; }

static int takePageTurnDir() {
    const int dir = s_pageTurnDir;
    s_pageTurnDir = 0;
    return dir;
}

static EpdDrawMode spatialWipeMode(const int dir) {
    return static_cast<EpdDrawMode>(MODE_GL16 | MODE_SPATIAL_WIPE_GL | (kSpatialWipeOrient << 13) |
                                    (dir < 0 ? MODE_SPATIAL_WIPE_REVERSE : 0));
}

void pushFrameBuffer(HalDisplay::RefreshMode mode, DisplayRefreshContext context) {
    const int turnDir = takePageTurnDir();
    // Conversion + scan are CPU-bound; keep DFS at max while they run.
    HalPowerManager::Lock pmLock(true);
    if (s_frameBuffer == nullptr) {
        return;
    }
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    if (hl == nullptr) {
        return;
    }
    uint8_t* epd = epd_hl_get_framebuffer(hl);
    const int width = HalDisplay::DISPLAY_WIDTH;
    const int height = HalDisplay::DISPLAY_HEIGHT;
    // Whole-row bit expansion. The epdiy 4bpp buffer packs two pixels per byte,
    // high nibble = even pixel; a byte of 8 source bits therefore becomes 4
    // target bytes. The old per-pixel epd_draw_pixel loop cost ~300 ms per frame
    // (830k calls) -- more than half the 48-phase scan it feeds.
    static uint32_t s_expand[256];
    static bool s_expandReady = false;
    if (!s_expandReady) {
        for (int v = 0; v < 256; ++v) {
            uint8_t out[4];
            for (int i = 0; i < 4; ++i) {
                const int b0 = (v >> (7 - 2 * i)) & 1;  // even pixel (left)
                const int b1 = (v >> (6 - 2 * i)) & 1;  // odd pixel (right)
                // epd_draw_pixel(): even x writes the LOW nibble, odd x the HIGH
                // one. Pairing them the other way swaps every two pixels and
                // turns every glyph into a 1-px comb (seen on the panel
                // 2026-11-11 as "every character jagged and ghosted").
                out[i] = static_cast<uint8_t>((b0 ? 0x0F : 0x00) | (b1 ? 0xF0 : 0x00));
            }
            memcpy(&s_expand[v], out, 4);
        }
        s_expandReady = true;
    }
    const uint8_t invertXor = s_inverted ? 0xFF : 0x00;
    const size_t dstStride = static_cast<size_t>(width) / 2;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = s_frameBuffer + static_cast<size_t>(y) * HalDisplay::DISPLAY_WIDTH_BYTES;
        uint8_t* dst = epd + static_cast<size_t>(y) * dstStride;
        for (int bx = 0; bx < width / 8; ++bx) {
            memcpy(dst + bx * 4, &s_expand[row[bx] ^ invertXor], 4);
        }
    }

    const int64_t startedUs = esp_timer_get_time();
    epd_poweron();
    s_lastPushTpsOk = epd_board_metalio_eink4_plus_tps_ok();
    s_railsOn = true;
    s_lastPanelMs = millis();
    // The waveform is only swapped by experimental paths; keep the balanced
    // profile explicit so a stale selection cannot leak in.
    epd_hl_waveform(hl, &MY_WAVEFORM);
    armScanWatchdog(3000);
    if (s_bootClean) {
        s_bootClean = false;
        s_firstPush = false;
        epd_hl_update_screen_full(hl, MODE_GC16, 25);
        ESP_LOGI("PLUS", "boot clean drive (GC16, whole panel)");
    } else if (s_firstPush) {
        epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
        s_firstPush = false;
    } else if (mode == HalDisplay::FULL_REFRESH) {
        // Full sweep with the GC16 conditioning pulse (periodic clean, explicit
        // clean, first frame after a grayscale page).
        epd_hl_update_screen_full(hl, MODE_GC16, 25);
        ESP_LOGI("PLUS", "full clean drive (GC16, whole panel)");
    } else {
        // HALF/FAST on a transient UI frame: the DU path above. Everything else
        // stays the differential GL16 CPYOS uses everywhere (Read Pico promotes
        // FAST to HALF for the same ghosting reason).
        // Fast page turns (mode FAST + ContinuousReading) are the same kind of
        // small-change frame as typing: Read Pico drives them with DU and accepts
        // the light ghost until the periodic clean. Measured here: a GL16 page
        // is ~890 ms, DU ~350-400 ms.
        const bool transient = context == DisplayRefreshContext::TransientUi;
        const bool readerFast =
            mode == HalDisplay::FAST_REFRESH && context == DisplayRefreshContext::ContinuousReading;
        const bool fastPath = transient || readerFast;
        if (!fastPath) s_transientRun = 0;
        const bool dueClean = fastPath && (++s_transientRun % kTransientCleanEvery) == 0;
        const char* waveName = "GL16";
        bool done = false;
        // Water-ripple parity: the vendor renders a full-screen GL16 wipe with
        // ten bands, each starting one phase later (frame_count += 9). It needs
        // the full-area difference frame, so it cannot combine with the DU fast
        // path; a rejected request falls back to the ordinary GL16 push.
        if (!transient && turnDir != 0) {
            epd_hl_waveform(hl, &MY_WAVEFORM);
            const EpdDrawError wipeErr = epd_hl_update_screen(hl, spatialWipeMode(turnDir), 25);
            if (wipeErr == EPD_DRAW_SUCCESS) {
                done = true;
                waveName = "WIPE";
            } else {
                ESP_LOGW("PLUS", "spatial wipe rejected err=%d", static_cast<int>(wipeErr));
            }
        }
        if (fastPath && !dueClean && s_transientWave != 0) {
            if (s_transientWave == 1) {
                epd_hl_waveform(hl, &MY_WAVEFORM);
                done = epd_hl_update_screen(hl, MODE_DU, 25) == EPD_DRAW_SUCCESS;
                waveName = "DU20";
            } else if (s_transientWave == 2) {
                const EpdWaveform* fast = my_waveform_fast_du();
                epd_hl_waveform(hl, fast ? fast : &MY_WAVEFORM);
                done = epd_hl_update_screen(hl, MODE_DU, 25) == EPD_DRAW_SUCCESS;
                waveName = "DU14";
            } else {
                const EpdWaveform* trimmed = my_waveform_fast();
                epd_hl_waveform(hl, trimmed ? trimmed : &MY_WAVEFORM);
                done = epd_hl_update_screen(hl, MODE_GL16, 25) == EPD_DRAW_SUCCESS;
                waveName = "GL36";
            }
        }
        if (!done) {
            epd_hl_waveform(hl, &MY_WAVEFORM);
            epd_hl_update_screen(hl, MODE_GL16, 25);
            waveName = "GL16";
        }
        ESP_LOGI("PLUS", "wave %s", waveName);
    }
    disarmScanWatchdog();
    epd_poweroff();
    s_railsOn = false;
    notePush(startedUs);
    ESP_LOGI("PLUS", "push %s ctx=%d %ums", mode == HalDisplay::FULL_REFRESH ? "FULL" : (mode == HalDisplay::HALF_REFRESH ? "HALF" : "FAST"),
             static_cast<int>(context), static_cast<unsigned>((esp_timer_get_time() - startedUs) / 1000));
}

uint8_t* ensureGrayBuffer() {
    if (s_grayBuffer != nullptr) return s_grayBuffer;
    const size_t bytes = static_cast<size_t>(HalDisplay::DISPLAY_WIDTH) *
                         HalDisplay::DISPLAY_HEIGHT / 2;
    s_grayBuffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_grayBuffer == nullptr) {
        s_grayBuffer = static_cast<uint8_t*>(std::malloc(bytes));
    }
    if (s_grayBuffer != nullptr) {
        std::memset(s_grayBuffer, 0xFF, bytes);
    }
    return s_grayBuffer;
}

void pushGrayBuffer(bool clean) {
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    const int turnDir = takePageTurnDir();
    if (hl == nullptr || s_grayBuffer == nullptr) return;
    uint8_t* epd = epd_hl_get_framebuffer(hl);
    const int width = HalDisplay::DISPLAY_WIDTH;
    const int height = HalDisplay::DISPLAY_HEIGHT;
    // Gray source: low nibble = even pixel, high nibble = odd pixel. Target:
    // high nibble = even pixel. One byte in, one byte out; inversion flips both
    // nibbles. This replaces the per-pixel epd_draw_pixel loop (same ~300 ms
    // saving as the B/W path).
    const size_t dstStride = static_cast<size_t>(width) / 2;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = s_grayBuffer + static_cast<size_t>(y) * (width / 2);
        uint8_t* dst = epd + static_cast<size_t>(y) * dstStride;
        for (size_t i = 0; i < dstStride; ++i) {
            // Both buffers use low nibble = even pixel, high nibble = odd pixel,
            // so the byte passes through unchanged; inversion is 15 - n in each
            // nibble, i.e. 0xFF - byte.
            dst[i] = s_inverted ? static_cast<uint8_t>(0xFF - row[i]) : row[i];
        }
    }
    const int64_t startedUs = esp_timer_get_time();
    epd_poweron();
    s_lastPushTpsOk = epd_board_metalio_eink4_plus_tps_ok();
    s_railsOn = true;
    s_lastPanelMs = millis();
    bool bootClean = false;
    if (s_bootClean) {
        bootClean = true;
        s_bootClean = false;
        s_firstPush = false;
        epd_hl_update_screen_full(hl, MODE_GC16, 25);
    } else if (s_firstPush) {
        epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
        s_firstPush = false;
    } else if (clean) {
        epd_hl_update_screen(hl, MODE_GC16, 25);
    } else if (turnDir != 0) {
        // Water-ripple page turn: the vendor's spatial GL16 wipe. A rejected
        // request falls back to the plain differential push so no frame is lost.
        const EpdDrawError wipeErr = epd_hl_update_screen(hl, spatialWipeMode(turnDir), 25);
        if (wipeErr != EPD_DRAW_SUCCESS) {
            ESP_LOGW("PLUS", "spatial wipe rejected err=%d", static_cast<int>(wipeErr));
            epd_hl_update_screen(hl, MODE_GL16, 25);
        } else {
            ESP_LOGI("PLUS", "gray wipe dir=%d", turnDir);
        }
    } else {
        // Ordinary 16-level page: GL16 keeps the grays without the GC16
        // black/white flash on every turn. The shared cadence still requests a
        // clean commit periodically, which is the GC16 branch above.
        epd_hl_update_screen(hl, MODE_GL16, 25);
    }
    epd_poweroff();
    s_railsOn = false;
    ++s_grayScans;
    ESP_LOGI("PLUS", "gray %s %ums", bootClean ? "GC16-boot" : (clean ? "GC16" : "GL16"),
             static_cast<unsigned>((esp_timer_get_time() - startedUs) / 1000));
    notePush(startedUs);
}

// Copy a rect of the 1-bit framebuffer into the epdiy 4-bit framebuffer and
// push only that rect. The rest of the epdiy framebuffer still holds the
// previous frame, which is exactly the differential baseline epdiy needs.
void blitBwRect(int x, int y, int w, int h) {
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    if (hl == nullptr || s_frameBuffer == nullptr) return;
    uint8_t* epd = epd_hl_get_framebuffer(hl);
    const int width = HalDisplay::DISPLAY_WIDTH;
    const int height = HalDisplay::DISPLAY_HEIGHT;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0) return;
    const int x0 = x & ~7;                       // byte-aligned start
    const int x1 = (x + w + 7) & ~7;             // byte-aligned end
    const int firstByte = x0 / 8;
    const int byteCount = (x1 - x0) / 8;
    const size_t dstStride = static_cast<size_t>(width) / 2;
    const uint8_t invertXor = s_inverted ? 0xFF : 0x00;
    for (int yy = y; yy < y + h; ++yy) {
        const uint8_t* row = s_frameBuffer + static_cast<size_t>(yy) * HalDisplay::DISPLAY_WIDTH_BYTES;
        uint8_t* dst = epd + static_cast<size_t>(yy) * dstStride;
        for (int i = 0; i < byteCount; ++i) {
            const uint8_t bits = static_cast<uint8_t>(row[firstByte + i] ^ invertXor);
            uint8_t out[4];
            for (int k = 0; k < 4; ++k) {
                const int b0 = (bits >> (7 - 2 * k)) & 1;
                const int b1 = (bits >> (6 - 2 * k)) & 1;
                out[k] = static_cast<uint8_t>((b0 ? 0x0F : 0x00) | (b1 ? 0xF0 : 0x00));
            }
            memcpy(dst + (firstByte + i) * 4, out, 4);
        }
    }
}

}  // namespace

HalDisplay display;

HalDisplay::HalDisplay() = default;
HalDisplay::~HalDisplay() = default;

HalDisplay::Controller HalDisplay::getController() const {
    return BoardConfig::DisplayController::LgfxEpd;
}

HalDisplay::GrayscaleCapabilities HalDisplay::grayscaleCapabilities(GrayscaleMode mode) const {
    GrayscaleCapabilities caps;
    // Absolute/Direct upload a complete 4bpp canvas, which is exactly what this
    // port's 16-level path does. The LSB/MSB overlay-mask API is not implemented.
    if (mode == GrayscaleMode::Overlay) return caps;
    caps.encoding = GrayscaleEncoding::AbsolutePlanes;
    caps.base = GrayscaleBase::Separate;
    return caps;
}

void HalDisplay::begin(bool) {
    ensureFrameBuffer();
    if (!MetalioEpd_Ready()) {
        MetalioEpd_Init();
    }
}

void HalDisplay::clearScreen(uint8_t color) const {
    if (s_frameBuffer != nullptr) {
        std::memset(s_frameBuffer, color, BUFFER_SIZE);
    }
}

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool) const {
    if (imageData == nullptr || s_frameBuffer == nullptr) {
        return;
    }
    const uint16_t imageWidthBytes = w / 8;
    for (uint16_t row = 0; row < h; ++row) {
        const uint16_t destY = y + row;
        if (destY >= DISPLAY_HEIGHT) {
            break;
        }
        const size_t destOffset = static_cast<size_t>(destY) * DISPLAY_WIDTH_BYTES + (x / 8);
        const size_t srcOffset = static_cast<size_t>(row) * imageWidthBytes;
        for (uint16_t col = 0; col < imageWidthBytes; ++col) {
            if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES) {
                break;
            }
            s_frameBuffer[destOffset + col] = imageData[srcOffset + col];
        }
    }
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w,
                                      uint16_t h, bool) const {
    if (imageData == nullptr || s_frameBuffer == nullptr) {
        return;
    }
    const uint16_t imageWidthBytes = w / 8;
    for (uint16_t row = 0; row < h; ++row) {
        const uint16_t destY = y + row;
        if (destY >= DISPLAY_HEIGHT) {
            break;
        }
        const size_t destOffset = static_cast<size_t>(destY) * DISPLAY_WIDTH_BYTES + (x / 8);
        const size_t srcOffset = static_cast<size_t>(row) * imageWidthBytes;
        for (uint16_t col = 0; col < imageWidthBytes; ++col) {
            if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES) {
                break;
            }
            s_frameBuffer[destOffset + col] &= imageData[srcOffset + col];
        }
    }
}

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen, DisplayRefreshContext context) {
    pushFrameBuffer(mode, context);
}

void HalDisplay::displayBufferAsync(RefreshMode mode, DisplayRefreshContext context) {
    pushFrameBuffer(mode, context);
}

void HalDisplay::waitRefreshComplete() {}

bool HalDisplay::supportsAsyncRefresh() const { return false; }

bool HalDisplay::supportsAsyncGrayscaleBase() const { return false; }

void HalDisplay::refreshDisplay(RefreshMode mode, bool) { pushFrameBuffer(mode, DisplayRefreshContext::Normal); }

void HalDisplay::setInverted(bool inverted) { s_inverted = inverted; }

bool HalDisplay::toggleInverted() {
    s_inverted = !s_inverted;
    return s_inverted;
}

bool HalDisplay::isInverted() const { return s_inverted; }

void HalDisplay::deepSleep() { epd_poweroff(); }

uint8_t* HalDisplay::getFrameBuffer() const { return s_frameBuffer; }

void HalDisplay::syncFrameBufferFromActive() {}

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t*) { return nullptr; }

void HalDisplay::returnFrameBufferStorage() {}

void HalDisplay::preconditionGrayscale() {}

void HalDisplay::preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen, DisplayRefreshContext) {
    refreshDisplay(fallback, turnOffScreen);
}

bool HalDisplay::displayGrayscaleBase(GrayscaleMode, RefreshMode fallback, bool turnOffScreen) {
    refreshDisplay(fallback, turnOffScreen);
    return true;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t*, const uint8_t*) {}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t*) {}

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t*) {}

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t*) {}

void HalDisplay::displayGrayBuffer(bool) { commitGrayscale16(false); }

void HalDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool) {
    HalPowerManager::Lock pmLock(true);
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    if (hl == nullptr || s_frameBuffer == nullptr || w == 0 || h == 0) {
        return;
    }
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) {
        return;
    }
    // Reject empty/out-of-range windows. The rect is passed to
    // epd_hl_update_area below under the scan watchdog; the bounds check keeps
    // the API contract and catches caller mistakes.
    int cx = x;
    int cy = y;
    int cw = w;
    int ch = h;
    if (cx + cw > DISPLAY_WIDTH) cw = DISPLAY_WIDTH - cx;
    if (cy + ch > DISPLAY_HEIGHT) ch = DISPLAY_HEIGHT - cy;
    if (cw <= 0 || ch <= 0) {
        return;
    }
    // Real window update: copy only the rect into the epdiy front buffer and let
    // epd_hl_update_area diff it against the back buffer. Only the changed pixels
    // are driven (the scan still covers the panel), which is what keeps a slider
    // drag from re-driving the whole page on every step -- the reference
    // firmware's UI works the same way.
    //
    // A rectangle taken before the first full frame has no trusted back buffer,
    // so the first window falls back to the complete push.
    if (s_firstPush) {
        pushFrameBuffer(HALF_REFRESH, DisplayRefreshContext::Normal);
        return;
    }
    blitBwRect(cx, cy, cw, ch);
    const EpdRect area = {static_cast<uint16_t>(cx), static_cast<uint16_t>(cy),
                          static_cast<uint16_t>(cw), static_cast<uint16_t>(ch)};
    const int64_t startedUs = esp_timer_get_time();
    epd_poweron();
    s_lastPushTpsOk = epd_board_metalio_eink4_plus_tps_ok();
    s_railsOn = true;
    s_lastPanelMs = millis();
    epd_hl_waveform(hl, &MY_WAVEFORM);
    armScanWatchdog(3000);
    const EpdDrawError err = epd_hl_update_area(hl, MODE_GL16, 25, area);
    disarmScanWatchdog();
    epd_poweroff();
    s_railsOn = false;
    notePush(startedUs);
    ESP_LOGI("PLUS", "window %d,%d %dx%d %ums err=%d", cx, cy, cw, ch,
             static_cast<unsigned>((esp_timer_get_time() - startedUs) / 1000), static_cast<int>(err));
    if (err != EPD_DRAW_SUCCESS) {
        // A failed area scan must not leave the panel half driven: re-present the
        // whole frame with the differential waveform.
        static bool warned = false;
        if (!warned) {
            ESP_LOGW("PLUS", "area update failed (%d); falling back to a full push", static_cast<int>(err));
            warned = true;
        }
        pushFrameBuffer(HALF_REFRESH, DisplayRefreshContext::Normal);
    }
}

// Force-drive one rectangle with GC16. epd_hl_update_area() skips unchanged
// pixels (a differential clean leaves exactly the ghosting this call removes),
// but the vendor port also carries epd_hl_update_area_full(), which masks every
// pixel of the rect as dirty while still scanning the same full lines. That is
// what a local clean needs: no whole-sheet flash, and same-colour conditioning
// pulses are kept. If the area path refuses, fall back to the old full clean.
bool HalDisplay::cleanWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    if (hl == nullptr || s_frameBuffer == nullptr) {
        return false;
    }
    int cx = x;
    int cy = y;
    int cw = w;
    int ch = h;
    if (cx >= DISPLAY_WIDTH || cy >= DISPLAY_HEIGHT) return false;
    if (cx + cw > DISPLAY_WIDTH) cw = DISPLAY_WIDTH - cx;
    if (cy + ch > DISPLAY_HEIGHT) ch = DISPLAY_HEIGHT - cy;
    if (cw <= 0 || ch <= 0) return false;
    blitBwRect(cx, cy, cw, ch);
    const int64_t startedUs = esp_timer_get_time();
    epd_poweron();
    s_lastPushTpsOk = epd_board_metalio_eink4_plus_tps_ok();
    s_railsOn = true;
    epd_hl_waveform(hl, &MY_WAVEFORM);
    const EpdRect area{cx, cy, cw, ch};
    armScanWatchdog(3000);
    enum EpdDrawError err = epd_hl_update_area_full(hl, MODE_GC16, 25, area);
    disarmScanWatchdog();
    if (err != EPD_DRAW_SUCCESS) {
        ESP_LOGW("HAL", "area clean failed (%d); using a full GC16", static_cast<int>(err));
        epd_hl_update_screen_full(hl, MODE_GC16, 25);
    }
    epd_poweroff();
    s_railsOn = false;
    s_lastPanelMs = static_cast<uint32_t>((esp_timer_get_time() - startedUs) / 1000);
    s_firstPush = false;
    notePush(startedUs);
    // The full-sheet fallback already cleaned the panel; reporting false here
    // made callers treat a successful clean as a failure and retry.
    return true;
}

bool HalDisplay::waitForScanIdle(uint32_t) { return true; }

bool HalDisplay::idlePowerTick(uint32_t) { return false; }

// "Take" is read-and-reset: the diagnostics view reports a window, not a
// lifetime total.
void HalDisplay::takePanelPushStats(uint32_t* frames, uint64_t* us) {
    if (frames != nullptr) {
        *frames = s_pushFrames;
    }
    if (us != nullptr) {
        *us = s_pushUs;
    }
    s_pushFrames = 0;
    s_pushUs = 0;
}

const uint8_t* HalDisplay::frameBufferSnapshot() { return s_frameBuffer; }

bool HalDisplay::frameBufferInverted() { return s_inverted; }

uint32_t HalDisplay::takeGrayScanCount() {
    const uint32_t count = s_grayScans;
    s_grayScans = 0;
    return count;
}

void HalDisplay::writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}

bool HalDisplay::supportsStripGrayscale() const { return false; }

bool HalDisplay::combinesGrayscaleBase() const { return false; }

bool HalDisplay::supportsTextOnlyCombinedBase() const { return false; }

bool HalDisplay::supportsReaderTransitions() const { return false; }

bool HalDisplay::supportsContinuousImageReading() const { return false; }

bool HalDisplay::canUseTextTransition() const { return false; }

void HalDisplay::cancelGrayscale() {}

void HalDisplay::setPageTurnDirection(int direction) {
    s_pageTurnDir = direction > 0 ? 1 : (direction < 0 ? -1 : 0);
}

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }

uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }

uint8_t HalDisplay::getGrayscaleLevels() const { return 16; }

uint8_t* HalDisplay::beginGrayscale16() {
    uint8_t* gray = ensureGrayBuffer();
    if (gray != nullptr) {
        std::memset(gray, 0xFF, static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT / 2);
    }
    return gray;
}

uint8_t* HalDisplay::beginGrayscale16From1bpp(const uint8_t* fb) {
    uint8_t* gray = ensureGrayBuffer();
    if (gray == nullptr || fb == nullptr) return gray;
    const int width = DISPLAY_WIDTH;
    const int height = DISPLAY_HEIGHT;
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = fb + static_cast<size_t>(y) * DISPLAY_WIDTH_BYTES;
        uint8_t* dst = gray + static_cast<size_t>(y) * (width / 2);
        for (int x = 0; x < width; x += 2) {
            const uint8_t bits = src[x >> 3];
            // Bit 1 is paper in the 1-bit buffer; the low nibble is the even pixel.
            const uint8_t even = (bits & (0x80 >> (x & 7))) ? 0x0F : 0x00;
            const uint8_t odd = (bits & (0x40 >> (x & 7))) ? 0x0F : 0x00;
            dst[x >> 1] = static_cast<uint8_t>(even | static_cast<uint8_t>(odd << 4));
        }
    }
    return gray;
}

const uint8_t* HalDisplay::getGrayscale16Buffer() const { return s_grayBuffer; }

bool HalDisplay::commitGrayscale16(bool clean) {
    if (s_grayBuffer == nullptr) return false;
    pushGrayBuffer(clean);
    return true;
}

bool HalDisplay::commitGrayscale16Area(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool clean) {
    // The vendor epdiy area path drives the whole panel anyway (data masking
    // only); reuse the full-sheet commit so the result is always correct.
    return commitGrayscale16(clean);
}

bool HalDisplay::commitGrayscale16Areas(const freeink::GrayArea*, int, bool clean) {
    return commitGrayscale16(clean);
}

void HalDisplay::cancelGrayscale16() {
    // Keep the PSRAM block (see s_grayBuffer); only the caller's pointer is
    // invalidated by GfxRenderer itself.
}

uint16_t HalDisplay::getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }

uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }

// Port helper: did the last panel push get a working EPD PMIC?
extern "C" bool plus_last_push_tps_ok() { return s_lastPushTpsOk; }
