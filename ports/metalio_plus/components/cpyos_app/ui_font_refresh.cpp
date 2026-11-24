// Read Pico's src/main.cpp owns these hooks; the Plus has its own shell main,
// so the shared reader (PDF reflow) needs local definitions.  The Plus selects
// faces through cpyos_ui/typography and does not build the external font-pack
// path, so both hooks are intentionally no-ops here.
#include "UiFontRefresh.h"

void useBuiltinFontFallback() {}

void refreshExternalFontFaces() {}
