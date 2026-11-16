// Metalio E-INK4 Plus input backend.
//
// The Plus has no GPIO buttons: navigation comes from the FT6336U two-point
// panel plus three capacitive zones in the strip of the touch panel BELOW the
// drawn image (vendor config.h: TOUCH_VK_Y 1600, centres x = 100/200/300).
//
// The vendor touch driver already publishes a PORTRAIT 684x1216 logical point
// (`metalio_touch_read`), so this file only has to translate that into the
// normalized PANEL frame CPYOS's GfxRenderer::tapToLogical expects and run the
// same tap / long-press / swipe classifier the SDK InputManager uses on the
// reference board (28 px tap slop, 60 px swipe, 500 ms long press).
//
// Mapping of the three cover keys follows the reference Read Pico strip
// (left -> Up/PageBack, middle -> Back, right -> Down/PageForward). The Plus
// silkscreen labels them home / prev / next, so the physical order differs; the
// logical actions are the ones every activity already understands.
//
// UNTESTED ON HARDWARE: the coordinates below come from the vendor driver and
// the reference board's transform, not from a measurement on a Plus unit.

#include "HalGPIO.h"

#include <Arduino.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <soc/usb_serial_jtag_reg.h>

#include <cmath>

#include "metalio_i2c1.h"
#include "metalio_touch.h"
#include "touch_map.h"
#include "plus_battery.h"
#include "plus_charger.h"
extern "C" {
#include "epd_board_metalio_eink4_plus.h"
#include "pca9555.h"
}

namespace {

constexpr const char* TAG = "PLUS-GPIO";

// Cover-key zones (vendor config.h). A real screen contact never reaches this
// raw Y, so this cannot swallow a normal touch.
constexpr int kVkRawY = 1600;
constexpr int kVkTol = 30;
constexpr int kVkHomeX = 100;
constexpr int kVkPrevX = 200;
constexpr int kVkNextX = 300;

constexpr int kTapSlopPx = 28;
constexpr int kSwipeMinPx = 60;
constexpr unsigned long kLongPressMs = 500;
constexpr unsigned long kSwipeMaxMs = 700;

// Motor pulse lengths by CPYOS haptic level (off/weak/medium/strong), taken from
// the vendor constants (main/haptic_feedback.cc: 35/50/70 ms). The vendor drives
// those pulses at 70 % soft-PWM on TCA9555 P0.3; this port holds the line steady
// until the motor polarity/feel is verified on hardware. Keep the 100 ms gap.
constexpr uint16_t kHapticPulseMs[] = {0, 35, 50, 70};
constexpr unsigned long kHapticMinGapMs = 100;

esp_timer_handle_t hapticTimer = nullptr;
unsigned long hapticLastMs = 0;

void hapticTimerExpired(void*) { epd_board_metalio_eink4_plus_motor_set(false); }

float dist2(int ax, int ay, int bx, int by) {
    const float dx = static_cast<float>(ax - bx);
    const float dy = static_cast<float>(ay - by);
    return dx * dx + dy * dy;
}

// A connected USB host advances the Serial/JTAG SOF frame counter at 1 kHz.
// The reference board uses the same probe; keep the last positive result for
// 1.5 s so a busy loop cannot flicker the charging indicator.
bool usbHostSofActive() {
    static uint32_t lastFrame = 0;
    static unsigned long lastAdvanceMs = 0;
    static bool seeded = false;
    if (!seeded) {
        seeded = true;
        lastFrame = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
        delay(3);
    }
    const uint32_t frame = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
    if (frame != lastFrame) {
        lastFrame = frame;
        lastAdvanceMs = millis();
        return true;
    }
    return lastAdvanceMs != 0 && millis() - lastAdvanceMs < 1500;
}

// Charge-only sources never advance the SOF counter; the gauge's signed
// current covers them. Sampled at 1 Hz so the input loop does not poll I2C.
bool gaugeCharging() {
    static unsigned long sampledAt = 0;
    static bool charging = false;
    const unsigned long now = millis();
    if (sampledAt != 0 && now - sampledAt < 1000) return charging;
    sampledAt = now;
    plus_charger_sample_t charger{};
    const bool chargerPresent = PlusCharger_Read(&charger) && charger.valid && charger.vbus != 0;
    charging = chargerPresent || PlusBattery_IsCharging();
    return charging;
}

int nearestKey(int rawX) {
    if (std::abs(rawX - kVkHomeX) <= kVkTol) return 0;
    if (std::abs(rawX - kVkPrevX) <= kVkTol) return 1;
    if (std::abs(rawX - kVkNextX) <= kVkTol) return 2;
    return -1;
}

}  // namespace

