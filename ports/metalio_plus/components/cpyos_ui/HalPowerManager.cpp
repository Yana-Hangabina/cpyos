#include "HalPowerManager.h"

#include <driver/gpio.h>
#include <esp_attr.h>

#include <cstdio>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "BackgroundWork.h"
#include "HalGPIO.h"
#include "PortRadioState.h"
#include "config.h"
#include "cpyos_sd.h"
#include "epd_board_metalio_eink4_plus.h"
#include "epdiy.h"
#include "plus_battery.h"
#include "plus_battery_filter.h"
// Included last: HalStorage.h defines the `Storage` singleton macro.
#include "HalStorage.h"

namespace {

constexpr const char* TAG = "PLUS-PWR";
plus_battery_sample_t s_sample{};
unsigned long s_lastPollMs = 0;
plus_battery::PercentFilter s_percent;
bool s_lowWarned = false;

uint8_t s_lastFailure = static_cast<uint8_t>(HalPowerManager::LightSleepFailure::None);
int s_lastError = 0;
volatile uint32_t s_wakeSeq = 0;
volatile uint8_t s_wakeReason = 0;
bool s_debugAllowUsbSleep = false;

// Survives a panic reset but not a power loss. The shell reads it on the next
// boot: a stage number means the panic happened inside the park/sleep/restore
// sequence, which is otherwise invisible because the USB console is parked.
RTC_NOINIT_ATTR uint32_t s_sleepStage;
// Set when lightSleepFor() returns through its success path. The next boot
// turns it into a power.log line even if the activity panicked before any SD
// write could happen; a panic inside the park leaves it 0 and the stage instead.
RTC_NOINIT_ATTR uint32_t s_pendingWake;
RTC_NOINIT_ATTR uint32_t s_pendingWakeReason;

void setStage(const uint32_t stage) { s_sleepStage = stage; }

void noteWake(const HalPowerManager::LightSleepWakeReason reason) {
    s_wakeReason = static_cast<uint8_t>(reason);
    s_wakeSeq = s_wakeSeq + 1;
}

// Appended from the UI task after the board is restored, before control returns
// to the activity: a panic in that activity can no longer erase the evidence
// that the sleep/wake itself completed.
bool appendWakeLine(const HalPowerManager::LightSleepWakeReason reason) {
    plus_battery_sample_t sample{};
    const bool have = PlusBattery_Read(&sample);
    char line[192];
    const int n = snprintf(line, sizeof(line), "wake reason=%u stage=%u ms=%lu mv=%d ma=%d soc=%d chg=%d\n",
                           static_cast<unsigned>(reason), static_cast<unsigned>(s_sleepStage),
                           static_cast<unsigned long>(millis()), have ? sample.millivolts : 0,
                           have ? sample.milliamps : 0, have ? sample.soc_percent : 0,
                           PlusBattery_IsCharging() ? 1 : 0);
    if (n <= 0) return false;
    FILE* f = fopen("/sdcard/.crosspoint/power.log", "a");
    if (f != nullptr) {
        fwrite(line, 1, static_cast<size_t>(n), f);
        fclose(f);
        return true;
    }
    // stdio can be unavailable in this component's link order; the storage
    // wrapper uses the same FAT volume and can append directly.
    HalFile h = Storage.open("/.crosspoint/power.log", O_WRONLY | O_CREAT | O_APPEND);
    if (!h) return false;
    const bool ok = h.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n)) == n;
    h.close();
    return ok;
}

void recordFailure(HalPowerManager::LightSleepFailure code, esp_err_t err) {
    s_lastFailure = static_cast<uint8_t>(code);
    s_lastError = static_cast<int>(err);
}

#if defined(CROSSPOINT_PLUS_PORT)

// Hardware that was parked before esp_light_sleep_start(). Every bit has a
// matching undo, and the undo runs in the vendor order on any failure path so a
// refused sleep never leaves the panel biased or the card unmounted.
enum : unsigned {
    kParkedSd = 1u << 0,
    kParkedTps = 1u << 1,
    kParkedBus = 1u << 2,
    kParkedEpd = 1u << 3,
    kMaskedKeys = 1u << 4,
    kWakePin = 1u << 5,
    kArmedWake = 1u << 6,
    kCutRails = 1u << 7,
    kLatchedBus = 1u << 8,
};

