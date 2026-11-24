#!/usr/bin/env python3
"""Flow-controlled file upload over the Plus console (putfilefast).

The 64-byte `putfile` loop is safe but far too slow for a multi-megabyte PDF
engine.  `putfilefast` accepts 1 KiB blocks and ACKs each write; this script
sends one block, waits for the ACK, and then continues, so the USB-Serial/JTAG
RX FIFO cannot be overrun.  A CRC32 over the stream is printed by the device and
checked here before the file is trusted.

    python3 ports/metalio_plus/scripts/putfile_fast.py \
        --local build/pdfmod-rv.bin --remote /.crosspoint/pdf/pdfmod.bin

The port is opened with raw termios and never touches DTR/RTS (opening with
pyserial toggles the modem lines and can reset the SoC mid-session).
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import select
import termios
import time
import zlib


class RawPort:
    def __init__(self, path: str) -> None:
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP | termios.INLCR |
                      termios.IGNCR | termios.ICRNL | termios.IXON | termios.IXOFF)
        attrs[1] &= ~termios.OPOST
        attrs[2] &= ~(termios.CSIZE | termios.PARENB)
        attrs[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] &= ~(termios.ICANON | termios.ECHO | termios.ECHOE | termios.ISIG | termios.IEXTEN)
        # HUPCL drops DTR on close, which resets the S31 (and can leave it in ROM
        # download mode). Clearing it keeps the app running across uploads.
        attrs[4] &= ~getattr(termios, "HUPCL", 0)
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.pending = b""

    def write(self, data: bytes) -> None:
        view = memoryview(data)
        while view:
            _, writable, _ = select.select([], [self.fd], [], 5.0)
            if not writable:
                raise TimeoutError("serial write stalled")
            n = os.write(self.fd, view)
            view = view[n:]

    def line(self, timeout: float) -> str | None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                return raw.decode("utf-8", "replace").strip()
            ready, _, _ = select.select([self.fd], [], [], 0.2)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                continue
            if chunk:
                self.pending += chunk
        return None

    def close(self) -> None:
        os.close(self.fd)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="default: the first /dev/cu.usbmodem*")
    ap.add_argument("--local", required=True)
    ap.add_argument("--remote", required=True, help="card path, e.g. /pdfmod.bin")
    ap.add_argument("--block", type=int, default=2048)
    args = ap.parse_args()

    data = open(args.local, "rb").read()
    size = len(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    port = args.port or (sorted(glob.glob("/dev/cu.usbmodem*")) or [""])[0]
    if not port:
        raise SystemExit("no /dev/cu.usbmodem* found")

    dev = RawPort(port)
    try:
        dev.line(0.5)  # drain a line if one is pending
        dev.write(f"putfilefast {args.remote} {size}\n".encode())
        # Boot/UI work can delay the console task; 15 s was too tight on the
        # first command after a flash (observed: READY arrived after the host
        # had already given up).
        deadline = time.time() + 60
        while True:
            line = dev.line(1.0)
            if line is None:
                if time.time() > deadline:
                    raise SystemExit("no PFB_READY (is the device in app mode?)")
                continue
            # Console log lines and the uploader's own prints interleave on the
            # same stream and the [hb] heartbeat has no trailing newline, so a
            # marker can arrive glued to the previous text.  Match anywhere.
            if "PFB_READY" in line:
                break
            if "putfilefast:" in line:
                raise SystemExit(line)
        print(f"uploading {size} bytes -> {args.remote} ({args.block} B blocks)", flush=True)

        off = 0
        last_report = 0
        while off < size:
            block = data[off:off + args.block]
            dev.write(block)
            target = off + len(block)
            while True:
                line = dev.line(30.0)
                if line is None:
                    raise SystemExit(f"timeout waiting for ACK at {off}/{size}")
                ack = re.search(r"PFB_ACK (\d+)", line)
                if ack:
                    acked = int(ack.group(1))
                    if acked == target:
                        off = acked
                        break
                    if acked > target or acked < off:
                        raise SystemExit(f"unexpected ACK {acked} (block {off}..{target})")
                    # The device only ACKs a fully drained block; a stray log
                    # line carrying an older count is ignored and we keep
                    # waiting for this block's ACK.
                    continue
                if "putfilefast:" in line:
                    raise SystemExit(line)
            if off - last_report >= 262144 or off == size:
                print(f"  {off}/{size} ({off * 100 // max(size, 1)}%)", flush=True)
                last_report = off

        while True:
            line = dev.line(30.0)
            if line is None:
                raise SystemExit("timeout waiting for PFB_DONE")
            done = re.search(r"PFB_DONE (\d+) ([0-9a-fA-F]{8})", line)
            if done:
                got = int(done.group(1))
                got_crc = int(done.group(2), 16)
                if got != size or got_crc != crc:
                    raise SystemExit(f"CRC/size mismatch: device {got}/{got_crc:08x}, host {size}/{crc:08x}")
                print(f"ok: {size} bytes, crc32 {crc:08x}")
                return 0
            if "putfilefast:" in line:
                raise SystemExit(line)
    finally:
        dev.close()


if __name__ == "__main__":
    raise SystemExit(main())
