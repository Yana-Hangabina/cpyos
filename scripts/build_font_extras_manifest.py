#!/usr/bin/env python3
"""Build the FEXT1 manifest consumed by Settings -> Font Extras.

The firmware deliberately does not parse JSON for this: the manifest is a
small tab-separated text file, so the device parser and this generator can be
tested independently. Assets are *not* copied by this script; point `--assets-dir`
at the directory that will be published (site/fonts for Pages, or a release
asset directory) and the manifest records each file's size and SHA-256.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

MAX_PACK_BYTES = 32 * 1024 * 1024
ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,32}$")
FAMILY_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
FILE_RE = re.compile(r"^[A-Za-z0-9._-]{5,64}$")
URL_RE = re.compile(r"^https://[^\s]+$")
SHA_RE = re.compile(r"^[0-9a-fA-F]{64}$")


class ManifestError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ManifestError(message)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build(spec_path: Path, assets_dir: Path) -> str:
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    packs = spec.get("packs")
    _require(isinstance(packs, list) and 0 < len(packs) <= 12, "packs must be a non-empty list (<=12)")

    lines = ["FEXT1"]
    seen_ids: set[str] = set()
    seen_families: set[str] = set()
    for index, pack in enumerate(packs):
        _require(isinstance(pack, dict), f"pack {index} is not an object")
        pack_id = str(pack.get("id", ""))
        kind = str(pack.get("kind", ""))
        family = str(pack.get("family", ""))
        name = str(pack.get("name", ""))
        file_name = str(pack.get("file", ""))
        url = str(pack.get("url", ""))

        _require(ID_RE.fullmatch(pack_id) is not None, f"pack {index}: invalid id {pack_id!r}")
        _require(pack_id not in seen_ids, f"pack {index}: duplicate id {pack_id!r}")
        seen_ids.add(pack_id)
        _require(kind in ("fallback", "emoji"), f"pack {index}: kind must be fallback or emoji")
        if kind == "fallback":
            _require(FAMILY_RE.fullmatch(family) is not None, f"pack {index}: invalid family {family!r}")
            _require(family not in seen_families, f"pack {index}: duplicate family {family!r}")
            seen_families.add(family)
        else:
            _require(family == "-", f"pack {index}: emoji family must be '-'")
        _require(name and not any(ord(c) < 0x20 or ord(c) == 0x7F for c in name) and len(name.encode()) <= 64,
                 f"pack {index}: invalid name")
        _require(FILE_RE.fullmatch(file_name) is not None and ".." not in file_name,
                 f"pack {index}: invalid file {file_name!r}")
        _require(file_name.lower().endswith((".ttf", ".otf", ".ttc")), f"pack {index}: file must be .ttf/.otf/.ttc")
        _require(URL_RE.fullmatch(url) is not None and not any(ord(c) <= 0x20 or ord(c) == 0x7F for c in url) and
                 url[8] != "/", f"pack {index}: URL must be https://")

        asset = assets_dir / file_name
        _require(asset.is_file(), f"pack {index}: missing asset {asset}")
        size = asset.stat().st_size
        _require(512 <= size <= MAX_PACK_BYTES, f"pack {index}: asset size {size} out of range")
        magic = asset.read_bytes()[:4]
        _require(magic in (b"\x00\x01\x00\x00", b"OTTO", b"ttcf", b"true", b"typ1"),
                 f"pack {index}: asset is not an sfnt")
        lines.append(
            "\t".join(
                [
                    "pack",
                    pack_id,
                    kind,
                    family,
                    name,
                    file_name,
                    str(size),
                    _sha256(asset),
                    url,
                ]
            )
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=Path, required=True, help="packs.json")
    parser.add_argument("--assets-dir", type=Path, required=True, help="directory containing the font files")
    parser.add_argument("--out", type=Path, help="write manifest-v1.txt here")
    parser.add_argument("--check", action="store_true", help="fail unless --out already matches the generated text")
    args = parser.parse_args(argv)

    try:
        manifest = build(args.spec, args.assets_dir)
    except ManifestError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.check:
        if args.out is None:
            print("error: --check requires --out", file=sys.stderr)
            return 1
        if not args.out.is_file() or args.out.read_text(encoding="utf-8") != manifest:
            print(f"error: {args.out} is stale", file=sys.stderr)
            return 1
        print(f"{args.out}: up to date")
        return 0

    if args.out is None:
        print(manifest, end="")
    else:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(manifest, encoding="utf-8")
        print(f"wrote {args.out} ({len(manifest)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