// Wake sources live in the always-on domain: map the 32-bit mask of the one
// pin we arm.
constexpr uint64_t kIntLineMask = 1ULL << static_cast<unsigned>(IO_EXPANDER_INT_GPIO);

// Vendor ordering: keys first (the expander must be back before any other I2C
// user), then the P14->P5 rails, then the EPD bus/LCD, then the card. Each step
// is attempted even when an earlier one failed, because a half-restored board is
// worse than a logged second failure.
bool restoreParkedHardware(unsigned applied, esp_err_t* firstError) {
    esp_err_t first = ESP_OK;
    auto note = [&first](esp_err_t err) {
        if (err != ESP_OK && first == ESP_OK) first = err;
    };

    if ((applied & (kMaskedKeys | kWakePin | kArmedWake)) != 0) {
        note(epd_board_metalio_eink4_plus_ioexp_restore_keys());
    }
    if ((applied & kCutRails) != 0) {
        note(epd_board_metalio_eink4_plus_sleep_restore_rails());
    }
    if ((applied & (kParkedBus | kLatchedBus)) != 0) {
        epd_board_metalio_eink4_plus_bus_restore();
    }
    if ((applied & kParkedSd) != 0) {
        if (!HalStorage::getInstance().restoreAfterLightSleep()) first = ESP_FAIL;
    }
    if (firstError != nullptr) *firstError = first;
    return first == ESP_OK;
}

