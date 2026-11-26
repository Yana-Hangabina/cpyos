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

Tier 1 (data partition): the reading core, committed as
CPYOSHanSansSC-Core.ttf (GB2312 level-1 + every UI codepoint + ASCII), compressed
into build/cjk_core.dat, released to SD on first boot and used as the reader
face.  It is smaller than the 8,105 family because the XIP layout's data
partition only leaves ~768 KiB for the compressed carrier; rare characters stay
available through the downloadable font packs.

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
CORE_SOURCE = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-Core.ttf"
WORLD_SOURCE = ROOT / "site" / "fonts" / "CPYOSWorldScripts.ttf"
TRANSLATIONS = ROOT / "lib" / "I18n" / "translations"
ALMANAC = ROOT / "lib" / "EpdFont" / "scripts" / "cn_almanac_chars.txt"

UI_TTF = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-UI.ttf"
UI_DEF = ROOT / "assets" / "fonts" / "cpyos_han_sans.def"
UI_CHARS = ROOT / "assets" / "fonts" / "chars_ui.txt"
EXT_TTF = ROOT / "assets" / "fonts" / "CPYOSWorldScripts-UI.ttf"
EXT_DEF = ROOT / "assets" / "fonts" / "cpyos_ui_ext.def"
CORE_DAT = ROOT / "build" / "cjk_core.dat"


def core_tier_path(tier: str) -> pathlib.Path:
    """The blob for one specific tier.

    Two targets read the same SystemCoreFont offset but fit different tiers: the Read
    Pico XIP layout takes 3,500, the Plus layout takes 8,105. With a single filename the
    second manifest generated on a machine described a blob the first one had already
    overwritten, so flashing the Plus from an older manifest would write the smaller core
    at the Plus offset -- and the partition would still pass every header check, just with
    less Han coverage. Naming the artifact after its tier makes that impossible rather
    than merely detectable; CORE_DAT stays as an alias for the last one built.
    """
    return ROOT / "build" / f"cjk_core_{tier}.dat"

# Tier-1 reading core, by name of what it covers. The carrier is one file at one
# partition offset, but the partition is not the same size on every target: the
# Read Pico XIP layout has 2.0 MiB of spiffs behind five other blobs, which only
# the 3,500-glyph carrier fits, while the Plus layout's 3.125 MiB also holds the
# full 8,105. When both devices shared one hardcoded source the smaller tier was
# applied to the larger partition too, which silently reduced the Plus reader's
# Han coverage; so the tier is now an argument, chosen from the partition table by
# scripts/generate_data_manifest.py rather than assumed here.
CORE_TIERS = {"3500": CORE_SOURCE, "8105": CJK_SOURCE}
DEFAULT_CORE_TIER = "3500"

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
    # Deterministic bytes: a wall-clock timestamp in head/name would make two
    # runs (and CI vs the committed assets) differ without any glyph change.
    options.recalc_timestamp = False
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


def _carrier(raw: bytes, tier: str | None = None) -> bytes:
    """Wrap a TTF in the CPFZ container the device reads.

    ``tier`` is written into the header's reserved field as an ASCII label. The published
    blob name is the same for every tier (cjk_core.dat, because devices on 0.8.0 ask for
    that name), so without a label nothing downstream can tell which coverage a given set of
    bytes carries -- not the device, not exp/verify_device_packs.py, not the person holding
    the file. The device treats it as a label only; size, CRC and version still decide use.
    """
    crc = zlib.crc32(raw) & 0xFFFFFFFF
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = compressor.compress(raw) + compressor.flush()
    tag = bytes(12)
    if tier is not None:
        encoded = tier.encode("ascii")
        if len(encoded) > 8:
            raise SystemExit(f"core tier label {tier!r} does not fit the 8-byte field")
        tag = encoded.ljust(8, b"\0") + bytes(4)
    header = MAGIC + struct.pack("<HHIII", VERSION, HEADER_SIZE, len(raw), crc, len(packed)) + tag
    assert len(header) == HEADER_SIZE
    return header + packed


def core_outputs(tier: str) -> dict[str, bytes]:
    """The tiered blob plus the compatibility alias."""
    blob = core_carriers()[tier]
    return {str(core_tier_path(tier)): blob, str(CORE_DAT): blob}


def core_carriers() -> dict[str, bytes]:
    """Every tier's compressed carrier, largest coverage first.

    Both sources are committed TTFs and the container is plain raw-deflate, so this
    costs two zlib passes and needs no fontTools -- which is what lets the manifest
    generator measure the options before it commits to one.
    """
    return {tier: _carrier(path.read_bytes(), tier) for tier, path in CORE_TIERS.items()}