HalGPIO gpio;

bool HalGPIO::isXteinkDevice() const { return false; }
bool HalGPIO::hasEdgeSideButtons() const { return false; }

void HalGPIO::begin() {
    // Leave the motor off and create its one-shot timer before any key edge.
    epd_board_metalio_eink4_plus_motor_set(false);
    if (!hapticTimer) {
        esp_timer_create_args_t args = {};
        args.callback = hapticTimerExpired;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "plus_haptic";
        if (esp_timer_create(&args, &hapticTimer) != ESP_OK) {
            hapticTimer = nullptr;
            ESP_LOGE(TAG, "haptic timer allocation failed");
        }
    }
    if (!metalio_touch_ok()) {
        if (metalio_touch_init()) {
            ESP_LOGI(TAG, "FT6336U ready; cover keys at raw y=%d x=%d/%d/%d", kVkRawY, kVkHomeX,
                     kVkPrevX, kVkNextX);
        } else {
            ESP_LOGE(TAG, "FT6336U did not answer; touch input unavailable");
        }
    }
}

void HalGPIO::updateHapticFeedback(uint8_t level) {
    if (level >= sizeof(kHapticPulseMs) / sizeof(kHapticPulseMs[0])) level = 2;
    const uint16_t duration = kHapticPulseMs[level];
    if (duration == 0) {
        stopHapticFeedback();
        return;
    }
    // Only a fresh press edge pulses; held keys must not buzz every frame.
    if (pressedMask_ == 0 && !touchDownEvent_) return;
    if (!hapticTimer) return;
    const unsigned long now = millis();
    if (hapticLastMs != 0 && now - hapticLastMs < kHapticMinGapMs) return;
    hapticLastMs = now;
    esp_timer_stop(hapticTimer);
    epd_board_metalio_eink4_plus_motor_set(true);
    if (esp_timer_start_once(hapticTimer, static_cast<uint64_t>(duration) * 1000ULL) != ESP_OK) {
        epd_board_metalio_eink4_plus_motor_set(false);
    }
}

void HalGPIO::pulseHaptic(uint16_t durationMs) {
    if (!hapticTimer || durationMs == 0) return;
    hapticLastMs = millis();
    esp_timer_stop(hapticTimer);
    epd_board_metalio_eink4_plus_motor_set(true);
    if (esp_timer_start_once(hapticTimer, static_cast<uint64_t>(durationMs) * 1000ULL) != ESP_OK) {
        epd_board_metalio_eink4_plus_motor_set(false);
    }
}

void HalGPIO::stopHapticFeedback() {
    if (hapticTimer) esp_timer_stop(hapticTimer);
    epd_board_metalio_eink4_plus_motor_set(false);
}

void HalGPIO::beginFrame() {
    pressedMask_ = 0;
    releasedMask_ = 0;
    volUpPressed_ = false;
    volUpReleased_ = false;
    volDownPressed_ = false;
    volDownReleased_ = false;
    touchDownEvent_ = false;
    touchReleasedEvent_ = false;
    touchLongPressEvent_ = false;
    touchTapEvent_ = false;
    swipeEvent_ = false;
    usbStateChanged_ = false;

    // Apply one queued injection per frame (console-driven UI walks).
    if (pendingReleaseKey_ >= 0) {
        releasedMask_ |= static_cast<uint8_t>(1u << pendingReleaseKey_);
        pendingReleaseKey_ = -1;
    }
    if (pendingKey_ >= 0) {
        pressedMask_ |= static_cast<uint8_t>(1u << pendingKey_);
        pendingReleaseKey_ = pendingKey_;
        pendingKey_ = -1;
    }
    if (pendingInjectDown_) {
        pendingInjectDown_ = false;
        injectedDown_ = true;
        injectedLongFired_ = false;
        suppressed_ = false;
        moved_ = false;
        longFired_ = false;
        downMs_ = millis();
        downLx_ = lastLx_ = pendingInjectLx_;
        downLy_ = lastLy_ = pendingInjectLy_;
        screenDown_ = true;
        touchDownEvent_ = true;
    } else if (injectedDown_ && !injectedLongFired_ && millis() - downMs_ >= kLongPressMs) {
        injectedLongFired_ = true;
        touchLongPressEvent_ = true;
    }
    if (pendingInjectUp_) {
        pendingInjectUp_ = false;
        if (injectedDown_) {
            injectedDown_ = false;
            screenDown_ = false;
            // A long press was already delivered while held; only a short hold
            // additionally reports a tap, like the hardware path.
            if (!suppressed_ && !moved_ && !injectedLongFired_) touchTapEvent_ = true;
            touchReleasedEvent_ = !suppressed_;
            suppressed_ = false;
        }
    }
    if (pendingTapLx_ >= 0) {
        downLx_ = lastLx_ = pendingTapLx_;
        downLy_ = lastLy_ = pendingTapLy_;
        downMs_ = millis();
        screenDown_ = true;
        touchDownEvent_ = true;
        touchReleasedEvent_ = true;
        touchTapEvent_ = true;
        pendingTapLx_ = pendingTapLy_ = -1;
    }
    if (pendingSwipe_[0] >= 0) {
        swipeEvent_ = true;
        swipeLx0_ = pendingSwipe_[0];
        swipeLy0_ = pendingSwipe_[1];
        swipeLx1_ = pendingSwipe_[2];
        swipeLy1_ = pendingSwipe_[3];
        pendingSwipe_[0] = -1;
    }
}

