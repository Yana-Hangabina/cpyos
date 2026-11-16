#!/usr/bin/env python3
"""Build the tiered UI font assets.

Tier 0 (app0): one TrueType family covering every codepoint the system UI can
draw -- the 35 translation catalogs, hard-coded UI string literals, the lunar
calendar list, plus every non-Han glyph the source family already carried
(kana, Cyrillic, Greek, punctuation, arrows, maths).  Book text and rare
ideographs are NOT included; those come from the core pack.

Tier 0b (app0, small): a second carrier with the non-CJK UI codepoints the CJK
family cannot draw (Vietnamese, Czech/Polish/Turkish diacritics, Hebrew,
Arabic, ...).  It is registered as a supplement at the UI sizes, so a Czech
label picks this face while Chinese labels stay on the CJK face.

Tier 1 (data partition): the full 8,105-character core, compressed, released to
SD on first boot and used as the reader face.

All carriers use the same CPFZ container as scripts/build_system_font_pack.py:

  0x00 magic "CPFZ"
  0x04 uint16 version (1)
  0x06 uint16 header size (32)
  0x08 uint32 raw size
  0x0c uint32 raw CRC-32
  0x10 uint32 compressed size
  0x14 reserved[12]
  0x20 raw-deflate stream

Usage:
  python3 scripts/build_ui_font_subset.py
  python3 scripts/build_ui_font_subset.py --check
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import pathlib
import re
import struct
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
CJK_SOURCE = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-Regular-8105.ttf"
WORLD_SOURCE = ROOT / "site" / "fonts" / "CPYOSWorldScripts.ttf"
TRANSLATIONS = ROOT / "lib" / "I18n" / "translations"
ALMANAC = ROOT / "lib" / "EpdFont" / "scripts" / "cn_almanac_chars.txt"

UI_TTF = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-UI.ttf"
UI_DEF = ROOT / "assets" / "fonts" / "cpyos_han_sans.def"
UI_CHARS = ROOT / "assets" / "fonts" / "chars_ui.txt"
EXT_TTF = ROOT / "assets" / "fonts" / "CPYOSWorldScripts-UI.ttf"
EXT_DEF = ROOT / "assets" / "fonts" / "cpyos_ui_ext.def"
CORE_DAT = ROOT / "build" / "cjk_core.dat"

# Codepoints the CJK source family does not carry but the UI uses. The CJK
# zero (U+3007, used by the standby calendar and TXT chapter numerals) maps to
# the family's full-width circle, which is the same glyph shape.
ALIASES = {0x3007: (0x25CB, 0x0030)}

# Byline separator that some labels use in its full-width katakana form. It sits
# in the kana block, so the non-Han sweep intentionally excludes it; add it to
# the UI contract explicitly.
REQUIRED_UI = {0x30FB}

MAGIC = b"CPFZ"
VERSION = 1
HEADER_SIZE = 32

# CJK Unified Ideographs (+ Ext A), compatibility ideographs, CJK punctuation,
# kana and full-width forms stay on the CJK family. Everything else is a
# candidate for the world-script supplement.
CJK_RANGES = (
    (0x2E80, 0x2FFF),
    (0x3000, 0x303F),
    (0x3040, 0x30FF),
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xF900, 0xFAFF),
    (0xFF00, 0xFFEF),
    (0x20000, 0x3FFFF),
)


def is_cjk(cp: int) -> bool:
    return any(lo <= cp <= hi for lo, hi in CJK_RANGES)


def _translation_value(line: str) -> str | None:
    m = re.match(r'^[A-Za-z0-9_]+:\s*"(.*)"\s*$', line)
    if not m:
        return None
    return m.group(1).replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\')


def translation_codepoints() -> set[int]:
    cps: set[int] = set()
    for path in sorted(glob.glob(str(TRANSLATIONS / "*.yaml"))):
        for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
            value = _translation_value(line)
            if value:
                cps.update(ord(c) for c in value)
    return cps


def _strip_comments(text: str) -> str:
    """Remove // and /* */ comments so Chinese prose does not enter the set."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        c = text[i]
        if c in '"\'':
            q = c
            j = i + 1
            buf = [c]
            while j < n:
                if text[j] == "\\":
                    buf.append(text[j : j + 2])
                    j += 2
                    continue
                buf.append(text[j])
                if text[j] == q:
                    break
                j += 1
            out.append("".join(buf))
            i = j + 1
            continue
        i += 1
    return "".join(out)


