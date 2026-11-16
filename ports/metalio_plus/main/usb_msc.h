#pragma once

namespace usb_msc {

// Hands the SD card to a USB host as one mass-storage LUN. The app's FAT/VFS is
// detached first and is NOT remounted: the shell must stop touching storage and
// the caller restarts the device to return to normal operation (the same
// handoff model as the reference U-disk page).
bool start();

// Stops the drivers if they were started; storage stays detached until reboot.
void stop();

bool running();
bool hostEjected();

}  // namespace usb_msc
