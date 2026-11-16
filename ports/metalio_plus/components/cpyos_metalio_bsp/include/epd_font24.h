#pragma once

#include "config.h"

/* Minimal stand-in for the vendor epd_font24 header. metalio_touch.c only
 * needs the logical panel size; the font tables themselves are not used. */
#define EPD_LOGICAL_W DISPLAY_WIDTH
#define EPD_LOGICAL_H DISPLAY_HEIGHT