# String literals that are data, not UI copy, and are drawn by a dedicated
# face: the EPUB entity table and the chess-piece sprites. Emoji have their own
# face too (src/EmojiFont.cpp), and a lone variation selector never renders ink.
EXTERNAL_FILES = (
    "lib/Epub/Epub/htmlEntities.cpp",
    "src/activities/apps/chinese-chess/",
)

# The port ships its own activities (power-off confirmation, OTA page, transfer
# pages) with hard-coded Chinese copy. Their glyphs must be in the UI subset
# too: without the reading core (no SD card, or a card that failed to mount)
# those screens have no other face to fall back to, and "确定要关机吗？" boxed
# the 吗 on the e4plus. Vendor/log trees are skipped, and managed components
# only contain third-party fixtures.
PORT_UI_SCAN = (
    ROOT / "ports" / "metalio_plus" / "components" / "cpyos_app",
    ROOT / "ports" / "metalio_plus" / "components" / "cpyos_ui",
    ROOT / "ports" / "metalio_plus" / "main",
)
PORT_SKIP_MARKERS = ("/managed_components/", "/vendor/", "/build/", "/build_", "/__pycache__/")


def is_external_glyph(cp: int) -> bool:
    return (
        0x1F000 <= cp <= 0x1FAFF
        or 0x2600 <= cp <= 0x27BF
        or cp in (0x200D, 0xFE0E, 0xFE0F)
    )


def hardcoded_codepoints() -> set[int]:
    """Non-ASCII codepoints inside C++ string literals in src/, lib/ and the port UI."""
    cps: set[int] = set()
    roots = [ROOT / "src", ROOT / "lib", *PORT_UI_SCAN]
    skipped = ("builtinFonts", "/I18n/", "\\I18n\\") + EXTERNAL_FILES + PORT_SKIP_MARKERS
    for root in roots:
        for path in root.rglob("*"):
            if path.suffix not in (".cpp", ".h"):
                continue
            text_path = str(path)
            if any(marker in text_path for marker in skipped):
                continue
            code = _strip_comments(path.read_text(encoding="utf-8", errors="ignore"))
            for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', code):
                for c in m.group(1):
                    cp = ord(c)
                    if cp > 0x7F and not is_external_glyph(cp):
                        cps.add(cp)
    return cps


def almanac_codepoints() -> set[int]:
    text = ALMANAC.read_text(encoding="utf-8")
    return {ord(c) for c in text if not c.isspace()}


def ui_codepoints() -> set[int]:
    cps = translation_codepoints() | hardcoded_codepoints() | almanac_codepoints() | REQUIRED_UI
    cps.discard(0x0A)
    cps.discard(0x0D)
    return cps


def _apply_aliases(font: TTFont) -> None:
    tables = [table for table in font['cmap'].tables if table.isUnicode()]
    present = set(font.getBestCmap())
    for target, sources in ALIASES.items():
        if target in present:
            continue
        for source in sources:
            glyph = next((table.cmap[source] for table in tables if source in table.cmap), None)
            if glyph is None:
                continue
            for table in tables:
                table.cmap[target] = glyph
            break


