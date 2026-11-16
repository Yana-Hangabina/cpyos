#include <cstdint>

// The scan watchdog is a Read Pico epdiy safety net; the port's panel driver
// does not have a bounded-wait watchdog yet.
void armScanWatchdog(uint32_t) {}
void disarmScanWatchdog() {}
