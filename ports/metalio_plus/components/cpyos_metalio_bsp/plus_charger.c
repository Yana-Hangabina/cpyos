#include "plus_charger.h"

#include <esp_err.h>
#include <esp_log.h>

#include "metalio_i2c1.h"

#define CX25601N_ADDR 0x6B
#define CX25601N_REG_STATUS1 0x1E
#define CX25601N_REG_CHG_CTRL1 0x16

// The chip's OTP defaults charge, but its watchdog parks charging after a few
// hours and nothing on this board re-arms it. The reference board support runs
// a long register bring-up; the two bits that matter for a working charger are
// the watchdog enable (CHG_CTRL1 bits1:0) and the charge enable (bit5), with
// HIZ (bit4) cleared. Read-modify-write so the other bits keep the OTP values.
static bool s_begun = false;

static bool charger_begin_locked(void) {
    uint8_t ctrl = 0;
    if (metalio_i2c1_read_reg_stop_locked(I2C_NUM_1, CX25601N_ADDR, CX25601N_REG_CHG_CTRL1, &ctrl, 1, 50) !=
        ESP_OK) {
        return false;
    }
    const uint8_t next = (uint8_t)((ctrl & ~0x37u) | 0x20u); /* WDT 0, HIZ 0, EN_CHG 1 */
    if (next == ctrl) {
        return true;
    }
    return metalio_i2c1_write_reg_locked(I2C_NUM_1, CX25601N_ADDR, CX25601N_REG_CHG_CTRL1, &next, 1, 50) ==
           ESP_OK;
}

bool PlusCharger_Read(plus_charger_sample_t *out) {
    if (out == nullptr) return false;
    out->valid = false;
    if (!metalio_i2c1_lock(50)) return false;
    if (!s_begun) {
        s_begun = charger_begin_locked();
        if (s_begun) ESP_LOGI("PLUS-CHG", "CX25601N: watchdog off, charging enabled");
    }
    uint8_t status = 0;
    const esp_err_t err =
        metalio_i2c1_read_reg_stop_locked(I2C_NUM_1, CX25601N_ADDR, CX25601N_REG_STATUS1, &status, 1, 50);
    metalio_i2c1_unlock();
    if (err != ESP_OK) return false;
    out->vbus = status & 0x07;
    out->chg_stat = (status >> 3) & 0x03;
    out->valid = true;
    return true;
}

const char *PlusCharger_VbusName(uint8_t vbus) {
    switch (vbus) {
        case 0: return "none";
        case 1: return "USB-SDP";
        case 2: return "USB-CDP";
        case 3: return "USB-DCP";
        case 4: return "adapter?";
        case 5: return "non-standard";
        case 7: return "OTG";
        default: return "adapter";
    }
}

const char *PlusCharger_ChargeName(uint8_t chg_stat) {
    switch (chg_stat) {
        case 0: return "idle/full";
        case 1: return "trickle/CC";
        case 2: return "CV";
        case 3: return "top-off";
        default: return "?";
    }
}
