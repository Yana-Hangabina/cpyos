#include "plus_battery.h"

#include <config.h>
#include <esp_log.h>
#include <esp_err.h>

#include "metalio_i2c1.h"

#define BQ27220_ADDR 0x55
#define BQ27220_CMD_VOLTAGE 0x08 /* mV, unsigned */
#define BQ27220_CMD_CURRENT 0x0C /* mA, signed */
#define BQ27220_CMD_SOC 0x2C     /* percent, unsigned */

static const char *TAG = "PLUS-BAT";

/* SMBus read-word: little-endian, separator between the command and the data.
 * The shared bus helper supports exactly that STOP-between-reads shape. */
static bool read_word(uint8_t command, uint16_t *value) {
    if (!metalio_i2c1_lock(50)) {
        return false;
    }
    uint8_t buf[2] = {0, 0};
    const esp_err_t err = metalio_i2c1_read_reg_stop_locked(
        I2C_NUM_1, BQ27220_ADDR, command, buf, sizeof(buf), 50);
    metalio_i2c1_unlock();
    if (err != ESP_OK) {
        return false;
    }
    *value = (uint16_t)(buf[0] | (buf[1] << 8));
    return true;
}

bool PlusBattery_Read(plus_battery_sample_t *out) {
    if (out == nullptr) {
        return false;
    }
    out->valid = false;
    uint16_t voltage = 0;
    uint16_t current = 0;
    uint16_t soc = 0;
    if (!read_word(BQ27220_CMD_VOLTAGE, &voltage) ||
        !read_word(BQ27220_CMD_CURRENT, &current) ||
        !read_word(BQ27220_CMD_SOC, &soc)) {
        ESP_LOGW(TAG, "BQ27220 did not answer");
        return false;
    }
    out->millivolts = (int)voltage;
    out->milliamps = (int)(int16_t)current;
    out->soc_percent = (int)soc > 100 ? 100 : (int)soc;
    out->valid = out->millivolts > 2000 && out->millivolts < 5000;
    return out->valid;
}

bool PlusBattery_IsCharging(void) {
    uint16_t current = 0;
    if (!read_word(BQ27220_CMD_CURRENT, &current)) {
        return false;
    }
    return (int)(int16_t)current > 20;
}

int PlusBattery_PercentFromMillivolts(int millivolts) {
    if (millivolts <= 3300) {
        return 0;
    }
    if (millivolts >= 4300) {
        return 100;
    }
    return (millivolts - 3300) * 100 / 1000;
}
