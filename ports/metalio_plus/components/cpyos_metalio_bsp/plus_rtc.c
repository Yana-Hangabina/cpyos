#include "plus_rtc.h"

#include <esp_err.h>
#include <esp_log.h>

#include "metalio_i2c1.h"

#define PCF8563_ADDR 0x51
#define PCF8563_REG_CTRL1 0x00
#define PCF8563_REG_SECONDS 0x02

static const char *TAG = "PLUS-RTC";
static bool s_ok = false;

static uint8_t to_bcd(int value) { return (uint8_t)(((value / 10) << 4) | (value % 10)); }
static int from_bcd(uint8_t value) { return ((value >> 4) & 0x0F) * 10 + (value & 0x0F); }

bool PlusRtc_Read(struct tm *utc) {
    if (utc == nullptr) {
        return false;
    }
    if (!metalio_i2c1_lock(50)) {
        return false;
    }
    uint8_t regs[7] = {0};
    const esp_err_t err = metalio_i2c1_read_reg_stop_locked(
        I2C_NUM_1, PCF8563_ADDR, PCF8563_REG_SECONDS, regs, sizeof(regs), 50);
    metalio_i2c1_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF8563 read failed: %s", esp_err_to_name(err));
        return false;
    }
    if ((regs[0] & 0x80) != 0) {
        /* VL: the oscillator stopped, the calendar is not trustworthy. */
        ESP_LOGW(TAG, "PCF8563 reports VL (oscillator stopped)");
        return false;
    }

    struct tm value = {};
    value.tm_sec = from_bcd(regs[0] & 0x7F);
    value.tm_min = from_bcd(regs[1] & 0x7F);
    value.tm_hour = from_bcd(regs[2] & 0x3F);
    value.tm_mday = from_bcd(regs[3] & 0x3F);
    value.tm_wday = from_bcd(regs[4] & 0x07);
    value.tm_mon = from_bcd(regs[5] & 0x1F) - 1;
    value.tm_year = from_bcd(regs[6]) + 100;  /* 2000 + year - 1900 */
    value.tm_isdst = 0;

    if (value.tm_mon < 0 || value.tm_mon > 11 || value.tm_mday < 1 || value.tm_mday > 31 ||
        value.tm_hour > 23 || value.tm_min > 59 || value.tm_sec > 59) {
        ESP_LOGW(TAG, "PCF8563 returned an out-of-range calendar");
        return false;
    }
    *utc = value;
    s_ok = true;
    return true;
}

bool PlusRtc_Write(const struct tm *utc) {
    if (utc == nullptr) {
        return false;
    }
    uint8_t regs[7];
    regs[0] = (uint8_t)(to_bcd(utc->tm_sec) & 0x7F);  /* clears VL */
    regs[1] = to_bcd(utc->tm_min);
    regs[2] = to_bcd(utc->tm_hour);
    regs[3] = to_bcd(utc->tm_mday);
    regs[4] = (uint8_t)(utc->tm_wday & 0x07);
    regs[5] = to_bcd(utc->tm_mon + 1);  /* century bit stays 0: 2000-2099 */
    regs[6] = to_bcd(utc->tm_year % 100);

    if (!metalio_i2c1_lock(50)) {
        return false;
    }
    const esp_err_t err = metalio_i2c1_write_reg_locked(
        I2C_NUM_1, PCF8563_ADDR, PCF8563_REG_SECONDS, regs, sizeof(regs), 50);
    metalio_i2c1_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF8563 write failed: %s", esp_err_to_name(err));
        return false;
    }
    s_ok = true;
    return true;
}

bool PlusRtc_Ok(void) { return s_ok; }
