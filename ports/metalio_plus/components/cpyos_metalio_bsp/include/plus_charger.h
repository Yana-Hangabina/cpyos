#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CX25601N charger status (0x6B), read-only.
 *
 * Register map from the vendor driver (MIT, boards/common/power/cx25601n.cc):
 *   0x38 PART_INFO, 0x1E STATUS1: [2:0] VBUS status, [4:3] CHG_STAT.
 * Charge parameters are deliberately left at the vendor defaults: the cell's
 * charge voltage is not documented in this repository, so reconfiguring VREG
 * would be a guess. */
typedef struct {
    bool valid;
    uint8_t vbus;      /* 0 none, 1 SDP, 2 CDP, 3 DCP, 4 unknown, 5 non-standard, 7 OTG */
    uint8_t chg_stat;  /* 0 not charging/full, 1 trickle/CC, 2 CV, 3 top-off */
} plus_charger_sample_t;

bool PlusCharger_Read(plus_charger_sample_t *out);
const char *PlusCharger_VbusName(uint8_t vbus);
const char *PlusCharger_ChargeName(uint8_t chg_stat);

#ifdef __cplusplus
}
#endif