def _subset(src: pathlib.Path, cps: set[int], *, layout: bool = False,
            aliases: bool = False) -> bytes:
    from fontTools.subset import Options, Subsetter
    from fontTools.ttLib import TTFont

    font = TTFont(str(src))
    options = Options()
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.notdef_outline = True
    options.recalc_bounds = False
    options.hinting = False
    options.drop_tables = list(options.drop_tables) + ["DSIG"]
    if not layout:
        options.layout_features = []
        options.drop_tables = list(options.drop_tables) + ["GSUB", "GPOS", "GDEF"]
    subsetter = Subsetter(options=options)
    subsetter.populate(unicodes=sorted(cps))
    subsetter.subset(font)
    if aliases:
        _apply_aliases(font)
    buf = pathlib.Path("/tmp/_ui_font_subset.ttf")
    # Keep head.modified fixed: the default would make every run differ by a
    # byte and defeat --check and the committed-asset test.
    font.recalcTimestamp = False
    font.save(str(buf))
    data = buf.read_bytes()
    buf.unlink()
    return data


def _carrier(raw: bytes) -> bytes:
    crc = zlib.crc32(raw) & 0xFFFFFFFF
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = compressor.compress(raw) + compressor.flush()
    header = MAGIC + struct.pack("<HHIII", VERSION, HEADER_SIZE, len(raw), crc, len(packed)) + bytes(12)
    assert len(header) == HEADER_SIZE
    return header + packed


def build(*, core: bool = True, core_only: bool = False) -> dict[str, bytes]:
    if core_only:
        return {str(CORE_DAT): _carrier(CJK_SOURCE.read_bytes())}
    from fontTools.ttLib import TTFont

    cjk = TTFont(str(CJK_SOURCE))
    cjk_cmap = set(cjk.getBestCmap())
    world = TTFont(str(WORLD_SOURCE))
    world_cmap = set(world.getBestCmap())

    ui = ui_codepoints()
    non_han = {cp for cp in cjk_cmap if not is_cjk(cp)}
    ui_cjk = (ui | non_han) & cjk_cmap
    # U+3007 lives in the CJK punctuation block but the world font is the only
    # source that carries the standalone glyph; include it there too so coverage
    # does not depend on the alias below.
    ui_world = ({cp for cp in ui if not is_cjk(cp)} | {0x3007}) & world_cmap

    # Every UI codepoint must be drawable by one of the two faces.
    uncovered = ui - cjk_cmap - world_cmap
    if uncovered:
        raise SystemExit(
            "UI codepoints no face can draw: "
            + ", ".join(f"U+{cp:04X}" for cp in sorted(uncovered)[:20])
        )

    ui_ttf = _subset(CJK_SOURCE, ui_cjk, aliases=True)
    ui_def = _carrier(ui_ttf)
    ext_ttf = _subset(WORLD_SOURCE, ui_world)
    ext_def = _carrier(ext_ttf)

    outputs: dict[str, bytes] = {
        str(UI_TTF): ui_ttf,
        str(UI_DEF): ui_def,
        str(EXT_TTF): ext_ttf,
        str(EXT_DEF): ext_def,
        str(UI_CHARS): ("\n".join(chr(cp) for cp in sorted(ui_cjk)) + "\n").encode("utf-8"),
    }
    if core:
        outputs[str(CORE_DAT)] = _carrier(CJK_SOURCE.read_bytes())
    return outputs


def write(outputs: dict[str, bytes]) -> None:
    for name, data in outputs.items():
        path = pathlib.Path(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"{path.relative_to(ROOT)}: {len(data)} B  sha256={hashlib.sha256(data).hexdigest()[:16]}")


def check(outputs: dict[str, bytes]) -> int:
    failed = False
    for name, data in outputs.items():
        path = pathlib.Path(name)
        if not path.exists():
            print(f"MISSING {path.relative_to(ROOT)}")
            failed = True
            continue
        if path.read_bytes() != data:
            print(f"STALE {path.relative_to(ROOT)}; re-run scripts/build_ui_font_subset.py")
            failed = True
    if not failed:
        print("UI font assets are up to date")
    return 1 if failed else 0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--no-core", action="store_true")
    parser.add_argument("--core-only", action="store_true",
                        help="only rebuild build/cjk_core.dat (no fontTools subsets)")
    args = parser.parse_args()
    outputs = build(core=not args.no_core, core_only=args.core_only)
    if args.check:
        raise SystemExit(check(outputs))
    write(outputs)


if __name__ == "__main__":
    main()
