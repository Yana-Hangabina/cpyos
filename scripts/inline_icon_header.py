#!/usr/bin/env python3
"""Make checked-in, generated icon headers use inline const tables.

The FreeInk SDK icon generator (libs/assets/Icons/tools/gen_icons.py) emits
`static const` arrays and `static const freeink::Icon` descriptors. Because the
headers have `#pragma once`, every translation unit that includes one gets a
private copy of every bitmap; the image ends up with up to eight copies of the
same 48 px icon (measured: 25,680 duplicated bytes in readpico).

`inline const` keeps one copy per program while leaving the values, order,
types and visibility untouched. The SDK generator cannot be changed from this
repository (it is a submodule), so after regenerating listIcons.h or
listIcons48.h run this script on the result. scripts/generate_cpy_duck.py
already emits `inline const` itself.

Usage:
    python3 scripts/inline_icon_header.py [FILE ...]

With no arguments it rewrites the two Lucide list headers. Idempotent.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FILES = [
    ROOT / "src/components/icons/listIcons.h",
    ROOT / "src/components/icons/listIcons48.h",
]

NEEDLE = "static const "
REPLACEMENT = "inline const "


def rewrite(path: Path) -> int:
    text = path.read_text(encoding="utf-8")
    count = text.count(NEEDLE)
    if count == 0:
        print(f"{path}: already inline")
        return 0
    text = text.replace(NEEDLE, REPLACEMENT)
    if NEEDLE in text:
        raise SystemExit(f"{path}: 'static const' still present after rewrite")
    path.write_text(text, encoding="utf-8")
    print(f"{path}: rewrote {count} declarations")
    return count


def main(argv: list[str]) -> int:
    files = [Path(a) for a in argv] if argv else DEFAULT_FILES
    total = sum(rewrite(p) for p in files)
    print(f"total declarations converted: {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
