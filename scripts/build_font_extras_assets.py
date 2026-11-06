#!/usr/bin/env python3
"""Build the optional Font Extras assets from Google's Noto sources.

Output (OFL 1.1):
  NotoEmoji-Full.ttf       full monochrome emoji, wght=400
  CPYOSCJK.ttf             Noto Sans SC: CJK Unified + Ext-A + kana
  CPYOSWorldScripts.ttf    merged Noto Sans KR/Thai/Arabic/Hebrew/Devanagari/Sans

The device does not use OpenType shaping, so GSUB/GPOS/GDEF are dropped along
with variable-font tables. That also makes the merged world font buildable with
fontTools' Merger (which cannot merge VarStore tables).

This script is deliberately separate from the firmware: the files are served
from GitHub Pages, never linked into app0 or the data partition. Re-run it only
when updating the published font set, then regenerate the FEXT1 manifest.
"""

from __future__ import annotations

import argparse
import os
import sys
import urllib.request
from pathlib import Path

from fontTools.merge import Merger
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

SOURCES = {
    "emoji": "https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf",
    "sc": "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf",
    "kr": "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf",
    "thai": "https://raw.githubusercontent.com/google/fonts/main/ofl/notosansthai/NotoSansThai%5Bwdth%2Cwght%5D.ttf",
    "arabic": "https://raw.githubusercontent.com/google/fonts/main/ofl/notosansarabic/NotoSansArabic%5Bwdth%2Cwght%5D.ttf",
    "hebrew": "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanshebrew/NotoSansHebrew%5Bwdth%2Cwght%5D.ttf",
    "devanagari": "https://raw.githubusercontent.com/google/fonts/main/ofl/notosansdevanagari/NotoSansDevanagari%5Bwdth%2Cwght%5D.ttf",
    "latin": "https://raw.githubusercontent.com/google/fonts/main/ofl/notosans/NotoSans%5Bwdth%2Cwght%5D.ttf",
}
CACHE_NAMES = {
    "emoji": "NotoEmoji-var.ttf",
    "sc": "NotoSansSC-var.ttf",
    "kr": "NotoSansKR-var.ttf",
    "thai": "NotoSansThai-var.ttf",
    "arabic": "NotoSansArabic-var.ttf",
    "hebrew": "NotoSansHebrew-var.ttf",
    "devanagari": "NotoSansDevanagari-var.ttf",
    "latin": "NotoSans-var.ttf",
}
KEEP_TABLES = {"head", "hhea", "maxp", "OS/2", "hmtx", "cmap", "loca", "glyf", "name", "post"}
WORLD_KEYS = ("kr", "thai", "arabic", "hebrew", "devanagari", "latin")


def download(cache: Path, no_download: bool) -> None:
    cache.mkdir(parents=True, exist_ok=True)
    for key, url in SOURCES.items():
        path = cache / CACHE_NAMES[key]
        if path.is_file() and path.stat().st_size > 512:
            continue
        if no_download:
            sys.exit(f"error: missing cached source {path}")
        print(f"download {url}")
        urllib.request.urlretrieve(url, path)


def instantiate(source: Path, dest: Path) -> None:
    font = TTFont(source)
    location = {}
    if "fvar" in font:
        tags = {axis.axisTag for axis in font["fvar"].axes}
        if "wght" in tags:
            location["wght"] = 400
        if "wdth" in tags:
            location["wdth"] = 100
    if location:
        font = instantiateVariableFont(font, location, inplace=False, optimize=True)
    for tag in list(font.keys()):
        if tag != "GlyphOrder" and tag not in KEEP_TABLES:
            del font[tag]
    font.save(dest)


def cmap_of(path: Path) -> set[int]:
    font = TTFont(path, lazy=True)
    result: set[int] = set()
    for table in font["cmap"].tables:
        result.update(table.cmap)
    return result


def require_coverage(path: Path, codepoints: dict[int, str]) -> None:
    cmap = cmap_of(path)
    missing = [name for cp, name in codepoints.items() if cp not in cmap]
    if missing:
        sys.exit(f"error: {path.name} lacks {', '.join(missing)}")


def build(cache: Path, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    work = cache / "static"
    work.mkdir(exist_ok=True)
    for key in SOURCES:
        instantiate(cache / CACHE_NAMES[key], work / f"{key}.ttf")

    emoji_out = out / "NotoEmoji-Full.ttf"
    (work / "emoji.ttf").replace(emoji_out)
    require_coverage(emoji_out, {0x1F600: "grinning", 0x1F3E0: "house", 0x1F431: "cat"})

    cjk_out = out / "CPYOSCJK.ttf"
    (work / "sc.ttf").replace(cjk_out)
    require_coverage(cjk_out, {0x4E00: "CJK", 0x3400: "Ext-A", 0x3042: "kana", 0x9F98: "rare CJK"})

    world_out = out / "CPYOSWorldScripts.ttf"
    merged = Merger().merge([str(work / f"{key}.ttf") for key in WORLD_KEYS])
    for tag in list(merged.keys()):
        if tag != "GlyphOrder" and tag not in KEEP_TABLES:
            del merged[tag]
    merged.save(world_out)
    require_coverage(
        world_out,
        {
            0xAC00: "Hangul",
            0x0E01: "Thai",
            0x0627: "Arabic",
            0x05D0: "Hebrew",
            0x0905: "Devanagari",
            0x1EA1: "Vietnamese",
        },
    )

    for path in (emoji_out, cjk_out, world_out):
        print(f"{path.name}: {path.stat().st_size} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, required=True, help="downloaded Noto variable fonts")
    parser.add_argument("--out-dir", type=Path, required=True, help="published asset directory")
    parser.add_argument("--no-download", action="store_true", help="require the cache to be populated")
    args = parser.parse_args()
    download(args.cache_dir, args.no_download)
    build(args.cache_dir, args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
