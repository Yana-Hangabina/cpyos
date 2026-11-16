#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCF8563 real-time clock on the shared I2C1 bus (0x51).
 *
 * The chip stores plain calendar fields with no timezone. This port treats
 * what it writes as UTC seconds and leaves the local-time conversion to
 * HalClock/tz. UNTESTED ON HARDWARE.
 *
 * Register map (datasheet 8.2): 0x02 seconds, 0x03 minutes, 0x04 hours,
 * 0x05 days, 0x06 weekday, 0x07 months|century, 0x08 years (00..99). */
bool PlusRtc_Read(struct tm *utc);
bool PlusRtc_Write(const struct tm *utc);

/* True once a read has produced a plausible calendar value. */
bool PlusRtc_Ok(void);

#ifdef __cplusplus
}
#endif
