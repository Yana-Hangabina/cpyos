#!/usr/bin/env python3
"""Chunked recovery writer for the Metalio E-INK4 Plus.

The USB-Serial/JTAG link on the bench unit drops after roughly 0.5 MB of a
single `esptool write-flash`, at 921600, 460800 and even 115200 baud. A
64 KB chunk per esptool invocation keeps every transfer short; a dropped chunk
is retried. Flash writes are idempotent and the regular flash tool still
requires a full 16 MiB backup, so this is a recovery path, not a replacement.

Run it with a Python that has esptool (PlatformIO's penv works):

    ~/.platformio/penv/bin/python ports/metalio_plus/scripts/flash_chunked.py \
        --firmware ports/metalio_plus/build/cpyos_metalio_plus.bin

`--app-only` rewrites app0/app1 only (for iterating after a full install).
"""
from __future__ import annotations

import argparse
import glob
import pathlib
import subprocess
import sys
import time

PORT_DIR = pathlib.Path(__file__).resolve().parent.parent
REPO = PORT_DIR.parent.parent
CHUNK = 64 * 1024


def discover_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise SystemExit("no /dev/cu.usbmodem*; connect the board and retry")
    return ports[0]


def esptool(args: list[str], port: str, baud: int, tries: int = 8) -> bool:
    for attempt in range(tries):
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s31", "--port", port,
               "--baud", str(baud), "--before", "default-reset", "--after", "hard-reset"] + args
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            return True
        tail = (result.stderr or result.stdout).strip().splitlines()
        print(f"  retry {attempt + 1}: {tail[-1] if tail else 'failed'}", flush=True)
        time.sleep(1.5)
    return False


def write(offset: int, data: bytes, label: str, port: str, baud: int, chunk: int) -> None:
    total = (len(data) + chunk - 1) // chunk
    for index in range(total):
        part = data[index * chunk:(index + 1) * chunk]
        path = pathlib.Path("/tmp/cpyos_plus_chunk.bin")
        path.write_bytes(part)
        print(f"{label}: {index + 1}/{total} @0x{offset + index * chunk:06x}", flush=True)
        if not esptool(["write-flash", "--no-compress", hex(offset + index * chunk), str(path)],
                       port, baud):
            raise SystemExit(f"{label}: chunk at 0x{offset + index * chunk:06x} failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=None)
    parser.add_argument("--baud", type=int, default=460800)
    parser.add_argument("--chunk", type=int, default=CHUNK)
    parser.add_argument("--firmware", type=pathlib.Path,
                        default=PORT_DIR / "build" / "cpyos_metalio_plus.bin")
    parser.add_argument("--app-only", action="store_true")
    args = parser.parse_args()

    port = args.port or discover_port()
    firmware = args.firmware.read_bytes()
    segments: list[tuple[int, pathlib.Path, str]] = [
        (0x010000, args.firmware, "app0"),
        (0x670000, args.firmware, "app1"),
    ]
    if not args.app_only:
        segments = [
            (0x008000, PORT_DIR / "build" / "partition_table" / "partition-table.bin", "table"),
        ] + segments + [
            (0xCD0000, REPO / "build" / "pinyin.dat", "pinyin"),
            (0xD26000, REPO / "build" / "web_pack.dat", "web"),
            (0xD60000, REPO / "build" / "hyphen.dat", "hyphen"),
            (0xDD0000, PORT_DIR / "build" / "i18n_pack.dat", "i18n"),
            (0xE30000, REPO / "build" / "emoji.dat", "emoji"),
            (0xE50000, REPO / "build" / "cjk_core.dat", "core"),
        ]
    for offset, path, label in segments:
        if not path.is_file():
            raise SystemExit(f"missing {path}")
        print(f"== {label}: {path.stat().st_size} B at 0x{offset:06x}", flush=True)
        write(offset, path.read_bytes(), label, port, args.baud, args.chunk)
    print("ALL CHUNKS WRITTEN", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
