#!/usr/bin/env python3
"""Validate docs/releases/<version>.ota.json before the release build starts.

The packager validates the notes too, but it only runs after the firmware build
(~20 minutes).  0.8.4's first run failed there with "unsafe en release note"
(a '<' in one note), so a malformed note cost a whole run.  This is the same
validator, reachable in a second from the command line and from both workflows.

    python3 scripts/check_release_notes.py 0.8.4
"""
from __future__ import annotations

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import generate_ota_notes  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    version = argv[1]
    path = ROOT / "docs" / "releases" / f"{version}.ota.json"
    if not path.is_file():
        print(f"missing {path}", file=sys.stderr)
        return 1
    try:
        notes = generate_ota_notes.validate_notes(json.loads(path.read_text(encoding="utf-8")))
    except (ValueError, json.JSONDecodeError) as error:
        print(f"{path.name}: {error}", file=sys.stderr)
        return 1
    print(f"{path.name}: {len(notes['en'])} en, {len(notes['zh'])} zh notes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
