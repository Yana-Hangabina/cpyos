#!/usr/bin/env python3
"""Send bench-console commands over the Plus USB-Serial/JTAG port.

pyserial's open/close toggles DTR/RTS on macOS, which resets the S31 (and can
park it in ROM download mode). This helper opens the tty with raw termios and
clears HUPCL, so a session neither resets the chip nor leaves it resetting.

    # wait for the shell, run `info`, print everything it said
    python3 ports/metalio_plus/scripts/plus_console_cmd.py --wait 8 --cmd info

    # install the engine and capture the reply
    python3 ports/metalio_plus/scripts/plus_console_cmd.py --cmd "pdfengineinstall /pdfmod.bin"

After a fresh flash the chip is in the app already only if esptool hard-reset
it; this script never resets, it just talks.
"""
from __future__ import annotations

import argparse
import glob
import os
import select
import sys
import termios
import time


class Port:
    def __init__(self, path: str) -> None:
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
                      termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON | termios.IXOFF)
        attrs[1] &= ~termios.OPOST
        attrs[2] &= ~(termios.CSIZE | termios.PARENB)
        attrs[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] &= ~(termios.ICANON | termios.ECHO | termios.ECHOE | termios.ISIG | termios.IEXTEN)
        attrs[4] &= ~getattr(termios, "HUPCL", 0)
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def drain(self, seconds: float) -> bytes:
        buf = b""
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.2)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 8192)
            except BlockingIOError:
                continue
            if chunk:
                buf += chunk
        return buf

    def write(self, data: bytes) -> None:
        os.write(self.fd, data)

    def close(self) -> None:
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        os.close(self.fd)


def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise SystemExit("no /dev/cu.usbmodem* found; pass --port")
    return ports[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--wait", type=float, default=0.0, help="seconds of boot output to drain first")
    ap.add_argument("--cmd", action="append", default=[], help="console command (repeatable)")
    ap.add_argument("--timeout", type=float, default=6.0, help="seconds to read after the last command")
    ap.add_argument("--echo", action="store_true", help="print drained boot output too")
    args = ap.parse_args()

    port = Port(find_port(args.port))
    if args.wait:
        boot = port.drain(args.wait)
        if args.echo:
            sys.stdout.write(boot.decode("utf-8", "replace"))
    out = b""
    for cmd in args.cmd:
        port.write(cmd.encode() + b"\n")
        out += port.drain(args.timeout)
    port.close()
    sys.stdout.write(out.decode("utf-8", "replace"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
