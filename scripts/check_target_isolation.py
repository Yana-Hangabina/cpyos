#!/usr/bin/env python3
"""Target isolation preflight for the CPYOS tree.

Read Pico (ESP32-S3, PlatformIO) and Metalio E-INK4 Plus (ESP32-S31, ESP-IDF)
share most sources but have incompatible images, toolchains and flash layouts.
This checker fails loudly when a tree or a connected board belongs to the other
target, before a build or a flash can mix them.

    python3 scripts/check_target_isolation.py --target e4plus
    python3 scripts/check_target_isolation.py --target readpico --port /dev/cu.usbmodem101
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PLUS = REPO / "ports" / "metalio_plus"


def fail(message: str) -> None:
    print(f"target-isolation: FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        fail(f"cannot read {path}: {exc}")
    return ""


def parse_app_slots(csv_path: Path) -> dict[str, int]:
    slots: dict[str, int] = {}
    for line in read(csv_path).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 5 or fields[1] != "app":
            continue
        slots[fields[0]] = int(fields[4], 0)
    return slots


def check_partition_table(label: str, path: Path) -> None:
    if not path.is_file():
        fail(f"{label}: partition table missing at {path}")
    slots = parse_app_slots(path)
    if slots.get("app0") != 0x660000 or slots.get("app1") != 0x660000:
        fail(f"{label}: expected two 6.375 MiB app slots in {path}, found {slots}")
    print(f"ok  {label}: two 0x660000 app slots in {path.name}")


def check_e4plus() -> None:
    sdkconfig = PLUS / "sdkconfig"
    if not sdkconfig.is_file():
        fail(f"e4plus: {sdkconfig} is missing; configure the Plus project first")
    target = re.search(r'^CONFIG_IDF_TARGET="([^"]+)"', read(sdkconfig), re.MULTILINE)
    if not target or target.group(1) != "esp32s31":
        fail(f"e4plus: {sdkconfig} is not configured for esp32s31")
    print("ok  e4plus: sdkconfig targets esp32s31")

    custom = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"', read(sdkconfig), re.MULTILINE)
    if not custom:
        fail("e4plus: sdkconfig has no custom partition table")
    table = PLUS / custom.group(1)
    check_partition_table("e4plus", table)

    cmake = read(PLUS / "CMakeLists.txt")
    if 'IDF_TARGET STREQUAL "esp32s31"' not in cmake:
        fail("e4plus: ports/metalio_plus/CMakeLists.txt lost its esp32s31 guard")
    print("ok  e4plus: CMake target guard present")

    if (REPO / ".pio" / "build" / "readpico").is_dir():
        print("note e4plus: .pio/build/readpico also exists in this tree; keep builds in separate trees")


def check_readpico() -> None:
    table = REPO / "partitions_readpico.csv"
    check_partition_table("readpico", table)
    platformio = read(REPO / "platformio.ini")
    if "board_build.partitions = partitions_readpico.csv" not in platformio:
        fail("readpico: platformio.ini no longer selects partitions_readpico.csv")
    print("ok  readpico: platformio.ini selects partitions_readpico.csv")


def probe_chip(port: str, python_exe: str, expected: str) -> None:
    cmd = [python_exe, "-m", "esptool", "--port", port, "chip_id"]
    result = subprocess.run(cmd, check=False, text=True, capture_output=True)
    text = result.stdout + result.stderr
    if result.returncode != 0:
        fail(f"chip probe on {port} failed:\n{text.strip()}")
    if expected not in text:
        fail(f"{port} is not {expected}:\n{text.strip()}")
    print(f"ok  {port}: {expected}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", choices=("e4plus", "readpico"), required=True)
    parser.add_argument("--port")
    parser.add_argument("--esptool-python", default=sys.executable)
    args = parser.parse_args()

    if args.target == "e4plus":
        check_e4plus()
        expected = "ESP32-S31"
    else:
        check_readpico()
        expected = "ESP32-S3"

    if args.port:
        probe_chip(args.port, args.esptool_python, expected)
    else:
        print(f"note {args.target}: no --port; connected chip was not checked")

    print(f"target-isolation: {args.target} preflight passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
