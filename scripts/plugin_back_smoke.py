#!/usr/bin/env python3
"""Automate the plugin Back acceptance on a connected CPYOS device.

    python3 scripts/plugin_back_smoke.py --target e4plus
    python3 scripts/plugin_back_smoke.py --target readpico

The serial handle is opened with raw termios and never touches DTR/RTS: a
pyserial open/close can wedge the Read Pico USB-Serial/JTAG unit. The script
opens the plugin picker, selects the first plugin row (rescan + check rows come
first), launches it, presses Back, and checks that the plugin exited while the
manager stayed on screen.
"""
from __future__ import annotations

import argparse
import glob
import os
import select
import sys
import termios
import time


class RawPort:
    def __init__(self, path: str):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP | termios.INLCR |
                      termios.IGNCR | termios.ICRNL | termios.IXON | termios.IXOFF)
        attrs[1] &= ~termios.OPOST
        attrs[2] &= ~(termios.CSIZE | termios.PARENB)
        attrs[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] &= ~(termios.ICANON | termios.ECHO | termios.ECHOE | termios.ISIG | termios.IEXTEN)
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def write(self, text: str) -> None:
        os.write(self.fd, text.encode() + b"\n")

    def drain(self, seconds: float) -> bytes:
        out = bytearray()
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                continue
            if chunk:
                out.extend(chunk)
        return bytes(out)

    def close(self) -> None:
        os.close(self.fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", choices=("e4plus", "readpico"), required=True)
    parser.add_argument("--port", default=None)
    args = parser.parse_args()
    port = args.port or (sorted(glob.glob("/dev/cu.usbmodem*")) or [""])[0]
    if not port:
        raise SystemExit("no /dev/cu.usbmodem* found")
    dev = RawPort(port)
    try:
        dev.drain(6)
        if args.target == "e4plus":
            sequence = ["plugins", "ring 2", "confirm", "key back"]
        else:
            sequence = ["CMD:PLUGINS", "CMD:ROWDOWN", "CMD:ROWDOWN", "CMD:CONFIRM", "CMD:BACK"]
        log = bytearray()
        for command in sequence:
            dev.write(command)
            log.extend(dev.drain(2 if command not in ("confirm", "CMD:CONFIRM") else 5))
        log.extend(dev.drain(3))
    finally:
        dev.close()
    text = log.decode("utf-8", "replace")
    launch = text.find("Entering activity: Plugin")
    after_launch = text[launch:] if launch >= 0 else ""
    ok = (launch >= 0 and "Exiting activity: Plugin" in after_launch and
          "Exiting activity: Plugins" not in after_launch)
    print(f"debug launch={launch} child_exit={'Exiting activity: Plugin' in after_launch} "
          f"parent_exit={'Exiting activity: Plugins' in after_launch}")
    print(text[-2500:])
    print(f"plugin Back smoke: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
