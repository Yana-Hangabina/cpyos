"""Compile the real HalGPIO::isUsbConnected() with explicit power telemetry stubs.

The X3 has no USB-detect pin: its external-power answer comes from the SDK's
BatteryMonitor (BQ27220 Current() sign / charger input rail), which initialises
the I2C bus first. This harness proves the app layer no longer owns a raw I2C
probe, that X3 never samples its SDA/SCL pin as a digital USB signal, and that an
authoritative external-power reading wins over the charging-state fallback.

Extracted from lib/hal/HalGPIO.cpp so the test cannot drift from the shipped code.
"""

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


CONFIGS = (
    # metalio, waveshare — every board family the function has a branch for,
    # plus the generic (X3/X4/…) path.
    (0, 0),
    (1, 0),
    (0, 1),
)


def run():
    usb = function((ROOT / "lib/hal/HalGPIO.cpp").read_text(), "bool HalGPIO::isUsbConnected() const")
    harness = (
        r"""
#include <cassert>
#include <cstdint>
constexpr int HIGH=1;
bool x3=false;int usbLevel=0,digitalReads=0;
int digitalRead(int pin) {assert(pin==20);++digitalReads;return usbLevel;}
namespace BoardConfig {struct Profile {int usbDetect=-1;};Profile ACTIVE;}
struct BatteryMonitor {
 static inline bool known=false,present=false,charging=false;
 static inline int powerReads=0,chargeReads=0;
 bool isExternalPowerPresent(bool* out) const {++powerReads;*out=known;return present;}
 bool isCharging() const {++chargeReads;return charging;}
};
bool frames=false;bool usbHostSofActive() {return frames;}
struct Waveshare397Power {
 static inline bool known=false,present=false;
 static bool externalPowerConnected(bool& out) {out=present;return known;}
};
namespace freeink { namespace metalio {
 static inline bool known=false,present=false;
 inline bool externalPowerConnected(bool& out) {out=present;return known;}
}}
struct HalGPIO {bool deviceIsX3() const {return x3;} bool isUsbConnected() const;};
"""
        + usb
        + r"""
int main() {
 HalGPIO gpio;
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
 Waveshare397Power::known=true;Waveshare397Power::present=true;assert(gpio.isUsbConnected());
 Waveshare397Power::present=false;frames=true;assert(!gpio.isUsbConnected());
 Waveshare397Power::known=false;assert(gpio.isUsbConnected());
 frames=false;assert(!gpio.isUsbConnected());
 assert(BatteryMonitor::powerReads==0 && BatteryMonitor::chargeReads==0);
#elif FREEINK_DEVICE_METALIO_EINK4
 // Metalio's own helper answers first; when it has no opinion the function is
 // allowed to fall through to the generic SDK telemetry tail (unlike Waveshare,
 // whose block returns unconditionally).
 freeink::metalio::known=true;freeink::metalio::present=true;assert(gpio.isUsbConnected());
 freeink::metalio::present=false;frames=true;assert(!gpio.isUsbConnected());
 freeink::metalio::known=false;assert(gpio.isUsbConnected());
 frames=false;assert(!gpio.isUsbConnected());
 assert(BatteryMonitor::powerReads==1 || BatteryMonitor::chargeReads==1);
#else
 // X4 uses the physical USB pin, independently of battery telemetry.
 BoardConfig::ACTIVE.usbDetect=20;
 BatteryMonitor::charging=true;
 assert(!gpio.isUsbConnected());usbLevel=HIGH;assert(gpio.isUsbConnected());
 assert(digitalReads==2 && BatteryMonitor::powerReads==0 && BatteryMonitor::chargeReads==0);
 // X3 must never sample SDA20 as a digital USB signal, even with an old profile.
 x3=true;assert(gpio.isUsbConnected());
 BatteryMonitor::charging=false;assert(!gpio.isUsbConnected());
 assert(digitalReads==2 && BatteryMonitor::chargeReads==2);
 BoardConfig::ACTIVE.usbDetect=-1;
 assert(!gpio.isUsbConnected());
 BatteryMonitor::charging=true;assert(gpio.isUsbConnected());
 // An authoritative external-power reading wins over charging state.
 BatteryMonitor::known=true;
 int before=BatteryMonitor::chargeReads;
 assert(!gpio.isUsbConnected());
 BatteryMonitor::present=true;BatteryMonitor::charging=false;assert(gpio.isUsbConnected());
 assert(BatteryMonitor::chargeReads==before && digitalReads==2);
#endif
}
"""
    )
    with tempfile.TemporaryDirectory(prefix="hal-power-") as directory:
        tmp = Path(directory)
        (tmp / "check.cpp").write_text(harness)
        for metalio, waveshare in CONFIGS:
            binary = tmp / f"check-{metalio}-{waveshare}"
            subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    f"-DFREEINK_DEVICE_METALIO_EINK4={metalio}",
                    f"-DFREEINK_DEVICE_WAVESHARE_EPAPER_397={waveshare}",
                    str(tmp / "check.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                env=dict(os.environ, CCACHE_DISABLE="1"),
            )
            subprocess.run([str(binary)], check=True)
    print("X3/X4 SDK power routing, authoritative telemetry and board fallbacks passed")


if __name__ == "__main__":
    run()
