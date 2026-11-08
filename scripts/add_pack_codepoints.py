#!/usr/bin/env python3
"""Merge missing Latin outlines from an OFL source into the system font pack.

The committed pack (`assets/fonts/CPYOSHanSansSC-Regular-8105.ttf`) is a Noto
Sans CJK SC subset: it carries 8,105 Han characters, ASCII, CJK punctuation,
Greek, Cyrillic and kana, but only a handful of Latin-1/Latin-Extended letters.
The Azerbaijani and Turkish UI strings need `Ə/ə ÇÖÜçöğİıŞş`, and a single
missing codepoint makes `GfxRenderer::resolveTextFontId` fall the whole line
back to the 8pt emergency face (the "small byline" regression).

This script appends the missing outlines - real GlyphSource outlines, never a
cmap alias to a lookalike - from a Latin Noto Sans face (SIL OFL 1.1, same
licence family as the CJK source). It writes the pack TTF in place; rebuild the
carriers and the font ids afterwards:

    python3 scripts/add_pack_codepoints.py --source /path/NotoSans-Regular.ttf
    python3 scripts/build_system_font_pack.py \
        --embedded-out assets/fonts/cpyos_han_sans.def
    python3 scripts/build_font_ids.py

`chars_extra.txt` lists what must be present; `chars_8105.txt` stays at exactly
8,105 Han characters because `test_system_font_pack.py` asserts that count.
Pass `--check` to verify coverage without writing (CI-friendly).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FONT = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-Regular-8105.ttf"
DEFAULT_CHARS = ROOT / "assets" / "fonts" / "chars_extra.txt"
# Largest glyph id a format-4 `cmap` subtable can address.
FORMAT4_GLYPH_LIMIT = 0xFFFF


def read_codepoints(path: Path) -> list[int]:
    """One character per line; `#` starts a comment line, blank lines ignored."""
    codepoints: list[int] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        text = line.split("#", 1)[0].strip()
        if not text:
            continue
        if len(text) != 1:
            raise SystemExit(f"{path.name}:{number}: expected one character, got {text!r}")
        codepoint = ord(text)
        if codepoint in codepoints:
            raise SystemExit(f"{path.name}:{number}: duplicate U+{codepoint:04X}")
        codepoints.append(codepoint)
    if not codepoints:
        raise SystemExit(f"{path.name}: no codepoints listed")
    return codepoints


def missing_codepoints(font, codepoints: list[int]) -> list[int]:
    cmap = font.getBestCmap()
    return [cp for cp in codepoints if cp not in cmap or cp not in font.getGlyphOrder()]


def add_codepoint(font, source, codepoint: int) -> str:
    """Append `codepoint` to `font` using `source`'s outline; return the glyph name."""
    source_cmap = source.getBestCmap()
    if codepoint not in source_cmap:
        raise SystemExit(
            f"U+{codepoint:04X} is missing from the Latin source; refusing to alias a lookalike"
        )
    source_name = source_cmap[codepoint]
    source_glyph = source["glyf"][source_name]
    if source_glyph.numberOfContours == 0 and not source_glyph.isComposite():
        raise SystemExit(f"U+{codepoint:04X} has an empty outline in the Latin source")

    font_upem = font["head"].unitsPerEm
    source_upem = source["head"].unitsPerEm
    if font_upem != source_upem:
        raise SystemExit(
            f"unitsPerEm differs (pack {font_upem}, source {source_upem}); "
            "add an explicit scale instead of assuming one"
        )

    name = f"uni{codepoint:04X}"
    glyph_order = font.getGlyphOrder()
    while name in glyph_order:  # synthetic names collide only on a re-run
        name += "_"
    glyph_order.append(name)

    # Decompose composites so the pack stays free of cross-font components.
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    recording = DecomposingRecordingPen(source.getGlyphSet())
    source.getGlyphSet()[source_name].draw(recording)
    pen = TTGlyphPen(None)
    recording.replay(pen)
    font["glyf"][name] = pen.glyph()

    advance, side_bearing = source["hmtx"][source_name]
    font["hmtx"].metrics[name] = (advance, side_bearing)

    for subtable in font["cmap"].tables:
        if not subtable.isUnicode() or subtable.format not in (4, 12):
            continue
        subtable.cmap[codepoint] = name
    return name


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--font", default=str(DEFAULT_FONT), help="pack TTF to extend in place")
    parser.add_argument("--source", required=True, help="Latin Noto Sans TTF (SIL OFL) providing the outlines")
    parser.add_argument("--chars", default=str(DEFAULT_CHARS), help="chars_extra.txt")
    parser.add_argument("--check", action="store_true", help="only verify coverage; do not write")
    args = parser.parse_args(argv)

    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("error: fontTools is required (pip install fonttools)", file=sys.stderr)
        return 2

    font_path = Path(args.font)
    source_path = Path(args.source)
    codepoints = read_codepoints(Path(args.chars))
    font = TTFont(font_path)
    source = TTFont(source_path)

    missing = missing_codepoints(font, codepoints)
    if args.check:
        if missing:
            print(
                "error: pack is missing " + " ".join(f"U+{cp:04X}" for cp in missing),
                file=sys.stderr,
            )
            return 1
        print(f"{font_path.relative_to(ROOT)} covers all {len(codepoints)} extra codepoints")
        return 0

    if not missing:
        print(f"{font_path.relative_to(ROOT)} already covers all {len(codepoints)} extra codepoints")
        return 0

    added: list[tuple[int, str]] = []
    for codepoint in missing:
        added.append((codepoint, add_codepoint(font, source, codepoint)))

    font["maxp"].numGlyphs = len(font.getGlyphOrder())
    font.save(font_path)
    for codepoint, name in added:
        print(f"added U+{codepoint:04X} ({chr(codepoint)}) as {name} from {source_path.name}")
    print(f"wrote {font_path.relative_to(ROOT)}: {len(added)} outline(s) merged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
