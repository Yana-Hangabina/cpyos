#!/usr/bin/env python3
"""Reader for build/data_manifest*.txt.

The manifest is the only place the data-partition map is written down once
(scripts/generate_data_manifest.py builds it from the partition CSV and the C++ loader
headers).  Anything that flashes or bundles a blob has to ask it, because the offsets move
when the partition layout moves -- Read Pico's XIP relayout moved every blob -- and the
reading core now has one file per tier, so a hardcoded filename can name the wrong font.
"""

from __future__ import annotations

import pathlib
import re
from typing import Optional

ROW = re.compile(r"^data\t(\w+)\t(0x[0-9a-fA-F]+)\t(0x[0-9a-fA-F]+)\t(\d+)\t([0-9a-fA-F]{64})\t(\S+)$")

def asset(name: str) -> Optional[str]:
    """The published name for a manifest role.

    There is no rename table any more: a blob is published under the build file
    name the manifest lists (``cjk_core_3500.dat`` for the XIP layout).  The pack
    list is fetched fresh from the release, so devices follow the current name;
    a separate alias could drift from what the bundle actually shipped, and a
    missing pack is skipped silently because data packs are optional.
    """
    return name


def rows(manifest: pathlib.Path) -> list[dict[str, object]]:
    """Every data row of a manifest, as dicts.

    Raises FileNotFoundError rather than returning an empty list: callers use this to
    decide what to write to a device, and a silently missing manifest would make them skip
    a blob instead of stopping.
    """
    out: list[dict[str, object]] = []
    for line in pathlib.Path(manifest).read_text(encoding="utf-8").splitlines():
        m = ROW.match(line)
        if not m:
            continue
        out.append({
            "name": m.group(1),
            "offset": int(m.group(2), 16),
            "address": int(m.group(3), 16),
            "size": int(m.group(4)),
            "sha256": m.group(5),
            "path": m.group(6),
        })
    return out


def find(manifest: pathlib.Path, name: str) -> Optional[dict[str, object]]:
    for row in rows(manifest):
        if row["name"] == name:
            return row
    return None


def blob(repo: pathlib.Path, manifest: pathlib.Path, name: str) -> Optional[pathlib.Path]:
    """The file a manifest names for one blob, or None when it describes nothing.

    A manifest generated for another target is not an error here -- the caller decides
    whether a missing row means "skip" or "stop" -- but a row whose file is absent *is*
    reported as None so nobody flashes a path that does not exist.
    """
    row = find(manifest, name)
    if row is None:
        return None
    path = pathlib.Path(repo) / str(row["path"])
    return path if path.is_file() else None
