#pragma once

// Releases the ESP32-S3 USB-OTG PHY and reconnects the hardware USB
// Serial/JTAG peripheral before a normal application restart. No-op on targets
// that do not enable the USB Drive capability (FREEINK_CAP_USB_MSC).
void handoffUsbOtgToSerialJtag();

// Inverse of the above, run once before Storage.beginUsbDrive(): detaches the
// hardware USB Serial/JTAG peripheral from the shared internal PHY so the
// Arduino TinyUSB stack can claim it for the U-disk. The console stays down
// until handoffUsbOtgToSerialJtag() + reboot. No-op off S3 / without the cap.
void prepareUsbOtgHandoff();
