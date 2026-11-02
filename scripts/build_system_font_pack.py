#!/usr/bin/env python3
"""Build the external system-font pack for no-PSRAM targets.

Layout (little-endian):
  0x00  magic "CPFP"
  0x04  uint16 version (1)
  0x06  uint16 header size (32)
  0x08  uint32 payload size
  0x0c  uint32 payload CRC-32
  0x10  reserved[16]
  0x20  TrueType font bytes

The firmware streams glyphs from this raw TTF through TtfEpdFont; it never
loads the whole file into internal RAM. The pack is written to the ESP32-C3
spiffs partition by exp/flash_system_font.sh.
"""
import argparse
import pathlib
import struct
import zlib

MAGIC = b"CPFP"
VERSION = 1
HEADER_SIZE = 32


ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_FONT = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-Regular-8105.ttf"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default=str(DEFAULT_FONT),
                    help="TTF to wrap (default: the committed GB2312 subset)")
    ap.add_argument("--out", default=str(ROOT / "build" / "system_font.dat"))
    ap.add_argument("--embedded-out", default=None,
                    help="also write the raw-deflate carrier linked into PSRAM builds")
    args = ap.parse_args()
    font = pathlib.Path(args.font).read_bytes()
    if not font:
        raise SystemExit("empty font")
    crc = zlib.crc32(font) & 0xFFFFFFFF
    header = MAGIC + struct.pack("<HHII", VERSION, HEADER_SIZE, len(font), crc) + bytes(16)
    assert len(header) == HEADER_SIZE
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + font)
    print(f"{out}: {len(font)} B payload, crc=0x{crc:08x}, total={len(header)+len(font)} B")

    if args.embedded_out:
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = compressor.compress(font) + compressor.flush()
        embedded_header = b"CPFZ" + struct.pack("<HHIII", VERSION, HEADER_SIZE, len(font), crc, len(packed)) + bytes(12)
        assert len(embedded_header) == HEADER_SIZE
        embedded = pathlib.Path(args.embedded_out)
        embedded.parent.mkdir(parents=True, exist_ok=True)
        embedded.write_bytes(embedded_header + packed)
        print(f"{embedded}: {len(font)} -> {len(packed)} B deflate, total={len(embedded_header)+len(packed)} B")


if __name__ == "__main__":
    main()
