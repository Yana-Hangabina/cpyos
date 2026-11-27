#!/usr/bin/env python3
"""Stage the E-INK4 Plus data packs under target-scoped published names.

Why this exists (0.8.3 defect): both device workflows published their data
packs into one GitHub release under the manifest's plain build names.  Four
packs are byte-identical between the targets, but two are not:

    i18n_pack.dat   Read Pico 6e4a4441...   Plus 3d512953...   (same size!)
    web_pack.dat    Read Pico 9f74a214...   Plus a65ac776...

Same name, different bytes, last writer wins.  A hash-verifying installer then
refuses the pack, and an installer without verification writes the other
target's bytes -- the language pack is rejected at boot (English fallback) and
the web pack can land on its recovery page.  Both look like "the update
installed" until the device is used.

The fix is structural rather than a two-name exception list: *every* Plus data
pack is published as ``plus-<build name>``, so a Plus asset can never collide
with a Read Pico asset even if a third one diverges later.  Read Pico keeps the
plain names.

What this script guarantees, and what the release gate re-checks:

  * each source blob matches the size and SHA-256 in the repo manifest -- the
    manifest is generated from the packs, so a mismatch means the two targets'
    packs were mixed up (exactly the 0.8.3 failure);
  * the staged copy still matches that digest after copying;
  * the staged manifest names the staged files, so ``flash_plus_data.py`` and
    ``build_cpyos_site.py`` (which takes the basename of the last column) find
    the right bytes without a rename table.

    python3 scripts/plus_release_bundle.py --out dist/cpyos-plus/0.8.4
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import shutil
import sys

PREFIX = "plus-"


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def published_name(build_name: str, prefix: str = PREFIX) -> str:
    """The name a Plus pack travels under in the release."""
    return prefix + pathlib.Path(build_name).name


def data_rows(manifest: pathlib.Path) -> list[list[str]]:
    """Tab-split `data` rows; every other line is returned verbatim as a 1-item row."""
    rows = []
    for line in pathlib.Path(manifest).read_text(encoding="utf-8").splitlines():
        if line.startswith("data\t"):
            rows.append(line.split("\t"))
        else:
            rows.append([line])
    return rows


def collisions(plain: list[str], prefixed: list[str]) -> list[str]:
    """Names published by both targets. Any hit is the 0.8.3 defect returning."""
    return sorted(set(plain) & set(prefixed))


def verify_rows(rows: list[list[str]], root: pathlib.Path) -> list[tuple[list[str], pathlib.Path]]:
    """[(row, source path)] after verifying every data row against its file."""
    problems = []
    checked: list[tuple[list[str], pathlib.Path]] = []
    for row in rows:
        if len(row) < 7:
            continue
        role, size, digest, relative = row[1], int(row[4]), row[5].lower(), row[6]
        source = root / relative
        if not source.is_file():
            problems.append(f"{role}: {source} is missing")
            continue
        actual_size = source.stat().st_size
        if actual_size != size:
            problems.append(f"{role}: {relative} is {actual_size} B but the manifest says {size} B")
            continue
        actual = sha256(source)
        if actual != digest:
            problems.append(
                f"{role}: {relative} hashes {actual[:16]}... but the manifest says {digest[:16]}... "
                "(the packs and the manifest came from different builds/targets)")
            continue
        checked.append((row, source))
    if problems:
        raise SystemExit("Plus data bundle refused:\n  " + "\n  ".join(problems))
    if not checked:
        raise SystemExit("the manifest lists no data rows; refusing to stage an empty bundle")
    return checked


def check_sources(manifest: pathlib.Path, root: pathlib.Path) -> list[tuple[list[str], pathlib.Path]]:
    return verify_rows(data_rows(manifest), root)


def bundle(manifest: pathlib.Path, out_dir: pathlib.Path, root: pathlib.Path,
           prefix: str = PREFIX) -> list[pathlib.Path]:
    """Copy the verified blobs under target-scoped names and rewrite the manifest."""
    rows = data_rows(manifest)
    checked = verify_rows(rows, root)
    by_row = {id(row): path for row, path in checked}
    out_dir.mkdir(parents=True, exist_ok=True)
    staged: list[pathlib.Path] = []
    lines: list[str] = []
    for row in rows:
        if len(row) < 7:
            lines.append(row[0])
            continue
        source = by_row[id(row)]
        target = out_dir / published_name(row[6], prefix)
        shutil.copyfile(source, target)
        if sha256(target) != row[5].lower():
            raise SystemExit(f"{target.name} changed while copying; refusing the bundle")
        row = row[:]
        row[6] = published_name(row[6], prefix)
        lines.append("\t".join(row))
        staged.append(target)
    (out_dir / manifest.name).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return staged


def main(argv: list[str] | None = None) -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", type=pathlib.Path,
                        default=root / "build" / "data_manifest_plus.txt")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--root", type=pathlib.Path, default=root,
                        help="where the manifest's relative paths resolve (default: repo root)")
    parser.add_argument("--check-only", action="store_true",
                        help="verify the manifest against the files without staging")
    args = parser.parse_args(argv)
    if args.check_only:
        for row, _ in check_sources(args.manifest, args.root):
            print(f"ok  {row[1]}  {row[6]}  {row[5][:16]}...")
        return 0
    staged = bundle(args.manifest, args.out, args.root)
    for path in staged:
        print(f"staged {path.name} ({path.stat().st_size} B)")
    print(f"manifest: {args.out / args.manifest.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