void HalGPIO::debugInjectTap(int lx, int ly) {
    pendingTapLx_ = lx;
    pendingTapLy_ = ly;
}

void HalGPIO::debugInjectKey(uint8_t buttonIndex) {
    pendingKey_ = buttonIndex;
}

void HalGPIO::debugInjectSwipe(int x0, int y0, int x1, int y1) {
    pendingSwipe_[0] = x0;
    pendingSwipe_[1] = y0;
    pendingSwipe_[2] = x1;
    pendingSwipe_[3] = y1;
}

void HalGPIO::update() {
    const unsigned long now = millis();

    // PMU power key: TCA9555 P1.7, pulled up, pressed = low. Reading the input
    // port also clears the expander's INT latch. The vendor wiring documents
    // this pin; the active level still needs an on-hardware check.
    // P1.7 = power, P1.0/P1.1 = volume keys (active low, vendor decode:
    // P1.0 -> previous, P1.1 -> next). A failed read must not look like a
    // wall of pressed keys.
    bool powerKey = false;
    bool volPrev = false;
    bool volNext = false;
    // pca9555_read_input_ex() takes the I2C1 mutex itself. Do not wrap it in
    // another metalio_i2c1_lock(): the mutex is not recursive, so the inner
    // take would time out after 100 ms on every frame and keys would never
    // update (observed on the first hardware boot).
    uint8_t port1 = 0;
    if (pca9555_read_input_ex(I2C_NUM_1, 1, &port1) == ESP_OK) {
        if (port1 == port1Raw_) {
            if (port1StableCount_ < 2) ++port1StableCount_;
            if (port1StableCount_ >= 2) port1Stable_ = port1;
        } else {
            port1Raw_ = port1;
            port1StableCount_ = 0;
        }
    }
    // Use the debounced snapshot. A failed read keeps the last stable value
    // instead of reporting "all keys released", which is what produced the
    // spurious power release.
    powerKey = (port1Stable_ & 0x80) == 0;
    volPrev = (port1Stable_ & 0x01) == 0;
    volNext = (port1Stable_ & 0x02) == 0;

    // Volume-key edges for the shell. They are only reported while navigation
    // is off; otherwise the same keys are BTN_UP/BTN_DOWN (vendor rule: they
    // turn pages in the reader and change the volume everywhere else).
    volUpNow_ = volPrev;
    volDownNow_ = volNext;
    if (!sideKeysNavigate_) {
        if (volUpNow_ && !volUpPrev_) volUpPressed_ = true;
        if (!volUpNow_ && volUpPrev_) volUpReleased_ = true;
        if (volDownNow_ && !volDownPrev_) volDownPressed_ = true;
        if (!volDownNow_ && volDownPrev_) volDownReleased_ = true;
    }
    volUpPrev_ = volUpNow_;
    volDownPrev_ = volDownNow_;

    const bool usbNow = usbHostSofActive() || gaugeCharging();
    if (usbNow != usbConnected_) {
        usbConnected_ = usbNow;
        usbStateChanged_ = true;
    }

    metalio_touch_point_t pt{};
    const bool contact = metalio_touch_read(&pt);

    // Two-point snapshot for pinch-to-zoom (reader font size).
    metalio_touch_pair_t pair{};
    if (metalio_touch_read_pair(&pair, &pairSeq_)) {
        plus_touch_pair::Pair sample{pair.down, pair.x1, pair.y1, pair.x2, pair.y2};
        const auto pinch = plus_touch_pair::evaluate(
            sample, pinchState_, plus_touch::kPortraitWidth, plus_touch::kPanelWidth);
        if (pinch.fire) {
            pinchGesture_ = true;
            pinchPending_ = true;
            pinchScale_ = pinch.scale;
            pinchCx_ = pinch.cx;
            pinchCy_ = pinch.cy;
        }
    }

    // --- cover keys ---------------------------------------------------------
    int key = -1;
    if (contact && pt.raw_y >= kVkRawY - kVkTol) {
        key = nearestKey(pt.raw_x);
    }
    int button = -1;
    if (key == 0) button = BTN_BACK;  // home / leave this level
    if (key == 1) button = BTN_UP;    // prev / page back
    if (key == 2) button = BTN_DOWN;  // next / page forward
    // Side volume keys act as Up/Down only where the shell asks for it (the
    // reader); elsewhere they drive the speaker volume.
    if (button < 0 && sideKeysNavigate_ && volPrev) button = BTN_UP;
    if (button < 0 && sideKeysNavigate_ && volNext) button = BTN_DOWN;
    if (powerKey) button = BTN_POWER;

    if (button != heldButton_) {
        if (heldButton_ >= 0) releasedMask_ |= static_cast<uint8_t>(1u << heldButton_);
        if (button >= 0) {
            pressedMask_ |= static_cast<uint8_t>(1u << button);
            heldSince_ = now;
        }
        heldButton_ = button;
    }

    // A cover-key contact must not also be reported as a screen touch.
    const bool screen = contact && button < 0;

    // --- screen touch -------------------------------------------------------
    if (injectedDown_) {
        // Console hold in progress: the injection owns the touch state.
    } else if (screen && !screenDown_) {
        screenDown_ = true;
        suppressed_ = false;
        tapSuppressForContact_ = false;
        moved_ = false;
        longFired_ = false;
        downMs_ = now;
        downLx_ = lastLx_ = pt.x;
        downLy_ = lastLy_ = pt.y;
        touchDownEvent_ = true;
    } else if (screen) {
        lastLx_ = pt.x;
        lastLy_ = pt.y;
        if (dist2(pt.x, pt.y, downLx_, downLy_) >
            static_cast<float>(kTapSlopPx) * static_cast<float>(kTapSlopPx)) {
            moved_ = true;
        }
        if (!longFired_ && !suppressed_ && !tapSuppressForContact_ && !moved_ && now - downMs_ >= kLongPressMs) {
            longFired_ = true;
            touchLongPressEvent_ = true;
        }
    } else if (!screen && screenDown_) {
        screenDown_ = false;
        const bool pinched = pinchGesture_;
        pinchGesture_ = false;
        lastHeldMs_ = now - downMs_;
        const int dx = lastLx_ - downLx_;
        const int dy = lastLy_ - downLy_;
        const bool swipe = !pinched && dx * dx + dy * dy >= kSwipeMinPx * kSwipeMinPx &&
                           lastHeldMs_ <= kSwipeMaxMs;
        if (!suppressed_ && !tapSuppressForContact_ && swipe) {
            swipeEvent_ = true;
            swipeLx0_ = downLx_;
            swipeLy0_ = downLy_;
            swipeLx1_ = lastLx_;
            swipeLy1_ = lastLy_;
        } else if (!suppressed_ && !tapSuppressForContact_ && !pinched && !longFired_ && !moved_) {
            touchTapEvent_ = true;
        }
        touchReleasedEvent_ = !suppressed_;
        suppressed_ = false;
        tapSuppressForContact_ = false;
    }
}