def build(*, core: bool = True, core_only: bool = False,
          core_tier: str = DEFAULT_CORE_TIER) -> dict[str, bytes]:
    if core_tier not in CORE_TIERS:
        raise SystemExit(f"unknown --core-tier {core_tier!r}; expected one of "
                         + ", ".join(sorted(CORE_TIERS)))
    if core_only:
        return core_outputs(core_tier)
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
        # The reader face tier is the caller's decision (see CORE_TIERS); a full
        # build defaults to the tier that fits the tightest shipping layout.
        outputs.update(core_outputs(core_tier))
    return outputs


def write(outputs: dict[str, bytes]) -> None:
    for name, data in outputs.items():
        path = pathlib.Path(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"{path.relative_to(ROOT)}: {len(data)} B  sha256={hashlib.sha256(data).hexdigest()[:16]}")


def _rel(path: pathlib.Path) -> str:
    """Repo-relative for readability, absolute when the path is outside the repo.

    A staleness report must never be the thing that crashes: relative_to() raises for any
    path outside ROOT, which is exactly what a caller passing an alternate --out dir does.
    """
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def carriers_only_check() -> int:
    """Verify the committed carriers without fontTools, and say what it did not cover.

    Neither CI (ci.yml installs only pioarduino) nor a bare checkout has fontTools, so the
    full `--check` was unrunnable there -- which meant the "committed .def matches the
    committed .ttf" guarantee no automated step enforced. The carrier comparison needs
    nothing but zlib and the committed TTFs, so it can run anywhere; the subset byte
    comparison genuinely cannot, and this function refuses to pretend otherwise: callers get
    a nonzero exit so a green result always means the whole check actually ran.
    """
    failed = False
    carriers = core_carriers()
    for tier in sorted(CORE_TIERS):
        path = core_tier_path(tier)
        if not path.exists():
            print(f"MISSING {_rel(path)}")
            failed = True
        elif path.read_bytes() != carriers[tier]:
            print(f"STALE {_rel(path)}; re-run scripts/generate_data_manifest.py")
            failed = True
    for ttf, carrier in ((UI_TTF, UI_DEF), (EXT_TTF, EXT_DEF)):
        if not ttf.exists() or not carrier.exists():
            print(f"MISSING {_rel(ttf)} or {_rel(carrier)}")
            failed = True
            continue
        if _carrier(ttf.read_bytes()) != carrier.read_bytes():
            print(f"STALE {_rel(carrier)}; it is not the compressed form of "
                  f"{_rel(ttf)} -- re-run scripts/build_ui_font_subset.py")
            failed = True
    if not failed:
        print("carriers match their committed TTFs "
              "(subset byte-comparison still needs fontTools)")
    return 1 if failed else 0


def check(outputs: dict[str, bytes]) -> int:
    failed = False
    carriers = core_carriers()
    for name, data in outputs.items():
        path = pathlib.Path(name)
        if not path.exists():
            print(f"MISSING {_rel(path)}")
            failed = True
            continue
        if name == str(core_tier_path(core_tier)):
            # Tiered names are unambiguous, so this is a byte-exact comparison again --
            # which it could not be while one file served two targets.
            if path.read_bytes() != carriers[core_tier]:
                print(f"STALE {_rel(path)}; re-run scripts/generate_data_manifest.py")
                failed = True
            continue
        if name == str(CORE_DAT):
            # The alias is whatever was built last on this machine; only require that it
            # is some current tier, never that it matches a particular target.
            if path.read_bytes() not in carriers.values():
                print(f"STALE {path.relative_to(ROOT)}; it is not the compressed form "
                      "of any tier in CORE_TIERS -- re-run scripts/generate_data_manifest.py")
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
    parser.add_argument("--core-tier", default=DEFAULT_CORE_TIER, choices=sorted(CORE_TIERS),
                        help="which reading core to compress into the carrier "
                             "(default: %(default)s, the tier the tightest layout fits)")
    parser.add_argument("--core-sizes", action="store_true",
                        help="print '<tier> <bytes>' for every core tier and exit without writing")
    parser.add_argument("--check-carriers", action="store_true",
                        help="verify only the carrier/asset pairs, which needs no fontTools")
    args = parser.parse_args()
    if args.check_carriers:
        raise SystemExit(carriers_only_check())
    if args.core_sizes:
        for tier, blob in sorted(core_carriers().items()):
            print(f"{tier} {len(blob)}")
        return
    try:
        outputs = build(core=not args.no_core, core_only=args.core_only, core_tier=args.core_tier)
    except ModuleNotFoundError as exc:
        if args.check and exc.name and exc.name.startswith("fontTools"):
            print(f"error: --check needs {exc.name}; it cannot say whether the subsets are "
                  "current. Run --check-carriers for the part that works without it, or "
                  "install fontTools. Exiting nonzero rather than reporting a partial pass.")
            raise SystemExit(1)
        raise
    if args.check:
        raise SystemExit(check(outputs))
    write(outputs)


if __name__ == "__main__":
    main()
