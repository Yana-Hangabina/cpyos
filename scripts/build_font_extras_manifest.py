#!/usr/bin/env python3
"""Build the FEXT1/FEXT2 manifest consumed by Settings -> Font Extras.

The firmware deliberately does not parse JSON for this: the manifest is a
small tab-separated text file, so the device parser and this generator can be
tested independently. Assets are *not* copied by this script; point `--assets-dir`
at the directory that will be published (site/fonts for Pages, or a release
asset directory) and the manifest records each file's size and SHA-256.

FEXT2 adds one trailing column, `tier`, saying what the pack covers (e.g. "8105",
or "-" for nothing claimed). **Ship the parser before the manifest**: firmware that
only knows FEXT1 rejects an unknown header outright, so publishing a v2 manifest early
turns "font extras available" into a silent "no updates" -- the same class of failure as
a moved data-pack offset. The default here stays v1 for exactly that reason, and a spec
that declares `tier` while asking for v1 is an error rather than a silently dropped field.
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
# Must stay in step with font_extras::isValidTier (src/FontExtrasFormat.cpp):
# same alphabet, and at most 7 characters because the device stores it in char[8].
TIER_RE = re.compile(r"^[A-Za-z0-9._-]{1,7}$")


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


def build(spec_path: Path, assets_dir: Path, fmt: str = "v1") -> str:
    _require(fmt in ("v1", "v2"), f"unknown format {fmt!r} (expected v1 or v2)")
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    packs = spec.get("packs")
    _require(isinstance(packs, list) and 0 < len(packs) <= 12, "packs must be a non-empty list (<=12)")

    lines = ["FEXT1" if fmt == "v1" else "FEXT2"]
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
        row = [
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
        tier = pack.get("tier")
        if tier is not None:
            tier = str(tier)
            _require(TIER_RE.fullmatch(tier) is not None, f"pack {index}: invalid tier {tier!r}")
        if fmt == "v1":
            # Dropping a declared field is how a manifest ends up claiming something its
            # publisher wrote but nobody reads. Ask for v2 instead.
            _require(tier is None, f"pack {index}: declares a tier; pass --format v2 to publish it")
        else:
            row.append(tier if tier is not None else "-")
        lines.append("\t".join(row))
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=Path, required=True, help="packs.json")
    parser.add_argument("--assets-dir", type=Path, required=True, help="directory containing the font files")
    parser.add_argument("--out", type=Path, help="write manifest-v1.txt here")
    parser.add_argument("--format", choices=("v1", "v2"), default="v1",
                        help="v2 adds the tier column; only publish it once firmware that "
                             "parses FEXT2 is out (older devices refuse an unknown header)")
    parser.add_argument("--check", action="store_true", help="fail unless --out already matches the generated text")
    args = parser.parse_args(argv)

    try:
        manifest = build(args.spec, args.assets_dir, args.format)
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
