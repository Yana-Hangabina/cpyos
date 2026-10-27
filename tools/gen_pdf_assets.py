#!/usr/bin/env python3
"""Build the full PDF asset carrier stored in app1 (one-time provisioning).

The app1 carrier is read by src/pdf/PdfAssets.cpp on first boot and expanded
into:
  /.crosspoint/font_pack.dat   full cjk12 + title pack
  /.crosspoint/cmaps/*.cmap    all 71 MuPDF CMaps

Output: build/pdf_full_assets.bin (raw deflate, CMPS cmap container).
The carrier lives at app1 offset 0x1000 (flash 0x741000); it is overwritten by
an OTA update, but by then the SD copy is already installed.
"""
import os
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
FONT = BUILD / "font_pack.dat"
CMAPS = BUILD / "cmaps"
OUT = BUILD / "pdf_full_assets.bin"

MAGIC = b"PFPK"
CMAP_MAGIC = b"CMPS"
HEADER = 20


def deflate(data: bytes) -> bytes:
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def main() -> int:
    if not FONT.is_file():
        print(f"missing {FONT}; run tools/generate_font_pack first", file=sys.stderr)
        return 1
    if not CMAPS.is_dir():
        print(f"missing {CMAPS}", file=sys.stderr)
        return 1

    font_raw = FONT.read_bytes()
    font_comp = deflate(font_raw)

    names = sorted(p.name for p in CMAPS.iterdir() if p.is_file())
    if len(names) != 71:
        print(f"expected 71 CMaps, found {len(names)}", file=sys.stderr)
        return 1
    cmap = bytearray(struct.pack("<4sHH", CMAP_MAGIC, 1, len(names)))
    for name in names:
        raw = (CMAPS / name).read_bytes()
        comp = deflate(raw)
        nb = name.encode("ascii")
        cmap += struct.pack("<H", len(nb)) + nb + struct.pack("<II", len(raw), len(comp)) + comp

    out = bytearray(struct.pack("<4sHHIII", MAGIC, 1, 0, len(font_raw), len(font_comp), len(cmap)))
    out += font_comp
    out += cmap
    OUT.write_bytes(out)
    print(f"font: {len(font_raw)} -> {len(font_comp)} B")
    print(f"cmaps: {len(names)} -> {len(cmap)} B")
    print(f"carrier: {len(out)} B -> {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
