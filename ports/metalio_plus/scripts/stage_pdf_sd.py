#!/usr/bin/env python3
"""Stage the PDF engine and its asset bundle onto a mounted SD card.

The Plus keeps the MuPDF engine on the card (it is executed from PSRAM) and the
reading font + CMaps in one PFPK bundle that the device expands once on the
first PDF open.  Both are build artifacts; this script only copies them.

    python3 ports/metalio_plus/scripts/stage_pdf_sd.py \
        --card /Volumes/NO\\ NAME \
        --engine build/pdfmod-rv.bin \
        --bundle build/pdf_full_assets.bin

`--dry-run` prints the plan; `--sha-only` prints both digests without touching
the card.
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
from pathlib import Path

ENGINE_REL = Path(".crosspoint/pdf/pdfmod.bin")
BUNDLE_REL = Path(".crosspoint/pdf_assets.bin")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--card", type=Path, help="mounted SD-card root")
    ap.add_argument("--engine", type=Path, required=True, help="build/pdfmod-rv.bin")
    ap.add_argument("--bundle", type=Path, required=True, help="build/pdf_full_assets.bin")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--sha-only", action="store_true")
    args = ap.parse_args()

    for p, what in ((args.engine, "engine"), (args.bundle, "bundle")):
        if not p.is_file() or p.stat().st_size == 0:
            raise SystemExit(f"missing {what}: {p}")
    print(f"engine {args.engine} {args.engine.stat().st_size} B sha256 {sha256(args.engine)}")
    print(f"bundle {args.bundle} {args.bundle.stat().st_size} B sha256 {sha256(args.bundle)}")
    if args.sha_only:
        return 0

    if args.card is None:
        raise SystemExit("--card is required (or use --sha-only)")
    if not args.card.is_dir():
        raise SystemExit(f"not a directory: {args.card}")
    for rel, src in ((ENGINE_REL, args.engine), (BUNDLE_REL, args.bundle)):
        dest = args.card / rel
        print(f"{src} -> {dest}")
        if args.dry_run:
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        if sha256(dest) != sha256(src):
            raise SystemExit(f"copy verification failed: {dest}")
    if not args.dry_run:
        print("staged engine and asset bundle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
