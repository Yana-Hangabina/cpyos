#include "epd_display.h"
#include "waveforms/my_waveform.h"

// CPYOS Plus: only the E0470A14-AF panel is exposed. The other vendor display
// tables are intentionally omitted so the 273 KB ED047TC2 waveform table stays
// out of the image. Source: CloudZao/Metalio-E-INK4-Plus (MIT), 2b2c36a.
const EpdDisplay_t E0470A14_AF = {
    .width = 1216,
    .height = 684,
    .bus_width = 8,
    .bus_speed = 20,
    .default_waveform = &MY_WAVEFORM,
    .display_type = DISPLAY_TYPE_GENERIC,
};
