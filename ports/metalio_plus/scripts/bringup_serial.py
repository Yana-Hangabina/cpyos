#!/usr/bin/env python3
"""Run the first-power-on console checklist over the shell image's UART0.

USB-Serial/JTAG is not an interactive runtime console on this board: I2C takes
GPIO0/1 and the USB-JTAG link disappears. UART0 is on GPIO58/59 (vendor console),
so use a UART adapter or the board's debug pads. The bring-up image has no
command console; it only logs the 2 s UI-PROBE heartbeat.

Sends only read-only diagnostics plus the two panel test patterns and a
screenshot; it never flashes, erases, reboots or writes files.

    python3 scripts/bringup_serial.py --port /dev/cu.usbserial-XXXX --out /tmp/plus

Needs pyserial (the ESP-IDF/PlatformIO Python environment has it). The script
saves the full transcript to <out>/bringup.log and the framebuffer to
<out>/screen.bmp, then reports which expected markers were missing.
"""

from __future__ import annotations

import argparse
import glob
import sys
import time
from pathlib import Path

# (command, settle seconds, expected substring or None)
STEPS = [
    ("info", 0.6, "chip=esp32s31"),
    ("batt", 0.6, "BQ27220"),
    ("rtc", 0.6, "PCF8563"),
    ("sd", 0.8, "mounted"),
    ("sdwrite", 2.5, "sdwrite: PASS"),
    ("imu", 0.6, "imu: available"),
    ("touch", 0.6, "touch: present=1"),
    ("keys", 0.6, "keys: P0="),
    ("slpinfo", 0.6, "slpinfo: wake="),
    ("i2c", 3.0, "i2c1 scan:"),
    ("pinyin", 1.0, "pinyin: loaded=1"),
    ("hyphen", 1.0, "hyphen: en=1"),
    ("crypto", 0.8, "crypto: ok"),
    ("selftest", 2.5, "crypto: ok"),
    ("panel", 6.0, "panel:"),
    ("window", 4.0, "window:"),
]


def detect_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        raise SystemExit("no serial port found; pass --port")
    return ports[0]


def read_for(ser, seconds: float) -> bytes:
    deadline = time.time() + seconds
    data = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            data += chunk
        else:
            time.sleep(0.02)
    return bytes(data)


def capture_screenshot(ser, out: Path, timeout: float = 60.0) -> bool:
    ser.write(b"screenshot\n")
    deadline = time.time() + timeout
    buffer = bytearray()
    marker = b"SCREENSHOT_START:"
    while marker not in buffer and time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buffer += chunk
    if marker not in buffer:
        return False
    head, rest = buffer.split(marker, 1)
    if b"\n" not in rest:
        rest += read_for(ser, 1.0)
    size_text, rest = rest.split(b"\n", 1)
    try:
        size = int(size_text.strip())
    except ValueError:
        return False
    payload = bytearray(rest)
    while len(payload) < size + len(b"\nSCREENSHOT_END"):
        chunk = ser.read(min(8192, size + 32 - len(payload)))
        if not chunk:
            if time.time() > deadline:
                return False
            continue
        payload += chunk
    out.write_bytes(bytes(payload[:size]))
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", type=Path, default=Path("plus-bringup"))
    parser.add_argument("--skip-screenshot", action="store_true")
    parser.add_argument("--settle", type=float, default=1.5, help="extra wait before the first command")
    args = parser.parse_args()

    try:
        import serial  # type: ignore  # lazy so --help works with any python
    except ImportError:
        print("error: pyserial is missing; run this with the ESP-IDF or PlatformIO python", file=sys.stderr)
        return 2

    port = args.port or detect_port()
    args.out.mkdir(parents=True, exist_ok=True)
    transcript = bytearray()
    missing: list[str] = []
    print(f"port {port}; transcript -> {args.out / 'bringup.log'}")

    with serial.Serial(port, args.baud, timeout=0.2) as ser:
        time.sleep(args.settle)
        transcript += read_for(ser, 0.8)
        for command, settle, expected in STEPS:
            ser.write(command.encode() + b"\n")
            transcript += read_for(ser, settle)
            text = transcript.decode("utf-8", "replace")
            status = "ok" if expected is None or expected in text else "MISSING"
            if status == "MISSING":
                missing.append(f"{command}: {expected}")
            print(f"  {command:10s} {status}")

        if not args.skip_screenshot:
            before = len(transcript)
            ok = capture_screenshot(ser, args.out / "screen.bmp")
            transcript += read_for(ser, 0.5)
            print(f"  {'screenshot':10s} {'saved' if ok else 'FAILED'}")
            if not ok:
                missing.append("screenshot: SCREENSHOT_START")
            del before

        # Drain any late output so the transcript ends cleanly.
        transcript += read_for(ser, 0.5)

    (args.out / "bringup.log").write_bytes(bytes(transcript))
    if missing:
        print("\nmissing markers:")
        for item in missing:
            print(f"  - {item}")
        return 1
    print("\nall expected markers present; inspect screen.bmp and the log for the visual checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
