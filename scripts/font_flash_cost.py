#!/usr/bin/env python3
"""Report what each built-in font asset actually costs in the linked image.

Stage 3 of the 0.8.1 plan is "reclaim flash, family by family", and the last attempt at it was
wrong because it asked the wrong question: it grepped the sources for a symbol name and called
an absent name "unused". Built-in bitmap faces are reached through the fallback and preferred
binding tables (main.cpp, UITheme.cpp), not by their own names, so the source grep proved
nothing. The link map is not a proxy for reachability -- a section that survives
-ffunction-sections/--gc-sections is in the image and occupies flash, and one that does not
appear was never reachable from a kept section.

Usage:
  python3 scripts/font_flash_cost.py [.pio/build/<env>/firmware.map]
  python3 scripts/font_flash_cost.py --json

Two-line map entries are the reason this is a script rather than an awk one-liner: GNU ld puts
long section names on their own line and the address/size/object on the next one.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

# Symbol-name fragments identifying built-in font data. Kept as (label, pattern) so the output
# says which family a cost belongs to rather than dumping mangled names.
FAMILIES = (
    ('notosans_cjk (UI bitmaps)', r'notosans_cjk_\d+'),
    ('notosans (latin bitmaps)', r'(?<!cjk_)notosans_\d+'),
    ('ubuntu_12', r'ubuntu_12'),
    ('notoserif_title', r'notoserif_title'),
    ('chinese_chess_16', r'chinese_chess'),
    ('calculator fonts', r'calculator_\d+'),
    ('cn ui charset tables', r'cn_(?:i18n|common)_chars'),
)

SIZE_RE = re.compile(r'^\s+0x([0-9a-f]{1,16})\s+0x([0-9a-f]+)\s+(\S+)')


def parse_map(path: pathlib.Path) -> dict[str, list[dict[str, object]]]:
    """section-name -> [{bytes, object}], from a GNU ld map."""
    found: dict[str, list[dict[str, object]]] = {}
    pending: str | None = None
    in_discard = False
    with path.open(encoding='utf-8', errors='replace') as stream:
        for line in stream:
            if line.startswith('/DISCARD/'):
                in_discard = True
                continue
            if in_discard:
                # /DISCARD/ holds the linker script's own patterns, not gc-sections output;
                # treat nothing in it as image cost.
                if not line.startswith(' '):
                    in_discard = False
                continue
            stripped = line.strip()
            if stripped.startswith(('.rodata', '.data', '.bss')) and '0x' not in stripped:
                pending = stripped
                continue
            match = SIZE_RE.match(line)
            if match and pending:
                found.setdefault(pending, []).append(
                    {'bytes': int(match.group(2), 16), 'object': match.group(3)})
                pending = None
                continue
            if match and not stripped.startswith(' '):
                pending = None
            elif stripped and not stripped.startswith((' *', ' .', '0x', 'Memory Configuration',
                                                       'Linker script', 'Cross Reference',
                                                       '.text', '.data', '.bss', '.rodata')):
                pending = None
    return found


def summarise(sections: dict[str, list[dict[str, object]]]) -> list[dict[str, object]]:
    rows = []
    for label, pattern in FAMILIES:
        names = [n for n in sections if re.search(pattern, n)]
        total = sum(int(item['bytes']) for n in names for item in sections[n])
        rows.append({'family': label, 'sections': len(names), 'bytes': total,
                     'largest': max(((int(sections[n][0]['bytes']), n) for n in names),
                                    default=(0, None))[1]})
    return rows


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('map', nargs='?',
                        default='.pio/build/readpico_pdf_jjwxc/firmware.map')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args(argv)

    path = pathlib.Path(args.map)
    if not path.is_file():
        print(f'error: {path} not found; build the environment first '
              '(this reads the linker map, so it cannot estimate)', file=sys.stderr)
        return 1

    sections = parse_map(path)
    rows = summarise(sections)
    if args.json:
        print(json.dumps({'sections_scanned': len(sections), 'families': rows}, indent=2))
        return 0

    print(f'link map: {path}  ({len(sections)} named data sections)')
    for row in rows:
        if row['sections'] == 0:
            print(f"  {row['family']:26s} absent from the image (0 sections matched)")
            continue
        print(f"  {row['family']:26s} {row['bytes']:9,d} B in {row['sections']:2d} sections"
              f"   largest: {row['largest']}")
    print('\n"absent" means no kept section references it: deleting that header would reclaim '
          'nothing from this configuration, and its cost here is genuinely zero.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
