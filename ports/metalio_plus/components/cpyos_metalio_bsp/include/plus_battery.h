#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    int millivolts;
    int milliamps;   /* signed: positive = charging */
    int soc_percent; /* BQ27220 report, uncalibrated */
} plus_battery_sample_t;

/* Read the TI BQ27220 fuel gauge on the shared I2C1 bus (0x55).
 * Returns false when the gauge does not answer; the caller keeps its last
 * good sample. Vendor doc docs/battery-soc-and-charge.md warns that the chip's
 * SOC register is not calibrated for this cell, so treat soc_percent as a hint
 * and the voltage as the source of truth. */
bool PlusBattery_Read(plus_battery_sample_t *out);

/* Linear charge estimate used by the reference firmware while charging:
 * 3300 mV -> 0 %, 4300 mV -> 100 %. Not a discharge curve. */
int PlusBattery_PercentFromMillivolts(int millivolts);

/* True while the gauge reports a charging current (> 20 mA). Works on
 * charge-only sources that never enumerate as a USB host. */
bool PlusBattery_IsCharging(void);

#ifdef __cplusplus
}
#endif
