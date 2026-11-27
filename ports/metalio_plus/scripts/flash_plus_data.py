#!/usr/bin/env python3
"""Flash the Plus data/spiffs partition described by build/data_manifest_plus.txt.

Only `data` rows of the manifest are written; the reserved factory bytes and
every app/bootloader region are rejected.  esptool verifies the digest of each
written range, so a silent partial write is not possible.

    python3 scripts/generate_data_manifest.py \\
        --partitions ports/metalio_plus/partitions_plus_ota.csv \\
        --out build/data_manifest_plus.txt
    python3 ports/metalio_plus/scripts/flash_plus_data.py --dry-run
    python3 ports/metalio_plus/scripts/flash_plus_data.py

Run with the ESP-IDF python (it provides esptool):
    . .scratch/esp-idf-6.1/export.sh
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
DEFAULT_MANIFEST = os.path.join(ROOT, "build", "data_manifest_plus.txt")


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_blob(manifest_path: str, relative: str) -> str:
    """Where a data row's file lives.

    A release bundle ships the manifest next to its packs (plus-*.dat), so the
    manifest's own directory wins; a path that is not there falls back to the
    repository root, which is what a checkout with build/ expects.
    """
    local = os.path.join(os.path.dirname(os.path.abspath(manifest_path)), relative)
    if os.path.isfile(local):
        return local
    return os.path.join(ROOT, relative)


def parse_manifest(path: str):
    base = size = None
    rows = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if fields[0] == "partition":
                base = int(fields[2], 0)
                size = int(fields[3], 0)
            elif fields[0] == "data":
                rows.append({
                    "name": fields[1],
                    "address": int(fields[3], 0),
                    "size": int(fields[4], 0),
                    "sha256": fields[5],
                    "path": resolve_blob(path, fields[6]),
                })
    if base is None or size is None:
        raise SystemExit(f"{path}: no partition row")
    return base, size, rows


def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(glob.glob("/dev/ttyUSB*"))
    if not ports:
        raise SystemExit("no USB serial port found; pass --port")
    return ports[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--port")
    ap.add_argument("--baud", default="921600")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    base, size, rows = parse_manifest(args.manifest)
    problems = []
    for row in rows:
        if not os.path.isfile(row["path"]):
            problems.append(f"{row['name']}: missing {row['path']}")
            continue
        actual = os.path.getsize(row["path"])
        if actual != row["size"]:
            problems.append(f"{row['name']}: size {actual} != manifest {row['size']}")
        if sha256(row["path"]) != row["sha256"]:
            problems.append(f"{row['name']}: sha256 differs from the manifest")
        if row["address"] < base or row["address"] + row["size"] > base + size:
            problems.append(f"{row['name']}: 0x{row['address']:06x} is outside the data partition")
        # App slots start at 0x10000/0x670000; the data partition is well clear of
        # both, so anything below it is a manifest bug worth refusing.
        if row["address"] < base:
            problems.append(f"{row['name']}: would write below the data partition")
    if problems:
        for problem in problems:
            print("error:", problem, file=sys.stderr)
        return 1
    if not rows:
        print("error: the manifest has no data rows", file=sys.stderr)
        return 1

    print(f"data partition 0x{base:06x}..0x{base + size:06x}")
    for row in rows:
        print(f"  0x{row['address']:06x}  {row['size']:>9} B  {row['name']:<7} {row['path']}")

    if args.dry_run:
        print("dry run: would write",
              ", ".join(f"{row['name']}@0x{row['address']:06x}" for row in rows))
        return 0
    port = find_port(args.port)
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s31", "-p", port,
           "-b", args.baud, "write_flash"]
    for row in rows:
        cmd += [f"0x{row['address']:x}", row["path"]]
    print(" ".join(cmd))
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
