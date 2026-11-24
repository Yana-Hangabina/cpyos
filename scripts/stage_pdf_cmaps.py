#!/usr/bin/env python3
"""Copy MuPDF's external CMap resources to a mounted SD card.

The XIP PDF module deliberately does not contain the ~2 MB CMap tables.  The
app's CmapStore reads them from `/.crosspoint/cmaps`; this script stages the
directory from a MuPDF source checkout/release without touching the firmware or
the data partition.

Usage:
  python3 scripts/stage_pdf_cmaps.py /path/to/mupdf/resources/cmaps /Volumes/SD
"""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path, help="MuPDF resources/cmaps directory")
    ap.add_argument("card", type=Path, help="mounted SD-card root")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    if not args.source.is_dir():
        raise SystemExit(f"not a directory: {args.source}")
    files = sorted(p for p in args.source.iterdir() if p.is_file())
    if not files:
        raise SystemExit(f"no CMap files in {args.source}")
    destination = args.card / ".crosspoint" / "cmaps"
    print(f"{len(files)} CMap(s): {args.source} -> {destination}")
    if args.dry_run:
        return 0
    destination.mkdir(parents=True, exist_ok=True)
    for source in files:
        shutil.copy2(source, destination / source.name)
    print(f"staged {len(files)} CMap(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