void HalGPIO::setSideKeysNavigate(bool navigate) {
    if (navigate == sideKeysNavigate_) return;
    sideKeysNavigate_ = navigate;
    // Resync so the mode change itself cannot emit a volume edge.
    volUpPrev_ = volUpNow_;
    volDownPrev_ = volDownNow_;
    volUpPressed_ = volUpReleased_ = volDownPressed_ = volDownReleased_ = false;
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return heldButton_ == buttonIndex; }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
    return buttonIndex < 7 && (pressedMask_ & (1u << buttonIndex)) != 0;
}

bool HalGPIO::wasAnyPressed() const { return pressedMask_ != 0; }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
    return buttonIndex < 7 && (releasedMask_ & (1u << buttonIndex)) != 0;
}

bool HalGPIO::wasAnyReleased() const { return releasedMask_ != 0; }

unsigned long HalGPIO::getHeldTime() const {
    if (heldButton_ >= 0) return millis() - heldSince_;
    return lastHeldMs_;
}

unsigned long HalGPIO::getPowerButtonHeldTime() const { return isPressed(BTN_POWER) ? getHeldTime() : 0; }

bool HalGPIO::rawInputActive() { return screenDown_ || heldButton_ >= 0 || metalio_touch_has_pending_edges(); }

bool HalGPIO::hasTouch() const { return true; }
bool HalGPIO::hasHomeKey() const { return false; }
bool HalGPIO::wasHomeKeyPressed() const { return false; }
bool HalGPIO::wasHomeKeyTapped() const { return false; }
bool HalGPIO::wasHomeKeyLongPressed() const { return false; }