// The expander INT# is open-drain (wired-OR with the accelerometer). Vendor
// policy: configure it as a pulled-up input and require it to settle high
// before arming EXT1 ANY_LOW, otherwise the wake source is already asserted and
// esp_light_sleep_start() would return immediately (or be rejected).
bool waitIntLineHigh() {
    gpio_set_direction(IO_EXPANDER_INT_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(IO_EXPANDER_INT_GPIO);
    gpio_pulldown_dis(IO_EXPANDER_INT_GPIO);
    for (int i = 0; i < 30; ++i) {
        if (gpio_get_level(IO_EXPANDER_INT_GPIO) != 0) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "GPIO%d still low after masking the expander", static_cast<int>(IO_EXPANDER_INT_GPIO));
    return false;
}

// Latched at the instant the CPU returns, i.e. while the rails are still dead
// and the expander still has P1.7 configured as an input. A press/release that
// happens within the wake latency is missed, which is why three samples are
// taken ~10 ms apart: the vendor firmware also treats a momentarily-low P1.7 as
// "the power key woke us".
bool readPowerKeyLowAtWake() {
    constexpr int kPort = EPD_IO_POWER >> 3;
    constexpr uint8_t kBit = static_cast<uint8_t>(1u << (EPD_IO_POWER & 7));
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t port = 0xFF;
        if (epd_board_metalio_eink4_plus_ioexp_read_port(kPort, &port) && (port & kBit) == 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

#endif  // CROSSPOINT_PLUS_PORT

}  // namespace

HalPowerManager powerManager;

void HalPowerManager::begin() {
    plus_battery_sample_t sample{};
    if (PlusBattery_Read(&sample)) {
        s_sample = sample;
        s_lastPollMs = millis();
        ESP_LOGI(TAG, "BQ27220 %d mV %d mA %d%%", sample.millivolts, sample.milliamps,
                 sample.soc_percent);
    } else {
        ESP_LOGW(TAG, "BQ27220 not answering; battery percentage unavailable");
    }
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const { gpio.startDeepSleep(); }

void HalPowerManager::setPowerSaving(bool enable) {
    // DFS is not available on this SoC build: esp_pm_configure accepts
    // 320/240 but the clock never leaves 320 (see BRINGUP 2026-11-12), so the
    // port keeps a fixed 320 MHz and this stays a no-op.
    (void)enable;
}

uint16_t HalPowerManager::getBatteryPercentage() const {
    if (millis() - s_lastPollMs >= BATTERY_POLL_MS) {
        plus_battery_sample_t sample{};
        if (PlusBattery_Read(&sample)) {
            s_sample = sample;
        }
        s_lastPollMs = millis();
    }
    if (!s_sample.valid) {
        return 100;  // unknown: show full rather than a false empty warning
    }
    // The BQ27220 reports a real state of charge. The voltage curve is only a
    // fallback: Li-ion sits between 3.7 and 4.0 V for most of its range, so a
    // millivolt-derived percentage reads ~100 % until the pack is nearly empty.
    const int raw = (s_sample.soc_percent >= 0 && s_sample.soc_percent <= 100)
                        ? s_sample.soc_percent
                        : PlusBattery_PercentFromMillivolts(s_sample.millivolts);
    // Vendor firmware applies a dead band to its voltage-derived SOC; the same
    // idea keeps the status bar from flickering, and a critical pack still warns
    // once (the vendor cuts the BATFET at 3.1 V; this port has no such cutoff
    // yet, so the warning is the safety net).
    const bool charging = PlusBattery_IsCharging();
    if (raw <= 3 && !charging) {
        if (!s_lowWarned) {
            s_lowWarned = true;
            ESP_LOGE(TAG, "battery critical: %d%% (%d mV); connect power", raw, s_sample.millivolts);
        }
    } else if (raw > 8) {
        s_lowWarned = false;
    }
    return static_cast<uint16_t>(s_percent.update(raw));
}

uint8_t HalPowerManager::lastLightSleepFailure() { return s_lastFailure; }

int HalPowerManager::lastLightSleepError() { return s_lastError; }

uint32_t HalPowerManager::wakeEventSequence() { return s_wakeSeq; }

uint8_t HalPowerManager::lastWakeReason() { return s_wakeReason; }

uint32_t HalPowerManager::sleepStage() { return s_sleepStage; }

void HalPowerManager::clearSleepStage() { s_sleepStage = 0; }

bool HalPowerManager::pendingWake(uint32_t* stage, uint8_t* reason) {
    if (s_pendingWake == 0) return false;
    if (stage != nullptr) *stage = s_sleepStage;
    if (reason != nullptr) *reason = static_cast<uint8_t>(s_pendingWakeReason);
    s_pendingWake = 0;
    return true;
}

void HalPowerManager::setDebugAllowUsbSleep(const bool allow) { s_debugAllowUsbSleep = allow; }

bool HalPowerManager::debugUsbSleepAllowed() { return s_debugAllowUsbSleep; }

bool HalPowerManager::lightSleep(const HalGPIO& gpio) const {
    // There is no "nap for a few milliseconds" primitive on this board: the TCA
    // INT line is shared with the accelerometer, the panel PMIC must be parked,
    // and the SD card has to be unmounted first. Everything therefore goes
    // through lightSleepFor(); reporting success here would only hide a caller
    // that never parks anything.
    (void)gpio;
    recordFailure(LightSleepFailure::NotPorted, 0);
    return false;
}

bool HalPowerManager::canStandbyLightSleep(const HalGPIO& gpio) const {
#if defined(CROSSPOINT_PLUS_PORT)
    // Fail closed: every condition below is a state in which the park sequence
    // would either be pointless (USB power keeps the console alive and the rails
    // irrelevant) or unsafe (a live radio/worker/boot transaction touching I2C,
    // SPI or the card while the expander and the bus are masked).
    if (!epd_board_metalio_eink4_plus_ioexp_ok()) return false;  // no wake source
    if (epd_current_board() == nullptr) return false;
    if (gpio.isUsbConnected() && !s_debugAllowUsbSleep) return false;
    if (g_portRadioBusy) return false;  // radio bring-up/teardown in flight
    if (background_work::active()) return false;  // cache/comment/download worker
    // SD: either there is no card (nothing to quiesce) or the storage lock must
    // be free right now, i.e. no FAT transaction is in flight. Open handles are
    // parked and reopened by quiesceForLightSleep(), so reading a book no longer
    // blocks standby sleep.
    return HalStorage::getInstance().canParkForLightSleep();
#else
    (void)gpio;
    return false;
#endif
}

HalPowerManager::LightSleepWakeReason HalPowerManager::lightSleepFor(uint32_t seconds) const {
#if !defined(CROSSPOINT_PLUS_PORT)
    (void)seconds;
    return LightSleepWakeReason::Failed;
#else
    recordFailure(LightSleepFailure::None, 0);
    s_pendingWake = 0;
    if (seconds == 0) {
        recordFailure(LightSleepFailure::NotAllowed, 0);
        return LightSleepWakeReason::Failed;
    }
    if (!epd_board_metalio_eink4_plus_ioexp_ok() || epd_current_board() == nullptr) {
        recordFailure(LightSleepFailure::NotAllowed, ESP_ERR_INVALID_STATE);
        return LightSleepWakeReason::Failed;
    }

    unsigned applied = 0;
    // Restores whatever was parked and reports Failed. Used only before the CPU
    // actually sleeps: after a successful wake the restore runs on its own so a
    // single failing step still gets logged with its own code.
    auto abortPark = [&applied](LightSleepFailure code, esp_err_t err) {
        esp_err_t restoreErr = ESP_OK;
        restoreParkedHardware(applied, &restoreErr);
        recordFailure(code, err);
        noteWake(LightSleepWakeReason::Failed);
        appendWakeLine(LightSleepWakeReason::Failed);
        ESP_LOGW(TAG, "light sleep refused at %u (err=%d, restore=%d)", static_cast<unsigned>(code),
                 static_cast<int>(err), static_cast<int>(restoreErr));
        return LightSleepWakeReason::Failed;
    };

    // 1. SD quiescence: flush + close every open file under the storage lock.
    //    The handles are reopened at their saved offsets on the way back, so
    //    an open book no longer prevents sleep. Failing here means the card
    //    cannot be proven safe, so never sleep.
    setStage(1);
    if (!HalStorage::getInstance().quiesceForLightSleep()) {
        return abortPark(LightSleepFailure::SdPark, ESP_FAIL);
    }
    applied |= kParkedSd;

    // 2. Panel PMIC to SLEEP, then park the parallel bus (vendor order).
    setStage(2);
    esp_err_t err = epd_board_metalio_eink4_plus_tps_enter_sleep();
    if (err != ESP_OK) return abortPark(LightSleepFailure::TpsSleep, err);
    applied |= kParkedTps;

    setStage(3);
    epd_board_metalio_eink4_plus_bus_hold_low();
    applied |= kParkedBus;

    // 3. The epdiy power-off the display driver itself uses at the end of every
    //    push: keeps VCOM/PWRUP de-asserted while the rails go away.
    setStage(4);
    epd_poweroff();
    applied |= kParkedEpd;

    // 4. Expander: TPS pins low, keys idle high, P1.7 sampled as an input.
    setStage(5);
    err = epd_board_metalio_eink4_plus_ioexp_mask_for_sleep();
    if (err != ESP_OK) return abortPark(LightSleepFailure::IoExpMask, err);
    applied |= kMaskedKeys;

    setStage(6);
    if (!epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin()) {
        return abortPark(LightSleepFailure::PowerWakePin, ESP_ERR_INVALID_STATE);
    }
    applied |= kWakePin;

    setStage(7);
    if (!waitIntLineHigh()) return abortPark(LightSleepFailure::IntLineLow, ESP_ERR_TIMEOUT);

    // 5. Wake sources: EXT1 ANY_LOW on the expander INT plus the timer.
    setStage(8);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (!esp_sleep_is_valid_wakeup_gpio(IO_EXPANDER_INT_GPIO)) {
        return abortPark(LightSleepFailure::WakeConfig, ESP_ERR_INVALID_ARG);
    }
    err = esp_sleep_enable_ext1_wakeup_io(kIntLineMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) return abortPark(LightSleepFailure::WakeConfig, err);
    setStage(9);
    err = esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
    if (err != ESP_OK) return abortPark(LightSleepFailure::TimerConfig, err);
    applied |= kArmedWake;

    // 6. Cut the shared peripheral rails (P5 then P14) and latch the parked EPD
    //    pads. Ordering and registration of the restore responsibility follow
    //    the vendor policy: mark first, then cut, because a failed write may
    //    already have changed some outputs.
    setStage(10);
    applied |= kCutRails;
    err = epd_board_metalio_eink4_plus_sleep_cut_rails();
    if (err != ESP_OK) return abortPark(LightSleepFailure::RailCut, err);
    epd_board_metalio_eink4_plus_bus_latch_for_sleep();
    applied |= kLatchedBus;

    // 7. Sleep. The UI task owns this call (never the render task): the park
    //    sequence drives I2C/GPIO directly.
    setStage(11);
    const esp_err_t sleepErr = esp_light_sleep_start();
    setStage(12);
    // Bitmap form: esp_sleep_get_wakeup_cause() is deprecated in IDF 6.x.
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    const bool wokeOnExt1 = (causes & (1u << ESP_SLEEP_WAKEUP_EXT1)) != 0;

    // 8. Wake. P1.7 must be sampled before ioexp_restore_keys() re-configures
    //    the key ports, and while the rails are still dead. kiikoread requires
    //    two low samples 30 ms apart before inheriting a key: a single sample of
    //    a released contact can read low inside the wake latency.
    bool powerKeyLow = readPowerKeyLowAtWake();
    if (powerKeyLow) {
        vTaskDelay(pdMS_TO_TICKS(30));
        powerKeyLow = readPowerKeyLowAtWake();
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    esp_err_t restoreErr = ESP_OK;
    const bool restored = restoreParkedHardware(applied, &restoreErr);
    if (sleepErr != ESP_OK) {
        recordFailure(LightSleepFailure::SleepStart, sleepErr);
        noteWake(LightSleepWakeReason::Failed);
        appendWakeLine(LightSleepWakeReason::Failed);
        ESP_LOGW(TAG, "light sleep returned %s", esp_err_to_name(sleepErr));
        return LightSleepWakeReason::Failed;
    }
    if (!restored) {
        recordFailure(LightSleepFailure::Restore, restoreErr);
        noteWake(LightSleepWakeReason::Failed);
        appendWakeLine(LightSleepWakeReason::Failed);
        ESP_LOGE(TAG, "wake restore failed (%s); SD/rails may be incomplete",
                 esp_err_to_name(restoreErr));
        return LightSleepWakeReason::Failed;
    }
    setStage(13);

    // A bare GPIO2 wake is NOT a power key: the accelerometer shares that line,
    // and a key released inside the wake latency reads high here. Only an EXT1
    // wake with P1.7 actually low is reported as PowerButton.
    if (wokeOnExt1 && powerKeyLow) {
        recordFailure(LightSleepFailure::None, 0);
        noteWake(LightSleepWakeReason::PowerButton);
        setStage(appendWakeLine(LightSleepWakeReason::PowerButton) ? 15 : 14);
        s_pendingWake = 1;
        s_pendingWakeReason = static_cast<uint32_t>(LightSleepWakeReason::PowerButton);
        ESP_LOGI(TAG, "light sleep woke on the power key");
        return LightSleepWakeReason::PowerButton;
    }
    recordFailure(LightSleepFailure::None, 0);
    noteWake(LightSleepWakeReason::Timer);
    setStage(appendWakeLine(LightSleepWakeReason::Timer) ? 15 : 14);
    s_pendingWake = 1;
    s_pendingWakeReason = static_cast<uint32_t>(LightSleepWakeReason::Timer);
    ESP_LOGI(TAG, "light sleep woke on %s", wokeOnExt1 ? "GPIO2 (not the key)" : "timer");
    return LightSleepWakeReason::Timer;
#endif
}

HalPowerManager::Lock::Lock(bool /*quiet*/) {}
HalPowerManager::Lock::~Lock() {}
