"""Exercise the real HAL gesture path with controlled IMU samples and time.

HalTiltSensor no longer names a board: it asks the SDK whether the active part has
a gyroscope (upstream 8fc2b661). Both answers are exercised here, because an
accelerometer-only part must still be a usable motion source on every target.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

IMU_STUB = '''#pragma once
class Imu {
 public:
  struct Sample { float ax=0,ay=0,az=0,gx=0,gy=0,gz=0; };
  static Sample sample;
  static int reads;
  static bool readOk;
  static bool hasGyro;
  static bool beginOk;
  static bool sleepCalled;
  static bool capabilityQueriedBeforeSleep;
  bool begin() { return beginOk; }
  bool wake() { return true; }
  bool sleep() { sleepCalled = true; return true; }
  bool read(Sample& out) { ++reads; out=sample; return readOk; }
  bool hasGyroscope() const {
    if (!sleepCalled) capabilityQueriedBeforeSleep = true;
    return hasGyro;
  }
};
'''


def run():
    with tempfile.TemporaryDirectory(prefix="crossmux-tilt-") as directory:
        root = Path(directory)
        env = dict(os.environ, CCACHE_DIR=str(root / "ccache"))
        (root / "Arduino.h").write_text(
            "#pragma once\n#include <cstdint>\n#include <cmath>\n"
            "extern unsigned long clockMs;\ninline unsigned long millis() { return clockMs; }\n"
        )
        (root / "Logging.h").write_text("#pragma once\n#define LOG_ERR(...) ((void)0)\n#define LOG_INF(...) ((void)0)\n")
        (root / "Imu.h").write_text(IMU_STUB)
        for gyro in (0, 1):
            binary = root / f"tilt-gyro-{gyro}"
            subprocess.run(["c++", "-std=c++17", f"-DHAS_GYRO={gyro}",
                            "-I" + str(root), "-I" + str(ROOT / "lib/hal"),
                            str(ROOT / "lib/hal/HalTiltSensor.cpp"),
                            str(ROOT / "test/hal_tilt/gestures.cpp"), "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary)], check=True)
        print("Accelerometer cadence/baselines, gyro gestures and the gyroscope capability query passed")


if __name__ == "__main__":
    run()