static bool coords(bool have, int lx, int ly, float& nx, float& ny) {
    if (!have) return false;
    plus_touch::portraitToPanel(lx, ly, nx, ny);
    return true;
}

void HalGPIO::debugInjectTouchDown(int lx, int ly) {
    pendingInjectLx_ = lx;
    pendingInjectLy_ = ly;
    pendingInjectDown_ = true;
}

void HalGPIO::debugInjectTouchUp() { pendingInjectUp_ = true; }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return coords(touchTapEvent_, downLx_, downLy_, nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return coords(touchDownEvent_, downLx_, downLy_, nx, ny); }

bool HalGPIO::wasTouchReleased() const { return touchReleasedEvent_; }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
    if (!screenDown_ || moved_ || suppressed_) return false;
    heldMs = millis() - downMs_;
    return heldMs < kLongPressMs && coords(true, downLx_, downLy_, nx, ny);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const {
    if (!screenDown_ || suppressed_) return false;
    return coords(true, lastLx_, lastLy_, nx, ny);
}

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const {
    return coords(touchLongPressEvent_, downLx_, downLy_, nx, ny);
}

void HalGPIO::suppressTouchContact() { suppressed_ = true; }

unsigned long HalGPIO::lastTouchHeldMs() const { return lastHeldMs_; }

bool HalGPIO::wasMultiTouchPinch(float& scale, float& nxCenter, float& nyCenter) const {
    if (!pinchPending_) return false;
    scale = pinchScale_;
    nxCenter = pinchCx_;
    nyCenter = pinchCy_;
    pinchPending_ = false;
    return true;
}

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
    if (!swipeEvent_) return false;
    plus_touch::portraitToPanel(swipeLx0_, swipeLy0_, nxStart, nyStart);
    plus_touch::portraitToPanel(swipeLx1_, swipeLy1_, nxEnd, nyEnd);
    return true;
}

bool HalGPIO::wasTouchActivity() const {
    return touchDownEvent_ || touchReleasedEvent_ || touchLongPressEvent_ || touchTapEvent_ || swipeEvent_;
}

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(bool) {}
bool HalGPIO::restoreTouchAfterDisplayReset() { return metalio_touch_ok() || metalio_touch_init(); }
bool HalGPIO::consumeSimulatorSleepRequest() { return false; }

void HalGPIO::startDeepSleep() {
    // The Plus has a TPS65185 rail and a deep-sleep-capable ESP32-S31, but the
    // vendor wakes on the TCA9555 INT line and the RTC alarm. That sequencing
    // is not implemented yet; refuse to sleep rather than leave the panel
    // biased. The power key still performs the vendor's hard power-off.
    ESP_LOGW(TAG, "deep sleep is not implemented on the Plus port yet; staying awake");
}

bool HalGPIO::verifyPowerButtonWakeup() { return true; }
bool HalGPIO::verifyPowerButtonWakeup(uint16_t, bool) { return true; }

bool HalGPIO::isUsbConnected() const { return usbConnected_; }
bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged_; }

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
    const esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_DEEPSLEEP) return WakeupReason::PowerButton;
    if (reason == ESP_RST_POWERON) return WakeupReason::AfterUSBPower;
    return WakeupReason::Other;
}
